// Blaster SSR：纯数学射线相交判定（HitScan + 霰弹枪共用）
// 不操作 PhysX 碰撞体——直接用 FrameHistory 中的 Capsule/Bone 快照数据
// 做射线-胶囊体/球体数学相交测试，100% 确定性

#include "SSR_RewindManager.h"
#include "SSR_FrameHistory.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/WeaponSystem/Weapon/HitScanWeapon.h"
#include "Blaster/WeaponSystem/Weapon/Shotgun.h"
#include "Blaster/WeaponSystem/Weapon/Weapon.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/SkeletalMeshSocket.h" // 服务器枪口 MuzzleFlash 插槽（视线复核第二路）
#include "DrawDebugHelpers.h"

// ════════════════════════════════════════════════════════════════
// 纯数学相交辅助函数
// ════════════════════════════════════════════════════════════════

// 骨骼的近似碰撞半径（用于 head / pelvis / spine 等骨骼的射线-球体判定）
static constexpr float BONE_RADIUS = 12.f;

// 射线-球体相交：返回沿射线方向的参数 t（RayDir 必须是单位向量），无交点返回 -1
static float RaySphereIntersect(const FVector& RayOrigin, const FVector& RayDir,
	const FVector& SphereCenter, float SphereRadius)
{
	const FVector OC = RayOrigin - SphereCenter;
	const float b = 2.f * FVector::DotProduct(RayDir, OC);
	const float c = OC.SizeSquared() - SphereRadius * SphereRadius;
	const float Disc = b * b - 4.f * c;
	if (Disc < 0.f) return -1.f;

	const float t = (-b - FMath::Sqrt(Disc)) * 0.5f;
	return t > 0.f ? t : -1.f;
}

// 射线-胶囊体相交：胶囊体中心 C、半高 H（从中心到两端）、半径 R
// 将射线变换到胶囊体局部空间（胶囊体沿 Z 轴），分别检测圆柱段 + 两端半球
// 返回射线参数 t（RayDir 必须是单位向量），无交点返回 -1
static float RayCapsuleIntersect(const FVector& RayOrigin, const FVector& RayDir,
	const FVector& CapsuleCenter, const FQuat& CapsuleRot,
	float CapsuleHalfHeight, float CapsuleRadius)
{
	const FVector O = CapsuleRot.UnrotateVector(RayOrigin - CapsuleCenter);
	const FVector D = CapsuleRot.UnrotateVector(RayDir);

	const float H = CapsuleHalfHeight;
	const float R = CapsuleRadius;
	float BestT = TNumericLimits<float>::Max();
	bool bHit = false;

	const float aXY = D.X * D.X + D.Y * D.Y;

	// ── 圆柱段：|z| ≤ H-R，x² + y² = R² ──
	if (aXY > KINDA_SMALL_NUMBER)
	{
		const float b = 2.f * (O.X * D.X + O.Y * D.Y);
		const float c = O.X * O.X + O.Y * O.Y - R * R;
		const float Disc = b * b - 4.f * aXY * c;

		if (Disc >= 0.f)
		{
			const float SqrtDisc = FMath::Sqrt(Disc);
			for (float t : { (-b - SqrtDisc) / (2.f * aXY), (-b + SqrtDisc) / (2.f * aXY) })
			{
				if (t <= 0.f) continue;
				const float Z = O.Z + t * D.Z;
				if (Z >= -(H - R) && Z <= (H - R))
				{
					BestT = FMath::Min(BestT, t);
					bHit = true;
				}
			}
		}
	}
	else
	{
		const float DistXY = FMath::Sqrt(O.X * O.X + O.Y * O.Y);
		if (DistXY <= R)
		{
			const float ZEntry = FMath::Max(O.Z, -(H - R));
			if (ZEntry <= H - R)
			{
				const float t = (ZEntry - O.Z) / D.Z;
				if (t > 0.f) { BestT = FMath::Min(BestT, t); bHit = true; }
			}
		}
	}

	// ── 底端半球：中心 (0, 0, -(H-R))，半径 R ──
	{
		const float t = RaySphereIntersect(O, D, FVector(0.f, 0.f, -(H - R)), R);
		if (t > 0.f && (O.Z + t * D.Z) < -(H - R))
			{ BestT = FMath::Min(BestT, t); bHit = true; }
	}

	// ── 顶端半球：中心 (0, 0, H-R)，半径 R ──
	{
		const float t = RaySphereIntersect(O, D, FVector(0.f, 0.f, H - R), R);
		if (t > 0.f && (O.Z + t * D.Z) > H - R)
			{ BestT = FMath::Min(BestT, t); bHit = true; }
	}

	return bHit ? BestT : -1.f;
}

