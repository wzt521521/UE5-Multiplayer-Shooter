// SSR：帧历史录制器实现

#include "SSR_FrameHistory.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h" // UNetDriver::NetServerMaxTickRate（服务器 Tick 频率权威来源）

// ════════════════════════════════════════════════════════════════
// Console Variables：运行时通过控制台 ~ 调节 SSR 行为
// ════════════════════════════════════════════════════════════════

//ECVF_Default 是 CVar 的行为标志，含义就是"无特殊标志"——最普通的默认行为。
TAutoConsoleVariable<int32> CVarSSREnabled(
	TEXT("ssr.Enabled"),//总开关：0=关 SSR，1=开
	1,
	TEXT("Server-Side Rewind 延迟补偿\n0=禁用  1=启用"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarSSRMaxHistorySeconds(
	TEXT("ssr.MaxHistorySeconds"),//录多久历史：0.5s≈30帧@60Hz，覆盖 ~250ms ping
	0.5f,
	TEXT("历史快照保留时长（秒）\n0.5s ≈ 30帧 @60Hz，覆盖 ~250ms Ping"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarSSRMaxPingCompensation(
	TEXT("ssr.MaxPingCompensation"),//回退窗口上限：单向延迟超 0.25s 不回退，走当前帧
	0.25f,
	TEXT("最大 Ping 补偿上限（秒）\n超过此值的单向延迟不回退，直接走当前帧射线"),
	ECVF_Default
);

// ⚠ [已废弃-死代码] 双保险开关：全项目从未被读取（grep 仅定义 + extern 声明）。
// 设计意图：回退帧命中 OR 当前帧命中都算命中（0=只用回退帧，1=回退或当前任一命中即算）。
// 实际双保险由"SSR miss → 落到 MulticastFire → 旧路径当前帧射线"无条件实现
//   （bSSRHandledShot 门控防二次结算），无需此开关。
// 保留仅作演进痕迹，勿用于新逻辑；如需删除，同步清理头文件 extern 声明。
TAutoConsoleVariable<int32> CVarSSRValidateWithCurrent(
	TEXT("ssr.ValidateWithCurrent"),//双保险：回退帧或当前帧命中都算
	1,
	TEXT("双保险：回退帧命中或当前帧命中都算命中\n0=只用回退帧  1=回退或当前任一命中即算"),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarSSRDrawDebug(
	TEXT("ssr.DrawDebug"),//调试可视化：0=关，1=画回退射线，2=加画胶囊体
	0,
	TEXT("SSR Debug 可视化\n0=关闭  1=绘制回退射线  2=绘制射线+胶囊体线框"),
	ECVF_Default
);

// 视线复核（防隔墙报点）：命中点被墙挡住时拒绝该命中
TAutoConsoleVariable<int32> CVarSSROcclusionEnabled(
	TEXT("ssr.OcclusionEnabled"),
	1,
	TEXT("SSR 视线复核（防隔墙报点）\n0=关闭  1=启用"),
	ECVF_Default
);

// 双原点遮挡：叠加"服务器武器枪口"做第二路遮挡判定（堵客户端伪造枪口到墙另一侧）
TAutoConsoleVariable<int32> CVarSSROcclusionServerMuzzle(
	TEXT("ssr.OcclusionServerMuzzle"),
	1,
	TEXT("SSR 视线复核第二路：服务器枪口\n0=只用客户端枪口  1=客户端+服务器双路"),
	ECVF_Default
);

// SSR 判定分析日志（高延迟场景验证用，上线前关闭）：0=关闭 1=开启
// 在 ProcessHitScanShot 里每枪输出一条"回退帧 vs 当前帧"对比日志，
// 用于量化延迟补偿有效性（rewindHit=true & currentHit=false & 位移>0 ⇒ SSR 价值成立）
TAutoConsoleVariable<int32> CVarSSRAnalysisLog(
	TEXT("ssr.AnalysisLog"),
	1,
	TEXT("SSR 判定分析日志（验证延迟补偿用，上线前关掉）\n0=关闭  1=每枪输出回退帧 vs 当前帧对比"),
	ECVF_Default
);

// ════════════════════════════════════════════════════════════════
// 静态骨骼列表：UE5 Mannequin 标准碰撞相关骨骼
// 只追踪对命中判定有意义的骨骼，不追踪 IK 骨骼和末端效应器
// ════════════════════════════════════════════════════════════════

TArray<FName> USSR_FrameHistory::RelevantBoneNames;

void USSR_FrameHistory::BuildRelevantBoneList()
{
	if (RelevantBoneNames.Num() > 0) return; // 已构建，跳过

	// 头部 + 躯干链（爆头判定 + 身体命中判定核心）
	RelevantBoneNames.Add(FName("head"));
	RelevantBoneNames.Add(FName("neck_01"));
	RelevantBoneNames.Add(FName("spine_01"));
	RelevantBoneNames.Add(FName("spine_02"));
	RelevantBoneNames.Add(FName("spine_03"));
	RelevantBoneNames.Add(FName("pelvis"));

	// 左臂链（持枪手）
	RelevantBoneNames.Add(FName("upperarm_l"));
	RelevantBoneNames.Add(FName("lowerarm_l"));

	// 右臂链（扳机手）
	RelevantBoneNames.Add(FName("upperarm_r"));
	RelevantBoneNames.Add(FName("lowerarm_r"));

	// 左腿链
	RelevantBoneNames.Add(FName("thigh_l"));
	RelevantBoneNames.Add(FName("calf_l"));

	// 右腿链
	RelevantBoneNames.Add(FName("thigh_r"));
	RelevantBoneNames.Add(FName("calf_r"));
}

// ════════════════════════════════════════════════════════════════
// 初始化：根据 CVar + 服务器 Tick Rate 计算环形缓冲区容量
// ════════════════════════════════════════════════════════════════

void USSR_FrameHistory::Initialize(ABlasterGameState* InGameState)
{
	GameState = InGameState;

	// 从 NetServerMaxTickRate 获取服务器 Tick 频率（权威来源：UNetDriver 的 config 属性）
	// 项目 DefaultEngine.ini 的 [/Script/OnlineSubsystemUtils.IpNetDriver] 设为 60
	// NetServerMaxTickRate=60 → 每秒 60 帧 → 0.5s 历史 = 30 帧
	float TickRate = 60.f; // 兜底默认值（无 NetDriver 时）
	if (UWorld* World = GetWorld())
	{
		//按服务器实际发包频率计算环形缓冲容量，改 NetServerMaxTickRate 不会算错。
		if (const UNetDriver* NetDriver = World->GetNetDriver())
		{
			TickRate = FMath::Max(20.f, (float)NetDriver->NetServerMaxTickRate);
		}
	}

	// MaxHistorySeconds "读"录多久历史"的 CVar
	const float MaxHistorySeconds = CVarSSRMaxHistorySeconds.GetValueOnGameThread();
	MaxCapacity = FMath::Max(1, FMath::CeilToInt(TickRate * MaxHistorySeconds));//CeilToInt向上取整

	// 预分配环形缓冲区内存，避免运行时动态扩容
	// TArray<FSSR_FrameSnapshot> RingBuffer本质是线状，
	// 但逻辑上是环形的：HeadIndex指针循环推进，FrameCounter递增不回绕
	RingBuffer.SetNum(MaxCapacity);

	// 构建骨骼追踪列表（全局共享，只执行一次）
	BuildRelevantBoneList();

	UE_LOG(LogTemp, Log, TEXT("[SSR] FrameHistory initialized: MaxCapacity=%d frames (%.2fs @ %.0fHz) | %d bones tracked"),
		MaxCapacity, MaxHistorySeconds, TickRate, RelevantBoneNames.Num());
}

// ════════════════════════════════════════════════════════════════
// 每帧录制：遍历所有 PlayerController → 有效 Pawn → 拍快照 → 写入环形缓冲区
// 必须在所有角色移动完成后调用（GameState::Tick 末尾）
// ════════════════════════════════════════════════════════════════

void USSR_FrameHistory::RecordFrame()
{
	if (!CVarSSREnabled.GetValueOnGameThread()) return;//总开关

	UWorld* World = GetWorld();
	if (!World) return;

	// 定位环形缓冲区当前写入槽位
	FSSR_FrameSnapshot& CurrentSnapshot = RingBuffer[HeadIndex]; //定位槽位：拿到 HeadIndex 指向的那个帧快照（引用！）
	CurrentSnapshot.PlayerEntries.Reset(); //清空旧玩家数据（这个槽上一轮用过，得先清）
	CurrentSnapshot.Timestamp = World->GetTimeSeconds();// 记录当前服务器时间戳（秒）
	CurrentSnapshot.FrameNumber = CurrentFrameNumber;// 记录当前全局帧号，调试用

	// 遍历所有 PlayerController 获取有效玩家角色
	for (auto It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC) continue;

		ABlasterCharacter* BlasterChar = Cast<ABlasterCharacter>(PC->GetPawn());
		// 跳过无效 Pawn / 已死亡角色 / 等待下回合的玩家
		if (!BlasterChar || BlasterChar->IsElimmed()) continue;

		FSSR_PlayerFrameEntry& Entry = CurrentSnapshot.PlayerEntries.AddDefaulted_GetRef();
		CapturePlayerEntry(BlasterChar, Entry);// 录制玩家碰撞体状态
	}

	// 推进环形缓冲区指针
	HeadIndex = (HeadIndex + 1) % MaxCapacity;
	FrameCounter++;
	CurrentFrameNumber++;

	// 前 5 帧 + 每 60 帧输出录制状态，用 Log 级别确保能看到
	if (CurrentFrameNumber <= 5 || CurrentFrameNumber % 60 == 0)
	{
		const int32 Count = GetSnapshotCount();
		UE_LOG(LogTemp, Log, TEXT("[SSR] FrameHistory | Frame #%d | Snapshot pool: %d/%d | %d players recorded"),
			CurrentFrameNumber, Count, MaxCapacity, CurrentSnapshot.PlayerEntries.Num());
	}
}

// ════════════════════════════════════════════════════════════════
// 单个玩家碰撞体快照：胶囊体 + 关键骨骼的世界空间 Transform
// ════════════════════════════════════════════════════════════════

void USSR_FrameHistory::CapturePlayerEntry(ABlasterCharacter* Player, FSSR_PlayerFrameEntry& OutEntry) const
{
	OutEntry.Character = Player;

	// 1. 胶囊体碰撞体
	const UCapsuleComponent* Capsule = Player->GetCapsuleComponent();
	if (Capsule)
	{
		OutEntry.CapsuleLocation   = Capsule->GetComponentLocation();
		OutEntry.CapsuleRotation   = Capsule->GetComponentQuat();
		OutEntry.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		OutEntry.CapsuleRadius     = Capsule->GetScaledCapsuleRadius();
	}

	// 2. 关键骨骼的世界空间 Transform
	OutEntry.BoneSnapshots.Reset();
	USkeletalMeshComponent* Mesh = Player->GetMesh();
	if (!Mesh) return;

	const FTransform MeshWorldTM = Mesh->GetComponentTransform();

	// [已废弃] 物理回退遗留：纯数学判定不消费这两字段，保留写入仅作记录。见 SSRTypes.h 同注释。
	OutEntry.MeshWorldLocation = MeshWorldTM.GetLocation();
	OutEntry.MeshWorldRotation = MeshWorldTM.GetRotation();

	for (const FName& BoneName : RelevantBoneNames)
	{
		const int32 BoneIndex = Mesh->GetBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE) continue;

		// GetBoneTransform 返回 Component Space → 乘上 Mesh 的 World Transform 得世界空间
		const FTransform BoneCS = Mesh->GetBoneTransform(BoneIndex);
		const FTransform BoneWS = BoneCS * MeshWorldTM;

		FSSR_BoneSnapshot BoneSnapshot;
		BoneSnapshot.BoneName = BoneName;
		BoneSnapshot.Location = BoneWS.GetLocation();
		BoneSnapshot.Rotation = BoneWS.GetRotation();

		OutEntry.BoneSnapshots.Add(BoneSnapshot);
	}
}

// ════════════════════════════════════════════════════════════════
// 线性查找：在环形缓冲区中查找 Timestamp ≤ TargetTime 的最新快照
// 返回 nullptr 表示历史不足
// （容量仅 ~30 帧，线性遍历比二分更快，且无需处理环形回绕的索引映射）
// ════════════════════════════════════════════════════════════════

const FSSR_FrameSnapshot* USSR_FrameHistory::FindSnapshot(float TargetTime) const
{
	const int32 Count = GetSnapshotCount();// 环形缓冲区有效条目数
	if (Count == 0) return nullptr;

	// 环形缓冲区中最早帧的索引
	const int32 OldestIndex = (FrameCounter > MaxCapacity)
		? HeadIndex                          // 缓冲区已满：最老的槽就是 HeadIndex 本身（它指向下一个将被覆盖的位置）
		: 0;                                  // 缓冲区未满：最老在索引 0（按顺序写入的）

	// 环形缓冲时间顺序：从 OldestIndex 沿环形走到 HeadIndex-1，时间严格递增
	// 满了之后 HeadIndex 指向【最老】槽（下一个将被覆盖），HeadIndex-1 指向【最新】槽
	// 回绕导致"物理索引 0"不一定最老 → 最老索引由上面的 OldestIndex 决定
	// （查找是线性扫描：容量仅 ~30 帧，见下方实现，非二分）

	// 简化方案：先检查最早和最晚的边界
	const FSSR_FrameSnapshot& OldestSnap = RingBuffer[OldestIndex];
	const int32 NewestPhysIndex = (HeadIndex == 0) ? MaxCapacity - 1 : HeadIndex - 1;
	const FSSR_FrameSnapshot& NewestSnap = RingBuffer[NewestPhysIndex];

	// 目标时间比最早快照还早 → 无可用历史
	if (TargetTime < OldestSnap.Timestamp) return nullptr;

	// 目标时间比最新快照还晚 → 返回最新（极低延迟的情况）
	if (TargetTime >= NewestSnap.Timestamp) return &NewestSnap;

	// 线性查找（环形缓冲区只有 ~30 帧，没必要二分，循环遍历即可）
	// 找 Timestamp ≤ TargetTime 且 Timestamp 最大的帧
	const FSSR_FrameSnapshot* BestMatch = nullptr;
	for (int32 i = 0; i < Count; i++)
	{
		const int32 PhysIdx = (OldestIndex + i) % MaxCapacity;
		const FSSR_FrameSnapshot& Snap = RingBuffer[PhysIdx];

		if (Snap.Timestamp <= TargetTime)
		{
			BestMatch = &Snap;
		}
		else
		{
			break; // 时间递增，之后的帧都大于 TargetTime
		}
	}

	return BestMatch;
}
