// Blaster SSR：帧历史录制器
// 托管于 BlasterGameState。服务器每帧录制一份原子世界快照（所有玩家碰撞体）
// 存入定长环形缓冲区，提供按时间戳的二分查找查询接口

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "SSRTypes.h"
#include "SSR_FrameHistory.generated.h"

class ABlasterGameState;
class ABlasterCharacter;

// ────────────────────────────────────────────────────────────
// Console Variables（定义在 .cpp 中，此处为全局 extern 声明）
// ────────────────────────────────────────────────────────────
extern TAutoConsoleVariable<int32> CVarSSREnabled;
extern TAutoConsoleVariable<float> CVarSSRMaxHistorySeconds;
extern TAutoConsoleVariable<float> CVarSSRMaxPingCompensation;
extern TAutoConsoleVariable<int32> CVarSSRValidateWithCurrent; // [已废弃-死代码] 见 .cpp 同注释，从未被读取
extern TAutoConsoleVariable<int32> CVarSSRDrawDebug;
extern TAutoConsoleVariable<int32> CVarSSROcclusionEnabled;
extern TAutoConsoleVariable<int32> CVarSSROcclusionServerMuzzle;
extern TAutoConsoleVariable<int32> CVarSSRAnalysisLog;

UCLASS()
class BLASTER_API USSR_FrameHistory : public UObject
{
	GENERATED_BODY()

public:
	// ── 初始化：计算环形缓冲区容量 + 构建追踪骨骼列表 ──
	void Initialize(ABlasterGameState* InGameState);

	// ── 服务器每帧调用：拍摄当前帧所有角色的碰撞体快照 ──
	void RecordFrame();

	// ── 根据服务器时间查找最接近（≤ TargetTime 且最近）的历史快照 ──
	// 返回 nullptr 表示历史不足（刚开局 / 玩家刚加入）
	const FSSR_FrameSnapshot* FindSnapshot(float TargetTime) const;

	// ── 环形缓冲区有效条目数 ──
	int32 GetSnapshotCount() const { return FMath::Min(FrameCounter, MaxCapacity); }

	// ── 当前全局帧号 ──
	int32 GetCurrentFrameNumber() const { return CurrentFrameNumber; }

	// ── 获取追踪的骨骼名列表（供 Character 初始化使用） ──
	static const TArray<FName>& GetRelevantBoneNames() { return RelevantBoneNames; }

private:
	// ── 环形缓冲区 ──
	UPROPERTY()
	TArray<FSSR_FrameSnapshot> RingBuffer;

	int32 MaxCapacity = 0;           // 环形缓冲区最大容量 = TickRate × MaxHistorySeconds
	int32 HeadIndex = 0;             // 当前写入位置（环形推进）
	int32 FrameCounter = 0;         // 已录制帧数（用于 GetSnapshotCount）
	int32 CurrentFrameNumber = 0;   // 全局帧号（递增，不随环形回绕）

	// ── 缓存 ──
	UPROPERTY()
	TWeakObjectPtr<ABlasterGameState> GameState;

	// 需要追踪碰撞的骨骼名（静态共享，所有 USSR_FrameHistory 实例共用一份）
	static TArray<FName> RelevantBoneNames;

	// ── 内部辅助 ──
	void CapturePlayerEntry(ABlasterCharacter* Player, FSSR_PlayerFrameEntry& OutEntry) const;
	static void BuildRelevantBoneList();
};