// ════════════════════════════════════════════════════════════════
// 单条射线的历史命中检测（HitScan 和霰弹枪共用）
// 遍历历史快照中所有玩家，取 t 最小的胶囊体/骨骼命中
// ════════════════════════════════════════════════════════════════

static FSSR_TraceResult MathTraceSingleRay(
	const FVector& TraceStart,
	const FVector& TraceEnd,
	const FSSR_FrameSnapshot& HistoricalFrame,
	ABlasterCharacter* Shooter)
{
	FSSR_TraceResult Result;

	// ── 射线准备 ──
	const FVector RayDir = (TraceEnd - TraceStart).GetSafeNormal();  // 方向归一化成单位向量（t 即世界距离 cm）
	const float MaxDist = (TraceEnd - TraceStart).Size();            // 射线总长 = 最大命中距离（超出射程不算命中）

	float BestT = TNumericLimits<float>::Max();   // 目前最近命中的距离（初始"无穷大"）
	ABlasterCharacter* BestChar = nullptr;         // 目前最近的命中者（初始"没人"）
	FName BestBone = NAME_None;                    // 目前命中的部位（初始"无"）

	// ── 遍历历史帧里的所有玩家，对每人打这根射线 ──
	for (const FSSR_PlayerFrameEntry& Entry : HistoricalFrame.PlayerEntries)
	{
		// 跳过三类：快照角色已销毁（弱引用自动置空）/ 射击者自己 / 已死亡
		if (!Entry.Character.IsValid()) continue;
		ABlasterCharacter* OtherChar = Entry.Character.Get();
		if (OtherChar == Shooter || OtherChar->IsElimmed()) continue;

		// 诊断日志：打印历史快照姿势 + 射线，排查两端姿态/位置偏差（对照客户端枪口）
		UE_LOG(LogTemp, Warning, TEXT("[SSR]   MATH | Target=%s | HistCaps=(%.0f,%.0f,%.0f) H=%.0f R=%.0f | Bones=%d | RayStart=(%.0f,%.0f,%.0f) | RayDir=(%.3f,%.3f,%.3f)"),
			*GetNameSafe(OtherChar),
			Entry.CapsuleLocation.X, Entry.CapsuleLocation.Y, Entry.CapsuleLocation.Z,
			Entry.CapsuleHalfHeight, Entry.CapsuleRadius,
			Entry.BoneSnapshots.Num(),
			TraceStart.X, TraceStart.Y, TraceStart.Z,
			RayDir.X, RayDir.Y, RayDir.Z);

		// 1. 骨骼球体命中（用于判定爆头/肢体）
		//    对每根骨骼的小球（球心 = Bone.Location，半径 = BONE_RADIUS）做射线相交
		for (const FSSR_BoneSnapshot& Bone : Entry.BoneSnapshots)
		{
			const float t = RaySphereIntersect(TraceStart, RayDir, Bone.Location, BONE_RADIUS);
			// 有效命中三条件：射线前方(t>0) + 射程内(t<=MaxDist) + 比当前最近更近(t<BestT)
			if (t > 0.f && t <= MaxDist && t < BestT)
			{
				BestT = t;
				BestChar = OtherChar;
				BestBone = Bone.BoneName;  // 记住是哪个部位（head = 爆头）
			}
		}

		// 2. 胶囊体命中（覆盖整个身体，兜底）
		//    骨骼球都没中时，整身胶囊体接住 → BoneName 置 None = 普通身体伤害
		const float tCapsule = RayCapsuleIntersect(TraceStart, RayDir,
			Entry.CapsuleLocation, Entry.CapsuleRotation,
			Entry.CapsuleHalfHeight, Entry.CapsuleRadius);
		if (tCapsule > 0.f && tCapsule <= MaxDist && tCapsule < BestT)
		{
			BestT = tCapsule;
			BestChar = OtherChar;
			BestBone = NAME_None;  // 身体命中，不是具体部位
		}
	}

	// ── 组装结果：有最近命中才填，没命中返回空（调用方走兜底）──
	if (BestChar)
	{
		Result.bHit = true;
		Result.ImpactPoint = TraceStart + RayDir * BestT;  // 命中点 = 起点 + 方向 × 最近 t
		Result.BoneName = BestBone;                        // 部位（ApplySSRDamage 判爆头用）
		Result.HitActor = BestChar;                        // 被命中角色（伤害对象）

		UE_LOG(LogTemp, Log, TEXT("[SSR] ✓ MATH HIT | Target=%s | Bone=%s | Impact=(%.0f, %.0f, %.0f) | t=%.1f"),
			*GetNameSafe(BestChar),
			BestBone.IsNone() ? TEXT("body") : *BestBone.ToString(),
			Result.ImpactPoint.X, Result.ImpactPoint.Y, Result.ImpactPoint.Z,
			BestT);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[SSR] ✗ MATH miss | Trace=(%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) | %d entries checked"),
			TraceStart.X, TraceStart.Y, TraceStart.Z,
			TraceEnd.X, TraceEnd.Y, TraceEnd.Z,
			HistoricalFrame.PlayerEntries.Num());
	}

	return Result;
}

