#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "Templates/Atomic.h"   // TAtomic<bool>
#include "MatchResultRecord.h"

class IMatchStatsStore;
class FEvent;

/**
 * P4 玩家数据持久化 —— 异步写线程（FRunnable）。简历点本体。
 *
 * 设计意图（WHY）：
 * - 游戏线程（结算点）只做"入队 + Trigger"、绝不等待；真正的 DB IO 在本线程完成，
 *   因此写盘耗时不影响游戏帧。worker 独占一个 SQLite 连接（线程亲和），
 *   且本类不拥有任何资源——store/queue/stopflag/event 全由 UBlasterPersistenceSubsystem
 *   持有，本类只借用；子系统先 join 线程再释放资源，杜绝野指针。
 *
 * 模块配合（HOW）：
 * - 生产：UBlasterPersistenceSubsystem::EnqueueMatchResult()（游戏线程）→ Queue + Trigger。
 * - 消费：本线程 Run() 主循环 Dequeue → Store->WriteMatchResult()。
 * - 停止：子系统 Deinitialize 置 StopFlag 并 Trigger → worker 排空后 Close 退出。
 */
class FPersistenceWorker : public FRunnable
{
public:
	FPersistenceWorker(IMatchStatsStore* InStore, const FString& InConnectionSpec,
	                   TQueue<FMatchResultRecord, EQueueMode::Mpsc>* InQueue,
	                   TAtomic<bool>* InStopFlag, FEvent* InWakeEvent);

	virtual bool Init() override;
	virtual uint32 Run() override;   // 主循环：建连接 → 等/取 → 写 → 排空退出
	virtual void Stop() override;
	virtual void Exit() override;

private:
	// 单条记录写库 + 日志（打印线程 ID，与入队日志线程 ID 不同 = 异步 IO 的可视证明）
	void WriteAndLog(const FMatchResultRecord& Record);

	IMatchStatsStore* Store;
	FString           ConnectionSpec;
	TQueue<FMatchResultRecord, EQueueMode::Mpsc>* Queue;
	TAtomic<bool>*    StopFlag;//一个线程安全的布尔开关
	FEvent*           WakeEvent;
};
