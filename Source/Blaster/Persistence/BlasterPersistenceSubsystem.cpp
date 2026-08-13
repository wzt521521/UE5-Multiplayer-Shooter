#include "BlasterPersistenceSubsystem.h"

#include "SQLiteMatchStatsStore.h"
#include "PersistenceWorker.h"

// sqlite3 C API（BlasterDumpStats 用独立只读连接打印记录，不碰 worker 独占的连接）
#include "IncludeSQLite.h"
#include "Containers/StringConv.h"
#include "Engine/Engine.h"            // GEngine
#include "HAL/Event.h"               // FEvent
#include "HAL/IConsoleManager.h"     // FAutoConsoleCommand
#include "HAL/PlatformProcess.h"     // FPlatformProcess::Sleep / GetSynchEventFromPool
#include "HAL/PlatformTLS.h"         // FPlatformTLS::GetCurrentThreadId
#include "HAL/RunnableThread.h"      // FRunnableThread::Create
#include "Misc/App.h"                // IsRunningDedicatedServer
#include "Misc/Paths.h"              // FPaths

// sqlite3_exec 行回调：把每行拼成 "列=值 | 列=值 |" 追加到 UserData(FString)
static int DumpStatsCallback(void* UserData, int ColumnCount, char** ColumnValues, char** ColumnNames)
{
	FString* Out = static_cast<FString*>(UserData);
	for (int i = 0; i < ColumnCount; ++i)
	{
		Out->Appendf(TEXT("%s=%s | "),
			UTF8_TO_TCHAR(ColumnNames[i]),
			ColumnValues[i] ? UTF8_TO_TCHAR(ColumnValues[i]) : TEXT("NULL"));
	}
	Out->Append(TEXT("\n"));
	return 0;
}

void UBlasterPersistenceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 只在该进程是 Dedicated Server 时启动写线程：
	// 客户端进程不运行 GameMode（没有比赛结算点），也避免在客户端机器建出无意义的 .db。
	if (!IsRunningDedicatedServer())
	{
		UE_LOG(LogTemp, Log, TEXT("[Persistence] 非 DS 进程，不启动持久化 worker"));
		return;
	}

	DbPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Persistence"), TEXT("BlasterStats.db"));
	UE_LOG(LogTemp, Log, TEXT("[Persistence] DS 检测通过 | db=%s"), *DbPath);

	// 存储层实例化——将来换 MySQL 只改这一行（连接池 + 异步驱动，队列/线程模型不变）
	Store = new SQLiteMatchStatsStore();

	WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);  // auto-reset 事件
	StopFlag.Store(false);

	Worker = new FPersistenceWorker(Store, DbPath, &Queue, &StopFlag, WakeEvent);//新建一个行动手册
	// TPri_BelowNormal：写盘线程低于游戏线程优先级，IO 不抢占玩法帧

	//把worker手册传给线程
	WorkerThread = FRunnableThread::Create(Worker, TEXT("BlasterPersistenceWorker"), 0, TPri_BelowNormal);
	bWorkerRunning = (WorkerThread != nullptr);

	// 服务端控制台命令：开独立只读连接打印全部记录（demo 不依赖 sqlite3.exe）。
	// 静态对象常驻进程，lambda 捕获 this（子系统存活于整个引擎生命周期）。
	static FAutoConsoleCommand DumpCmd(
		TEXT("BlasterDumpStats"),
		TEXT("打印已持久化的比赛/玩家记录"),
		FConsoleCommandDelegate::CreateLambda([this]() { DumpStats(); })
	);

	UE_LOG(LogTemp, Log, TEXT("[Persistence] Worker started | thread=%u"), FPlatformTLS::GetCurrentThreadId());
}

void UBlasterPersistenceSubsystem::Deinitialize()
{
	if (bWorkerRunning)
	{
		bWorkerRunning = false;

		// ① 置停止标志并唤醒 worker → worker 排空剩余队列后自行退出
		StopFlag.Store(true);
		WakeEvent->Trigger();

		// ② 先 join 线程再释放 store，保证 worker 不会碰到已释放的连接。
		// Kill(true)：调用 Stop() 并等待 Run() 排空退出（StopFlag 已置位，正常几百毫秒内完成；
		// 队列里至多一条比赛记录，Drain 近乎瞬时）。
		if (WorkerThread)
		{
			WorkerThread->Kill(true);
			delete WorkerThread; WorkerThread = nullptr;
		}
		delete Worker;  Worker = nullptr;
		delete Store;   Store = nullptr;

		FPlatformProcess::ReturnSynchEventToPool(WakeEvent); WakeEvent = nullptr;
	}

	Super::Deinitialize();
}

void UBlasterPersistenceSubsystem::EnqueueMatchResult(const FMatchResultRecord& Record)
{
	if (!bWorkerRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Persistence] worker 未运行，丢弃本场比赛记录（players=%d）"), Record.Players.Num());
		return;
	}

	// 入队 + 唤醒：O(记录大小)，不触碰 sqlite，绝不等待
	Queue.Enqueue(Record);
	WakeEvent->Trigger();

	UE_LOG(LogTemp, Log, TEXT("[Persistence] 比赛记录已入队 | players=%d | winner=%s | thread=%u"),
		Record.Players.Num(), *Record.WinnerTeam, FPlatformTLS::GetCurrentThreadId());
}

void UBlasterPersistenceSubsystem::DumpStats() const
{
	if (DbPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Persistence] DumpStats: DB 未初始化（非 DS 进程？）"));
		return;
	}

	// 独立只读连接（游戏线程使用），绝不触碰 worker 线程独占的连接 —— sqlite 允许并发读
	sqlite3* ReadDb = nullptr;
	if (sqlite3_open_v2(TCHAR_TO_UTF8(*DbPath), &ReadDb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Persistence] DumpStats: 打开 %s 失败"), *DbPath);
		if (ReadDb) sqlite3_close(ReadDb);
		return;
	}

	FString Rows;
	char* ErrMsg = nullptr;
	const int Rc = sqlite3_exec(ReadDb,
		"SELECT * FROM matches; SELECT * FROM player_stats;",
		DumpStatsCallback, &Rows, &ErrMsg);
	if (Rc != SQLITE_OK)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Persistence] DumpStats: 查询失败 %s"), ErrMsg ? UTF8_TO_TCHAR(ErrMsg) : TEXT("?"));
		if (ErrMsg) sqlite3_free(ErrMsg);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[Persistence] === DB Dump ===\n%s"), *Rows);
	}

	sqlite3_close(ReadDb);
}

UBlasterPersistenceSubsystem* UBlasterPersistenceSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UBlasterPersistenceSubsystem>() : nullptr;
}