// ════════════════════════════════════════════════════════════════
// 初始化
// ════════════════════════════════════════════════════════════════

void USSR_RewindManager::Initialize(ABlasterGameState* InGameState)
{
	GameState = InGameState;
	UE_LOG(LogTemp, Log, TEXT("[SSR] RewindManager initialized"));
}

// ════════════════════════════════════════════════════════════════
// 时间计算 + 历史帧查找（HitScan 和霰弹枪共用）
// ════════════════════════════════════════════════════════════════

static const FSSR_FrameSnapshot* FindRewindFrame(
	UWorld* World, USSR_FrameHistory* FrameHistory,
	float ClientShotServerTime, FStringView CallerName)
{
	// ① 服务器当前时间 = "子弹数据到达"的时刻
	const double ServerNow = World->GetTimeSeconds();
	// ② 单向延迟 = 现在 - 客户端报的开枪时刻（子弹"在路上"多久）
	double OneWayDelay = ServerNow - ClientShotServerTime;
	// ③ ★回退限制：Clamp 到 [0, 0.25s]——正常延迟原样回退；
	//   超过上限（高延迟/伪造时间戳）截断，防无限回退（防"预判作弊"）
	OneWayDelay = FMath::Clamp( OneWayDelay,   0.0,   (double)CVarSSRMaxPingCompensation.GetValueOnGameThread() );
	//             └─钳制函数┘  └─被限制的值┘  └─下限┘  └─上限（0.25s）┘


	// 诊断日志：打印两端时间与截断后延迟（毫秒），排查时钟同步误差
	UE_LOG(LogTemp, Verbose, TEXT("[SSR] %.*s | ServerNow=%.3f | ClientTime=%.3f | ClampedDelay=%.1fms"),
		CallerName.Len(), CallerName.GetData(), ServerNow, ClientShotServerTime, OneWayDelay * 1000.0);

	// ④ 回退目标时刻 = 现在 - 延迟 = "客户端开枪那一刻"（换算成服务器时间）
	const double RewindTargetTime = ServerNow - OneWayDelay;
	// ⑤ 在历史缓冲里找"≤ 开枪时刻的最新帧"——这就是要拿去判定命中的那一帧
	const FSSR_FrameSnapshot* Frame = FrameHistory->FindSnapshot(RewindTargetTime);

	// 历史不足（目标比最老帧还早，如开局前 0.5s）→ 返回空，调用方走当前帧兜底
	if (!Frame)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[SSR] → No historical snapshot for t=%.3f (count=%d)"),
			RewindTargetTime, FrameHistory->GetSnapshotCount());
	}

	return Frame;
}

