// BlasterCharacterMovementComponent：服务器端移动校验（P3 服务器端校验增强 - 移动部分）
// 设计意图：
//   Blaster 是 Dedicated Server 权威架构，但移动走的是引擎默认 UCharacterMovementComponent，
//   没有任何 ServerMove 级反作弊——恶意客户端可伪造移动数据（超速、瞬移）蒙混过关。
//   本类替换角色默认移动组件，在 ServerMove 入口（ServerCheckClientError）叠加一层
//   "客户端自报位移速率"校验：超上限连续 N 次即判定违规，返回 true 触发引擎客户端校正。
//
// 为什么用 ServerCheckClientError 而不是 ServerMove_Implementation：
//   引擎所有 ServerMove 变体（单/双/无基础移动）最终都汇入 ServerMoveHandleClientError，
//   它是唯一的服务端校验闸口，且返回 true 会自动走标准校正链路（ClientAdjustPosition）。
//   参考：CharacterMovementComponent.h:2239（protected virtual）
//
// 防误杀关键：
//   - 只在 MOVE_Walking/NavWalking 判定（下落跳过，跳跃合法高速不被误伤）
//   - 允许速度 = GetEffectiveMaxSpeed() × 容差；GetEffectiveMaxSpeed 带 Buff 宽限，
//     覆盖"服务端 Buff 先到期、客户端晚一拍才降速"的失步窗口
//   - 连续违规 ≥ MinConsecutive 次才触发校正，吸收单帧预测噪声

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "BlasterCharacterMovementComponent.generated.h"

// ────────────────────────────────────────────────────────────
// Console Variables（定义在 .cpp 中，此处为全局 extern 声明）
// 沿用项目 SSR 的 TAutoConsoleVariable + extern 模式（见 SSR_FrameHistory.h）
// DS 上可用 ~ 控制台热调，无需重编；客户端控制台只在本进程生效，无被篡改面
// ────────────────────────────────────────────────────────────
extern TAutoConsoleVariable<int32> CVarBlasterSpeedCheckEnabled;
extern TAutoConsoleVariable<float> CVarBlasterSpeedCheckTolerance;
extern TAutoConsoleVariable<int32> CVarBlasterSpeedCheckMinConsecutive;
extern TAutoConsoleVariable<float> CVarBlasterSpeedCheckBuffGraceSeconds;
extern TAutoConsoleVariable<float> CVarBlasterSpeedCheckMaxGap;

// ── 客户端插值平滑（Phase 1 静态降缓冲）──
// 降低远端角色（simulated proxy）渲染位置的平滑收敛滞后 τ，
// 与 SSR 构成"补上行 + 补下行"的双向延迟补偿。
extern TAutoConsoleVariable<float> CVarBlasterNetSmoothLocationTime;
extern TAutoConsoleVariable<float> CVarBlasterNetSmoothRotationTime;

UCLASS()
class BLASTER_API UBlasterCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UBlasterCharacterMovementComponent();

protected:
	// 服务端每个 ServerMove 到达时由引擎调用（ServerMoveHandleClientError）
	// 返回 true → 引擎向客户端发校正（ClientAdjustPosition）
	virtual bool ServerCheckClientError(
		float ClientTimeStamp,
		float DeltaTime,
		const FVector& Accel,
		const FVector& ClientWorldLocation,
		const FVector& RelativeClientLocation,
		UPrimitiveComponent* ClientMovementBase,
		FName ClientBaseBoneName,
		uint8 ClientMovementMode) override;

private:
	// 返回本次判速允许的上限速度（含 Buff 宽限）
	float GetEffectiveMaxSpeed(float ServerNow);
	// 大间隔/乱序/首帧时重置速度追踪样本，避免把传送/重生/刚落地误判为超速
	void ResetSpeedTracking(float ClientTimeStamp, const FVector& ClientWorldLocation, float ServerNow);

	// ── 速度追踪状态（仅服务器实例有意义；客户端实例不参与 ServerMove 判定）──
	float LastMoveServerTime = -1.f;          // 上一次判速的服务端世界时间
	float LastClientTimeStamp = -1.f;         // 上一次判速的客户端移动时间戳
	FVector LastClientLocation = FVector::ZeroVector; // 上一次客户端自报位置
	bool bHasPreviousMove = false;            // 是否有可比的上一笔移动
	int32 ConsecutiveSpeedViolations = 0;     // 连续违规计数（≥MinConsecutive 才触发）
	int32 TotalViolationCount = 0;            // 累计违规计数，仅用于日志节流

	// ── Buff 宽限状态：近期见过的最大合法速度，窗口内沿用 ──
	float GraceMaxSpeed = -1.f;               // 宽限窗口内保留的最高速度上限
	float GraceEndServerTime = -1.f;          // 宽限窗口到期时间（服务端世界时间）
	bool bHasGraceSpeed = false;
};
