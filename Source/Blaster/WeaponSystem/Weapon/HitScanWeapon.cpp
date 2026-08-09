#include "HitScanWeapon.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundCue.h"

void AHitScanWeapon::Fire(const FVector& HitTarget)
{
	Super::Fire(HitTarget); // 基类公共三段：开火动画 + 抛壳(生成ACasing) + 扣弹药(SpendRound)

	// ── 取"是谁开的枪"：ApplyDamage 需要肇事者信息 ──
	APawn* OwnerPawn = Cast<APawn>(GetOwner());              // 武器持有者（玩家角色）
	if (OwnerPawn == nullptr) return;                        // 武器没有持有者（掉在地上的枪）→ 不结算不表现
	AController* InstigatorController = OwnerPawn->GetController(); // 持有者的 Controller，作伤害来源(Instigator)

	// ── 找枪口插槽：射线起点 = 枪口世界坐标 ──
	const USkeletalMeshSocket* MuzzleFlashSocket = GetWeaponMesh()->GetSocketByName("MuzzleFlash");
	if (MuzzleFlashSocket) // 必须存在 MuzzleFlash 插槽（蓝图配置的枪口），没有则整段跳过
	{
		FTransform SocketTransform = MuzzleFlashSocket->GetSocketTransform(GetWeaponMesh());
		FVector Start = SocketTransform.GetLocation();

		// 诊断日志：对比客户端 vs 服务器枪口位置（排查两端动画姿态偏差——SSR 用的是客户端枪口）
		UE_LOG(LogTemp, Warning, TEXT("[FIRE] %s | Muzzle=(%.0f,%.0f,%.0f) | HitTarget=(%.0f,%.0f,%.0f)"),
			HasAuthority() ? TEXT("SERVER") : TEXT("CLIENT"),
			Start.X, Start.Y, Start.Z,
			HitTarget.X, HitTarget.Y, HitTarget.Z);

		// ── 打射线：从枪口到瞄准点，看打中了什么 ──	客户端也打射线！（但只为找命中点播特效）
		FHitResult FireHit;
		WeaponTraceHit(Start, HitTarget, FireHit); // 引擎单通道射线检测(ECC_Visibility)，返回命中Actor/骨骼名/落点

		ABlasterCharacter* HitCharacter = Cast<ABlasterCharacter>(FireHit.GetActor()); // 命中者是不是玩家角色

		// ── 伤害结算（四个条件缺一不可）──
		// ① !bSSRHandledShot：SSR 已结算这一发 → 跳过，防二次伤害（这是 MulticastFire 的兜底路径）
		// ② HitCharacter：必须命中玩家角色，打墙/打地不结算
		// ③ InstigatorController：必须有射击者（无主武器的射线不算伤害）
		// ④ HasAuthority()：只有服务器能结算——客户端走这里只播表现，trace 结果仅供特效
		if (!bSSRHandledShot && HitCharacter && InstigatorController && HasAuthority())
		{
			// 命中骨骼是 head → 爆头伤害，否则普通伤害（部位伤害判定 = 看命中骨骼名）
			const float DamageToCause = FireHit.BoneName.ToString() == FString("head") ? HeadShotDamage : Damage;
			UGameplayStatics::ApplyDamage( // 引擎统一伤害入口 → 触发被击者 ReceiveDamage（护盾吸收→扣血→受击打断→死亡）
				HitCharacter,
				DamageToCause,
				InstigatorController,      // 伤害来源控制器（用于击杀归属 / 连杀奖励）
				this,                      // 伤害来源对象（武器）
				UDamageType::StaticClass() // 伤害类型（本项目未细分，统一 UDamageType）
			);
		}
		bSSRHandledShot = false; // 重置标记：这一发已判定完毕，下一发由 SSR 重新判定

		// ── 以下全是纯表现（每端各自本地播放；服务器不结算也一样播）──
		if (ImpactParticles) // 命中点粒子特效（打在目标/墙上的火花）
		{
			UGameplayStatics::SpawnEmitterAtLocation(
				GetWorld(),
				ImpactParticles,
				FireHit.ImpactPoint,            // 命中点位置
				FireHit.ImpactNormal.Rotation() // 朝向 = 命中面法线（让火花垂直于墙面）
			);
		}
		if (HitSound) // 命中音效（子弹击中目标的声音）
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				HitSound,
				FireHit.ImpactPoint // 在命中点发声
			);
		}
		if (MuzzleFlash) // 枪口火光（开火瞬间的视觉反馈）
		{
			UGameplayStatics::SpawnEmitterAtLocation(
				GetWorld(),
				MuzzleFlash,
				SocketTransform // 枪口插槽位置（随枪口动）
			);
		}
		if (FireSound) // 开枪声（在枪的位置发声，让玩家听到自己的枪声）
		{
			UGameplayStatics::PlaySoundAtLocation(
				this,
				FireSound,
				GetActorLocation()
			);
		}
	}
}

void AHitScanWeapon::WeaponTraceHit(const FVector& TraceStart, const FVector& HitTarget, FHitResult& OutHit)
{
	UWorld* World = GetWorld();
	if (World)
	{
		FVector End = TraceStart + (HitTarget - TraceStart) * 1.25f;

		World->LineTraceSingleByChannel(
			OutHit,
			TraceStart,
			End,
			ECollisionChannel::ECC_Visibility
		);

		FVector BeamEnd = End;
		if (OutHit.bBlockingHit)
		{
			BeamEnd = OutHit.ImpactPoint;
		}
		else
		{
			OutHit.ImpactPoint = End;
		}

		if (BeamParticles)
		{
			UParticleSystemComponent* Beam = UGameplayStatics::SpawnEmitterAtLocation(
				World,
				BeamParticles,
				TraceStart,
				FRotator::ZeroRotator,
				true
			);
			if (Beam)
			{
				Beam->SetVectorParameter(FName("Target"), BeamEnd);
			}
		}
	}
}