// ════════════════════════════════════════════════════════════════
// 视线复核（防隔墙报点）
// ════════════════════════════════════════════════════════════════

// 枪口→命中点之间是否有非角色阻塞体（墙/门/道具）挡住。
// 角色（目标/队友/射手自己）由 MathTraceSingleRay 的数学射线负责判定，这里只关心"墙"：
// 用默认碰撞参数 + 在结果里跳过所有 ABlasterCharacter，等价于"只看世界几何"。
// 起点沿射线前推 5cm，避免枪口贴墙/进墙时端点误判。
static bool IsImpactOccludedByWorld(UWorld* World, const FVector& Muzzle,
	const FSSR_TraceResult& Result)
{
	if (!World || !Result.bHit) return false;

	const FVector Dir = (Result.ImpactPoint - Muzzle).GetSafeNormal();
	const FVector Start = Muzzle + Dir * 5.f;

	TArray<FHitResult> Hits;
	World->LineTraceMultiByChannel(Hits, Start, Result.ImpactPoint, ECC_Visibility);
	for (const FHitResult& H : Hits)
	{
		AActor* A = H.GetActor();
		if (!A) continue;
		if (Cast<ABlasterCharacter>(A)) continue;   // 角色跳过（数学射线已处理）
		return true;                                // 第一个非角色阻塞体 → 被墙遮挡
	}
	return false;
}

// 双原点遮挡：客户端枪口 + 服务器武器枪口各打一路，任一被非角色阻塞体挡住即拒绝。
// 堵"作弊者把 ClientMuzzle 伪造到墙另一侧"——服务器武器枪口是权威的，躲不掉。
// 服务器与客户端枪口只差几厘米（服务端骨骼每帧刷新），误杀增量极小。
static bool IsShotOccluded(UWorld* World, const FVector& ClientMuzzle,
	AWeapon* Weapon, const FSSR_TraceResult& Result)
{
	if (IsImpactOccludedByWorld(World, ClientMuzzle, Result)) return true;
	if (Weapon)
	{
		if (const USkeletalMeshSocket* ServerMuzzle = Weapon->GetWeaponMesh()->GetSocketByName("MuzzleFlash"))
		{
			const FVector ServerMuzzleLoc = ServerMuzzle->GetSocketTransform(Weapon->GetWeaponMesh()).GetLocation();
			if (IsImpactOccludedByWorld(World, ServerMuzzleLoc, Result)) return true;
		}
	}
	return false;
}

// ════════════════════════════════════════════════════════════════
// 单发 HitScan 的 SSR 处理
// ════════════════════════════════════════════════════════════════

