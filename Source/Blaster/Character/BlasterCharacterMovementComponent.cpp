// BlasterCharacterMovementComponent 实现
// 服务器端移动校验（P3 移动部分）：在 ServerMove 入口叠加速度/位移校验

#include "BlasterCharacterMovementComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

// ════════════════════════════════════════════════════════════════
// Console Variables：DS 上 ~ 控制台热调，无需重编
// 数值语义：允许速度 = GetEffectiveMaxSpeed() × Tolerance；
//           连续违规 ≥ MinConsecutive 次才触发一次客户端校正
// ════════════════════════════════════════════════════════════════

TAutoConsoleVariable<int32> CVarBlasterSpeedCheckEnabled(
	TEXT("blaster.SpeedCheck.Enabled"),
	1,
	TEXT("服务器端移动校验总开关\n0=禁用  1=启用"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterSpeedCheckTolerance(
	TEXT("blaster.SpeedCheck.Tolerance"),
	1.5f,
	TEXT("允许速度系数：允许速度 = GetEffectiveMaxSpeed() × 此值\n正常 600cm/s → 阈值 900"),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarBlasterSpeedCheckMinConsecutive(
	TEXT("blaster.SpeedCheck.MinConsecutive"),
	3,
	TEXT("连续违规次数阈值：连续 ≥N 笔移动超限才触发校正\n吸收客户端预测的单帧噪声"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterSpeedCheckBuffGraceSeconds(
	TEXT("blaster.SpeedCheck.BuffGraceSeconds"),
	1.0f,
	TEXT("Buff 宽限秒数：近期见过的最大合法速度在此窗口内沿用\n覆盖服务端 Buff 先到期、客户端晚一拍才降速的失步窗口"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterSpeedCheckMaxGap(
	TEXT("blaster.SpeedCheck.MaxGap"),
	0.5f,
	TEXT("相邻 ServerMove 服务端到达间隔上限（秒）：超此值视为传送/重生/刚落地，重置速度追踪"),
	ECVF_Default
);

// ────────────────────────────────────────────────────────────
// 客户端插值平滑（Phase 1 静态降缓冲）：远端角色渲染位置平滑时间 τ
// 引擎默认 LocationTime=0.1s / RotationTime=0.05s，是"下行滞后"的主导项。
// 砍到 0.05/0.03 直接降低被击中者看到的位置滞后；τ 是时间常数（非加法延迟），
// 砍半只砍"平滑收敛滞后"这一项。不碰 NetworkMaxSmoothUpdateDistance（超距 snap 阈值，调低只增抖）。
// 运行期动态调整由 Phase 2 自适应负责（见 BlasterPlayerController）。
// ────────────────────────────────────────────────────────────
TAutoConsoleVariable<float> CVarBlasterNetSmoothLocationTime(
	TEXT("blaster.NetSmooth.LocationTime"),
	0.05f,
	TEXT("远端角色位置平滑时间 τ（秒），引擎默认 0.1。仅客户端 simulated proxy 生效"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetSmoothRotationTime(
	TEXT("blaster.NetSmooth.RotationTime"),
	0.03f,
	TEXT("远端角色旋转平滑时间（秒），引擎默认 0.05。仅客户端 simulated proxy 生效"),
	ECVF_Default
);

UBlasterCharacterMovementComponent::UBlasterCharacterMovementComponent()
{
	// Phase 1 静态降缓冲：构造时从 CVar 读 τ 默认值写入基类 public 成员。
	// 这两个成员只对远端角色（simulated proxy）的客户端平滑有意义，
	// 本地角色（autonomous proxy）与服务器实例不读它们，无副作用。
	// ★ 必须用 GetValueOnAnyThread()：构造函数可能在异步加载线程执行（蓝图类 CDO 构建，
	// 客户端启动时 FAsyncLoadingThread2 触发）。GetValueOnGameThread() 走 GameThread 快速路径，
	// 在控制台 shadow 状态下会 ensure 报错（实测客户端启动报
	// "Ensure condition failed: GetShadowIndex() == 0"）。AnyThread 版线程安全，构造仅跑一次，稍慢无碍。
	NetworkSimulatedSmoothLocationTime = CVarBlasterNetSmoothLocationTime.GetValueOnAnyThread();
	NetworkSimulatedSmoothRotationTime = CVarBlasterNetSmoothRotationTime.GetValueOnAnyThread();
}

void UBlasterCharacterMovementComponent::SetAdaptiveSmoothLocationTime(float NewSmoothTime)
{
	const float ClampedSmoothTime = FMath::Max(0.f, NewSmoothTime);

	// PredictionData 尚未创建时，它会在构造时读取此配置值。
	NetworkSimulatedSmoothLocationTime = ClampedSmoothTime;

	// PredictionData 已创建后，指数平滑实际读取的是这里的缓存值，必须同步更新。
	if (HasPredictionData_Client())
	{
		if (FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character())
		{
			ClientData->SmoothNetUpdateTime = ClampedSmoothTime;
		}
	}
}

// ════════════════════════════════════════════════════════════════
// 服务端移动校验主入口
// 引擎在所有 ServerMove 变体处理时汇入此函数（ServerMoveHandleClientError）；
// 返回 true 会向客户端发校正（ClientAdjustPosition）。
//
// 校验策略（两层互补）：
//   1. Super()：引擎默认位置误差 / 移动模式不一致检查（按 Ping 缩放阈值）
//   2. 本类：客户端"自报位移速率"检查——用相邻两笔移动的位移 ÷ 客户端时间戳差
//      求平均速度，超 GetEffectiveMaxSpeed()×Tolerance 连续 N 次判违规。
//      作弊者若伪造慢时间戳摊薄速度，累计位置漂移仍会被第 1 层逐步校正。
// ════════════════════════════════════════════════════════════════
bool UBlasterCharacterMovementComponent::ServerCheckClientError(
	float ClientTimeStamp,
	float DeltaTime,
	const FVector& Accel,
	const FVector& ClientWorldLocation,
	const FVector& RelativeClientLocation,
	UPrimitiveComponent* ClientMovementBase,
	FName ClientBaseBoneName,
	uint8 ClientMovementMode)
{
	// 1) 保留引擎默认行为（位置误差 + 移动模式不一致）——这条永远生效
	const bool bBaseError = Super::ServerCheckClientError(
		ClientTimeStamp, DeltaTime, Accel, ClientWorldLocation, RelativeClientLocation,
		ClientMovementBase, ClientBaseBoneName, ClientMovementMode);

	if (!CVarBlasterSpeedCheckEnabled.GetValueOnGameThread()) return bBaseError; // 是否开启总开关

	// 2) 只在行走/导航行走时做速度判定——下落跳过，否则误杀跳跃
	//    （JumpPickup 把 JumpZVelocity 提到 4000，下落真实速度可 >> MaxWalkSpeed）
	if (MovementMode != MOVE_Walking && MovementMode != MOVE_NavWalking)
	{
		ConsecutiveSpeedViolations = 0;
		return bBaseError;
	}

	const float ServerNow = GetWorld()->GetTimeSeconds();

	// 3) 大间隔/乱序/首帧 → 重置追踪，本帧不判
	//    覆盖：传送、重生、刚落地、丢包后的首笔移动
	const float ServerGap = ServerNow - LastMoveServerTime;
	if (!bHasPreviousMove
		|| ServerGap > CVarBlasterSpeedCheckMaxGap.GetValueOnGameThread()
		|| ClientTimeStamp <= LastClientTimeStamp)
	{
		ResetSpeedTracking(ClientTimeStamp, ClientWorldLocation, ServerNow);
		return bBaseError;
	}

	// 4) 平均速度 = 位移 ÷ 客户端时间戳差
	//    用客户端时间戳差做分母：客户端合并/引擎批处理 ServerMove 会放大
	//    服务端到达间隔波动，客户端时间戳差才是真实位移速率
	const float ClientDt = FMath::Max(ClientTimeStamp - LastClientTimeStamp, 0.016f);
	const float ClientSpeed = (ClientWorldLocation - LastClientLocation).Size() / ClientDt;
	const float Allowed = GetEffectiveMaxSpeed(ServerNow)
		* CVarBlasterSpeedCheckTolerance.GetValueOnGameThread();

	bool bSpeedViolation = false;
	if (ClientSpeed > Allowed)
	{
		// 5) 连续违规计数：达到阈值才判违规，吸收单帧预测噪声
		++ConsecutiveSpeedViolations;
		if (ConsecutiveSpeedViolations >= CVarBlasterSpeedCheckMinConsecutive.GetValueOnGameThread())
		{
			bSpeedViolation = true;
			// 日志节流：只在首次触线/计数翻倍时打，避免 60Hz 刷屏拖慢 DS
			if (++TotalViolationCount % 32 == 1)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[AntiSpeed] %s | speed=%.0fcm/s allowed=%.0f | consecutive=%d"),
					*GetNameSafe(CharacterOwner.Get()), ClientSpeed, Allowed, ConsecutiveSpeedViolations);
			}
		}
	}
	else
	{
		// 一次干净移动即清零，吸收单帧噪声
		ConsecutiveSpeedViolations = 0;
	}

	// 6) 更新追踪样本（用收到的位置，无论本帧是否判违规）
	LastMoveServerTime = ServerNow;
	LastClientTimeStamp = ClientTimeStamp;
	LastClientLocation = ClientWorldLocation;

	return bBaseError || bSpeedViolation; // true → 引擎发客户端校正
}

// ════════════════════════════════════════════════════════════════
// 允许速度计算（含 Buff 宽限，防误杀）
// 服务端 GetMaxSpeed() 是权威的（Buff 通过 NetMulticast 服务端同步改、
// 瞄准改 MaxWalkSpeed），正常合法速度不会超过它。
// 唯一风险窗口：SpeedBuff 到期时服务端先降速、客户端晚一个单向延迟才降速，
// 期间客户端真实速度 1600 > 朴素阈值 600×1.5=900，若不宽限必误杀。
// 对策：把"服务端近期见过的最大允许速度"保留 BuffGraceSeconds，盖住失步窗口。
// ════════════════════════════════════════════════════════════════
float UBlasterCharacterMovementComponent::GetEffectiveMaxSpeed(float ServerNow)
{
	const float Current = GetMaxSpeed(); // 服务端权威：含瞄准 600/300、SpeedBuff 1600、蹲

	if (!bHasGraceSpeed)
	{
		// 首帧：建立基准并起窗
		bHasGraceSpeed = true;
		GraceMaxSpeed = Current;
		GraceEndServerTime = ServerNow + CVarBlasterSpeedCheckBuffGraceSeconds.GetValueOnGameThread();
		return Current;
	}

	if (ServerNow > GraceEndServerTime)
	{
		// 窗口过期：之前的高速度已不被服务端认可，回落到当前权威速度并重新起窗
		// （Buff 存续期每次过期都会重新锚定到 1600，因此长 Buff 全程被覆盖）
		GraceMaxSpeed = Current;
		GraceEndServerTime = ServerNow + CVarBlasterSpeedCheckBuffGraceSeconds.GetValueOnGameThread();
		return Current;
	}

	if (Current > GraceMaxSpeed)
	{
		// 服务端允许速度升高（Buff 生效）：抬高上限并重新起窗
		GraceMaxSpeed = Current;
		GraceEndServerTime = ServerNow + CVarBlasterSpeedCheckBuffGraceSeconds.GetValueOnGameThread();
	}
	return FMath::Max(GraceMaxSpeed, Current);
}

void UBlasterCharacterMovementComponent::ResetSpeedTracking(float ClientTimeStamp, const FVector& ClientWorldLocation, float ServerNow)
{
	// 重置为"仅基准样本"：下一笔移动才具备可比性，本笔不做速率判定
	LastMoveServerTime = ServerNow;
	LastClientTimeStamp = ClientTimeStamp;
	LastClientLocation = ClientWorldLocation;
	bHasPreviousMove = true; // 从下一笔开始可判
	ConsecutiveSpeedViolations = 0;
}