FSSR_TraceResult USSR_RewindManager::ProcessHitScanShot(
	ABlasterCharacter* Shooter,
	AHitScanWeapon* Weapon,
	const FVector& TraceStart,
	const FVector& HitTarget,
	float ClientShotServerTime)
{
	FSSR_TraceResult Result;

	// ── 防御检查：任一不满足直接返回空（调用方走当前帧兜底）──
	if (!Shooter || !Weapon || !GameState.IsValid()) return Result;   // 参数 / GameState 无效
	if (!CVarSSREnabled.GetValueOnGameThread()) return Result;         // SSR 总开关关闭

	UWorld* World = GetWorld();
	if (!World) return Result;

	USSR_FrameHistory* FrameHistory = GameState->GetSSRFrameHistory(); // 拿帧历史（录制方）
	if (!FrameHistory) return Result;

	// ClientShotTime = 客户端上报的"估计服务器时刻"

	//拿"客户端开枪那一刻"对应的历史帧（回退的核心动作）
	const FSSR_FrameSnapshot* HistoricalFrame = FindRewindFrame(World, FrameHistory, ClientShotServerTime, TEXT("ProcessHitScan"));
	if (!HistoricalFrame) return Result;   // 没拿到帧 → 直接返回空结果

	UE_LOG(LogTemp, Verbose, TEXT("[SSR] → Rewind to frame #%d (t=%.3f), %d player entries"),
		HistoricalFrame->FrameNumber, HistoricalFrame->Timestamp, HistoricalFrame->PlayerEntries.Num());

	// 射线终点：瞄准点向后延长 25%——防止目标"擦着瞄准点边缘"漏判（略偏后也能打到）
	const FVector TraceEnd = TraceStart + (HitTarget - TraceStart) * 1.25f;
	// ── 核心判定：把"射线 + 历史帧 + 射击者"交给纯函数，找最近命中 ──
	Result = MathTraceSingleRay(TraceStart, TraceEnd, *HistoricalFrame, Shooter);

	// ── 视线复核（防隔墙报点）：命中点被墙挡住 → 拒绝，落回当前帧兜底路径 ──
	// 原 SSR 只做纯数学相交不查墙，作弊者报墙后 HitTarget 可穿墙命中；
	// 这里从客户端枪口 + 服务器枪口双路查墙，任一被挡即拒绝（双原点防枪口伪造）。
	if (Result.bHit && CVarSSROcclusionEnabled.GetValueOnGameThread() > 0
		&& IsShotOccluded(World, TraceStart, Weapon, Result))
	{
		UE_LOG(LogTemp, Warning, TEXT("[SSR] ✗ OCCLUDED | Shooter=%s → %s blocked by wall"),
			*GetNameSafe(Shooter), *GetNameSafe(Result.HitActor.Get()));
		Result.bHit = false;
		Result.HitActor = nullptr;
		Result.BoneName = NAME_None;
	}

	// Debug 可视化（ssr.DrawDebug 控制）：命中画绿、未命中画红；命中点画球
	if (CVarSSRDrawDebug.GetValueOnGameThread() > 0)
	{
		const FColor RayColor = Result.bHit ? FColor::Green : FColor::Red;
		DrawDebugLine(World, TraceStart, TraceEnd, RayColor, false, 2.f, 0, 1.f);
		if (Result.bHit)
			DrawDebugSphere(World, Result.ImpactPoint, 12.f, 12, FColor::Green, false, 2.f);
	}

	return Result;   // 交给调用方：命中 → ApplySSRDamage 结算；未命中 → 兜底当前帧
}

// ════════════════════════════════════════════════════════════════
// 霰弹枪多弹丸 SSR 处理（每颗弹丸独立数学判定）
// ════════════════════════════════════════════════════════════════

TArray<FSSR_TraceResult> USSR_RewindManager::ProcessShotgunPellets(
	ABlasterCharacter* Shooter,
	AShotgun* Shotgun,
	const FVector& TraceStart,
	const TArray<FVector_NetQuantize>& HitTargets,
	float ClientShotServerTime)
{
	TArray<FSSR_TraceResult> Results;

	// ── 防御检查（和单发同款）──
	if (!Shooter || !Shotgun || !GameState.IsValid()) return Results;
	if (!CVarSSREnabled.GetValueOnGameThread()) return Results;

	UWorld* World = GetWorld();
	if (!World) return Results;

	USSR_FrameHistory* FrameHistory = GameState->GetSSRFrameHistory();
	if (!FrameHistory) return Results;

	// ── 时间对齐（和单发同款）：找"开枪那一刻"的帧 ──
	// 关键：所有弹丸用【同一帧】判定——一枪十几颗弹丸共享同一个回退时刻
	const FSSR_FrameSnapshot* HistoricalFrame = FindRewindFrame(World, FrameHistory, ClientShotServerTime, TEXT("Shotgun"));
	if (!HistoricalFrame) return Results;

	UE_LOG(LogTemp, Verbose, TEXT("[SSR] Shotgun | frame #%d | %d pellets | %d history entries"),
		HistoricalFrame->FrameNumber, HitTargets.Num(), HistoricalFrame->PlayerEntries.Num());

	// ── 每颗弹丸独立判定：各自打一根射线（复用 MathTraceSingleRay！）──
	// 每颗弹丸有自己的散射命中点(HitTarget)，各自算射线终点、各自找最近命中
	int32 HitCount = 0;
	for (const FVector& HitTarget : HitTargets)
	{
		const FVector TraceEnd = TraceStart + (HitTarget - TraceStart) * 1.25f;   // 每颗弹丸独立延长 25%
		FSSR_TraceResult PelletResult = MathTraceSingleRay(TraceStart, TraceEnd, *HistoricalFrame, Shooter);

		// 视线复核：该弹丸命中点被墙挡住 → 取消这颗弹丸的命中（防穿墙结算）
		if (PelletResult.bHit && CVarSSROcclusionEnabled.GetValueOnGameThread() > 0
			&& IsShotOccluded(World, TraceStart, Shotgun, PelletResult))
		{
			UE_LOG(LogTemp, Verbose, TEXT("[SSR] ✗ Pellet occluded | %s"),
				*GetNameSafe(PelletResult.HitActor.Get()));
			PelletResult.bHit = false;
			PelletResult.HitActor = nullptr;
			PelletResult.BoneName = NAME_None;
		}

		if (PelletResult.bHit) HitCount++;   // 统计命中了几颗
		Results.Add(PelletResult);           // 每颗弹丸的结果单独存（调用方聚合成伤害）
	}

	UE_LOG(LogTemp, Log, TEXT("[SSR] Shotgun result | %d/%d pellets hit"), HitCount, HitTargets.Num());
	return Results;   // 调用方（ServerShotgunFire）遍历 Results，按目标累加伤害后一次性 ApplyDamage
}

// ════════════════════════════════════════════════════════════════
// SSR 命中伤害应用
// ════════════════════════════════════════════════════════════════

bool USSR_RewindManager::ApplySSRDamage(
	ABlasterCharacter* Shooter,
	AWeapon* Weapon,
	const FSSR_TraceResult& Result)
{
	if (!Result.bHit || !Result.HitActor.IsValid() || !Shooter || !Weapon) return false;

	ABlasterCharacter* HitChar = Cast<ABlasterCharacter>(Result.HitActor.Get());
	if (!HitChar) return false;

	const bool bHeadShot = (Result.BoneName == FName("head"));
	const float Damage = bHeadShot ? Weapon->GetHeadShotDamage() : Weapon->GetDamage();

	UE_LOG(LogTemp, Log, TEXT("[SSR] DAMAGE | Shooter=%s → Victim=%s | %s | %.0f dmg | HitPos=(%.0f, %.0f, %.0f)"),
		*GetNameSafe(Shooter),
		*GetNameSafe(HitChar),
		bHeadShot ? TEXT("HEADSHOT") : TEXT("body"),
		Damage,
		Result.ImpactPoint.X, Result.ImpactPoint.Y, Result.ImpactPoint.Z);

	AController* InstigatorController = Shooter->GetController();
	UGameplayStatics::ApplyDamage(
		HitChar, Damage, InstigatorController, Weapon, UDamageType::StaticClass());

	return true;
}
