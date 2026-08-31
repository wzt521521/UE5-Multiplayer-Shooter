


#include "CombatComponent.h"
#include "../Character/BlasterCharacter.h"
#include "Blaster/WeaponSystem/Weapon/Weapon.h"
#include "Blaster/WeaponSystem/Weapon/Shotgun.h"
#include "Camera/CameraComponent.h"
#include "Engine/SkeletalMeshSocket.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "DrawDebugHelpers.h"
#include "../PlayerController/BlasterPlayerController.h"
#include "Blaster/SSR/SSR_FrameHistory.h"       // CVarSSREnabled
#include "Blaster/SSR/SSR_RewindManager.h"      // ProcessHitScanShot / ProcessShotgunPellets
#include "Blaster/GameState/BlasterGameState.h" // GetSSRRewindManager
#include "Blaster/WeaponSystem/Projectile/Projectile.h"

// ════════════════════════════════════════════════════════════════
// P3 开火校验 CVars：DS 上 ~ 控制台热调，无需重编
// 校验逻辑见 ValidateServerFire()：状态/弹药/FireDelay/冷却/时间窗/枪口/射程
// ════════════════════════════════════════════════════════════════

// 服务端开火冷却判定容差（秒）：吸收网络到达抖动，避免合法射击被误判
TAutoConsoleVariable<float> CVarBlasterFireCooldownTolerance(
	TEXT("blaster.Fire.CooldownTolerance"),
	0.05f,
	TEXT("服务端开火冷却判定容差（秒）"),
	ECVF_Default
);

// 拒绝未来 ClientShotTime 的容差（秒）：时钟同步有精度误差，给一点缓冲
TAutoConsoleVariable<float> CVarBlasterFireTimeFutureTolerance(
	TEXT("blaster.Fire.TimeFutureTolerance"),
	0.10f,
	TEXT("拒绝未来 ClientShotTime 的容差（秒）"),
	ECVF_Default
);

// 拒绝过旧 ClientShotTime 的附加窗口：在 ssr.MaxPingCompensation(0.25) 基础上叠加
// 依据：SSR 回退本身把 OneWayDelay clamp 到 [0, MaxPingCompensation]，调老时间无收益
TAutoConsoleVariable<float> CVarBlasterFireTimePastExtra(
	TEXT("blaster.Fire.TimePastExtra"),
	0.05f,
	TEXT("过去时间窗口在 ssr.MaxPingCompensation 上的附加秒数"),
	ECVF_Default
);

// 枪口到角色最大距离（防"伪造枪口到墙另一侧"；仅 SSR 路径 ClientMuzzle 非零时校验）
// 真实枪口距角色中心约 50-150cm，动画/位移去同步余量后上限设为 250cm；
// 原 1000cm 形同虚设——10m 足够跨过绝大多数墙，让枪口伪造几乎不受约束（P2 修复）。
TAutoConsoleVariable<float> CVarBlasterFireMaxMuzzleDist(
	TEXT("blaster.Fire.MaxMuzzleDist"),
	250.f,
	TEXT("枪口到角色最大距离（cm），防伪造枪口；当前上限 250"),
	ECVF_Default
);

// 瞄准合理性：射击方向与玩家视角的最大夹角（度），防"子弹拐弯"
// 注意：服务器旋转由客户端移动复制喂入、可被伪造，此校验是"防呆网"不是硬反作弊——
// 拦 90~180° 的离谱开火（身侧/身后），配合 SSR 视线复核覆盖主要拐弯场景。
TAutoConsoleVariable<float> CVarBlasterFireMaxAimAngleDeg(
	TEXT("blaster.Fire.MaxAimAngleDeg"),
	45.f,
	TEXT("射击方向与视角的最大夹角（度），防子弹拐弯"),
	ECVF_Default
);

// 视角校验模式：1=只记录不拒绝（先上线采集误杀率），0=超阈值直接拒绝
TAutoConsoleVariable<int32> CVarBlasterFireAimAngleLogOnly(
	TEXT("blaster.Fire.AimAngleLogOnly"),
	1,
	TEXT("视角校验模式\n1=只记录不拒绝  0=超阈值直接拒绝"),
	ECVF_Default
);

// 射程校验开关（0=关 1=开）
TAutoConsoleVariable<int32> CVarBlasterFireRangeCheckEnabled(
	TEXT("blaster.Fire.RangeCheckEnabled"),
	1,
	TEXT("射程校验开关\n0=关闭 1=启用"),
	ECVF_Default
);

// 目标到枪口最大距离（cm）：准星射线本就 80000，留出散布余量
TAutoConsoleVariable<float> CVarBlasterFireMaxRange(
	TEXT("blaster.Fire.MaxRange"),
	100000.f,
	TEXT("目标到枪口最大距离（cm）"),
	ECVF_Default
);

// 是否拒绝 bDisableGameplayInput 状态下的开火（安包/拆包/回合准备，严格项）
TAutoConsoleVariable<int32> CVarBlasterFireRejectWhileInputDisabled(
	TEXT("blaster.Fire.RejectWhileInputDisabled"),
	1,
	TEXT("是否拒绝 bDisableGameplayInput 状态下开火\n0=放行 1=拒绝"),
	ECVF_Default
);

UCombatComponent::UCombatComponent()
{

	PrimaryComponentTick.bCanEverTick = true;

	BaseWalkSpeed=600.f;
	AimWalkSpeed=300.f;
}

void UCombatComponent::EquipWeapon(AWeapon *WeaponToEquip)//此函数从始至终都只会在服务器上面执行
{
	if(Character==NULL)return;
	if(WeaponToEquip==NULL)return;
	if (CombatState != ECombatState::ECS_Unoccupied) return;
	EquipPrimaryWeapon(WeaponToEquip);
	Character->GetCharacterMovement()->bOrientRotationToMovement = false;
	Character->bUseControllerRotationYaw = true;
}

void UCombatComponent::EquipPrimaryWeapon(AWeapon* WeaponToEquip)
{
	if (WeaponToEquip == nullptr) return;
	DropEquippedWeapon();
	EquippedWeapon = WeaponToEquip;
	EquippedWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
	AttachActorToRightHand(EquippedWeapon);
	EquippedWeapon->SetOwner(Character);
	EquippedWeapon->SetHUDAmmo();
	PlayEquipWeaponSound(WeaponToEquip);
	ReloadEmptyWeapon();
}

void UCombatComponent::PlayEquipWeaponSound(AWeapon* WeaponToEquip)
{
	if (Character && WeaponToEquip && WeaponToEquip->EquipSound)
	{
		UGameplayStatics::PlaySoundAtLocation(
			this,
			WeaponToEquip->EquipSound,
			Character->GetActorLocation()
		);
	}
}

void UCombatComponent::DropEquippedWeapon()
{
	if (EquippedWeapon)
	{
		EquippedWeapon->Dropped();
	}
}

void UCombatComponent::AttachActorToRightHand(AActor* ActorToAttach)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || ActorToAttach == nullptr) return;
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(FName("RightHandSocket"));
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach, Character->GetMesh());
	}
}

void UCombatComponent::AttachActorToLeftHand(AActor* ActorToAttach)
{
	if (Character == nullptr || Character->GetMesh() == nullptr || ActorToAttach == nullptr || EquippedWeapon == nullptr) return;
	bool bUsePistolSocket =
		EquippedWeapon->GetWeaponType() == EWeaponType::EWT_Pistol ||
		EquippedWeapon->GetWeaponType() == EWeaponType::EWT_SubmachineGun;
	FName SocketName = bUsePistolSocket ? FName("PistolSocket") : FName("LeftHandSocket");
	const USkeletalMeshSocket* HandSocket = Character->GetMesh()->GetSocketByName(SocketName);
	if (HandSocket)
	{
		HandSocket->AttachActor(ActorToAttach, Character->GetMesh());
	}
}

void UCombatComponent::ReloadEmptyWeapon()
{
	if (EquippedWeapon && EquippedWeapon->IsEmpty())
	{
		Reload();
	}
}

void UCombatComponent::Reload()
{
	if (EquippedWeapon && EquippedWeapon->HasSpareAmmo()
		&& CombatState != ECombatState::ECS_Reloading
		&& !EquippedWeapon->IsFull()
		&& !bLocallyReloading)
	{
		if (!Character->HasAuthority())
		{
			HandReload();
			// 客户端预测：乐观装填，避免等 RTT 才看到弹药数字变化
			int32 ReloadAmount = AmountToReload();
			if (ReloadAmount > 0)
			{
				EquippedWeapon->ReloadFromSpare(ReloadAmount);
			}
		}
		ServerReload();
		bLocallyReloading = true;
	}
}

void UCombatComponent::FinishReloading()
{
	if (Character == NULL) return;
	bLocallyReloading = false;
	if (Character->HasAuthority()) {
		CombatState = ECombatState::ECS_Unoccupied;
	}
	if (bFireButtonPressed) {
		Fire();
	}
}

void UCombatComponent::ServerReload_Implementation()//只会在服务器执行
{
	if(Character == NULL||EquippedWeapon==NULL) return;

	int32 ReloadAmount = AmountToReload();
	if (ReloadAmount > 0)
	{
		EquippedWeapon->ReloadFromSpare(ReloadAmount);
	}
	HandReload();
	CombatState = ECombatState::ECS_Reloading;
	bLocallyReloading = true;
}

void UCombatComponent::SetAiming(bool bIsAiming)
{
	bAiming = bIsAiming;
	if (!GetOwner()->HasAuthority())
	{
		ServerSetAiming(bIsAiming);
	}
	if(Character){
		Character->GetCharacterMovement()->MaxWalkSpeed = bIsAiming ? AimWalkSpeed : BaseWalkSpeed;
	}
	// 狙击枪开镜/关镜 Scope Widget（仅本地玩家可见）
	if (Character && Character->IsLocallyControlled() && EquippedWeapon && EquippedWeapon->GetWeaponType() == EWeaponType::EWT_SniperRifle)
	{
		Character->ShowSniperScopeWidget(bIsAiming);
	}
}

void UCombatComponent::OnRep_EquippedWeapon()
{
	if (EquippedWeapon && Character)
	{
		EquippedWeapon->SetWeaponState(EWeaponState::EWS_Equipped);
		AttachActorToRightHand(EquippedWeapon);
		Character->GetCharacterMovement()->bOrientRotationToMovement = false;
		Character->bUseControllerRotationYaw = true;
		PlayEquipWeaponSound(EquippedWeapon);
		EquippedWeapon->EnableCustomDepth(false);
		EquippedWeapon->SetHUDAmmo();
	}
}

void UCombatComponent::FireButtonPressed(bool bPressed)
{
	bFireButtonPressed = bPressed;
	if (bFireButtonPressed)
	{
		Fire();
		if(EquippedWeapon){
			CrosshairShootingFactor = 0.75f;
		}
	}
}

void UCombatComponent::Fire()
{
	if (CanFire())
	{
		bCanFire = false;
		if (EquippedWeapon == nullptr) return;

		if (Character && Character->IsLocallyControlled())
		{
			FHitResult HitResult;
			TraceUnderCrosshairs(HitResult);
			HitTarget = HitResult.ImpactPoint;
		}

		switch (EquippedWeapon->FireType)
		{
		case EFireType::EFT_Projectile:
			FireProjectileWeapon();
			break;
		case EFireType::EFT_HitScan:
			FireHitScanWeapon();
			break;
		case EFireType::EFT_Shotgun:
			FireShotgun();
			break;
		}

		ApplyRecoil();
		StartFireTimer();
	}
}

void UCombatComponent::FireProjectileWeapon()
{
	if (EquippedWeapon && Character)
	{
		HitTarget = EquippedWeapon->bUseScatter ? EquippedWeapon->TraceEndWithScatter(HitTarget, bAiming) : HitTarget;
		LocalFire(HitTarget);
		// 投射物不使用 SSR（有物理飞行时间），但签名保持一致
		float ClientShotTime = 0.f;
		if (Controller) ClientShotTime = Controller->GetServerTime();
		ServerFire(HitTarget, EquippedWeapon->FireDelay, ClientShotTime, FVector::ZeroVector);
	}
}

void UCombatComponent::FireHitScanWeapon()
{
	if (EquippedWeapon && Character)
	{
		HitTarget = EquippedWeapon->bUseScatter ? EquippedWeapon->TraceEndWithScatter(HitTarget, bAiming) : HitTarget;
		LocalFire(HitTarget);
		// 附加客户端估计的服务器时间 + 客户端枪口位置，用于 SSR 延迟补偿
		float ClientShotTime = 0.f;
		if (Controller) ClientShotTime = Controller->GetServerTime();
		// 获取客户端枪口位置：与服务端动画姿态有偏差，SSR 需要客户端数据保证射线一致
		FVector ClientMuzzle = FVector::ZeroVector;
		const USkeletalMeshSocket* MuzzleSocket = EquippedWeapon->GetWeaponMesh()->GetSocketByName("MuzzleFlash");
		if (MuzzleSocket)
		{
			ClientMuzzle = MuzzleSocket->GetSocketTransform(EquippedWeapon->GetWeaponMesh()).GetLocation();
		}
		ServerFire(HitTarget, EquippedWeapon->FireDelay, ClientShotTime, ClientMuzzle);
	}
}

void UCombatComponent::FireShotgun()
{
	AShotgun* Shotgun = Cast<AShotgun>(EquippedWeapon);
	if (Shotgun && Character)
	{
		TArray<FVector_NetQuantize> HitTargets;
		Shotgun->ShotgunTraceEndWithScatter(HitTarget, HitTargets, bAiming);
		if (!Character->HasAuthority()) ShotgunLocalFire(HitTargets);
		// 附加客户端估计的服务器时间 + 客户端枪口位置，用于 SSR 延迟补偿
		float ClientShotTime = 0.f;
		if (Controller) ClientShotTime = Controller->GetServerTime();
		FVector ClientMuzzle = FVector::ZeroVector;
		const USkeletalMeshSocket* MuzzleSocket = EquippedWeapon->GetWeaponMesh()->GetSocketByName("MuzzleFlash");
		if (MuzzleSocket)
		{
			ClientMuzzle = MuzzleSocket->GetSocketTransform(EquippedWeapon->GetWeaponMesh()).GetLocation();
		}
		ServerShotgunFire(HitTargets, EquippedWeapon->FireDelay, ClientShotTime, ClientMuzzle);
	}
}

void UCombatComponent::ShotgunLocalFire(const TArray<FVector_NetQuantize>& TraceHitTargets)
{
	AShotgun* Shotgun = Cast<AShotgun>(EquippedWeapon);
	if (Shotgun == nullptr || Character == nullptr) return;
	if (CombatState == ECombatState::ECS_Unoccupied)
	{
		bLocallyReloading = false;
		Character->PlayFireMontage(bAiming);
		Shotgun->FireShotgun(TraceHitTargets);
	}
}

void UCombatComponent::LocalFire(const FVector_NetQuantize& TraceHitTarget)
{
	if (EquippedWeapon == nullptr) return;
	if (Character)
	{
		//只在本地玩家的客户端上面播放开火动画和特效，不实际生成子弹
		Character->PlayFireMontage(bAiming);
		EquippedWeapon->Fire(TraceHitTarget);
	}
}

void UCombatComponent::ApplyRecoil()
{
	// 纯客户端视觉效果：开火后摄像机视角上跳+随机水平偏移，不影响子弹落点
	// 不实现自动恢复 → 玩家需手动拉鼠标压枪
	if (!Character || !Character->IsLocallyControlled() || !EquippedWeapon) return;

	APlayerController* PC = Character->GetController<APlayerController>();
	if (!PC) return;

	float PitchRecoil = FMath::RandRange(EquippedWeapon->RecoilPitchMin, EquippedWeapon->RecoilPitchMax);
	float YawRecoil  = FMath::RandRange(EquippedWeapon->RecoilYawMin,  EquippedWeapon->RecoilYawMax);

	PC->AddPitchInput(-PitchRecoil); // UE Pitch: 正=低头, 负=抬头 → 取反实现上跳
	PC->AddYawInput(YawRecoil);      // 正值 = 屏幕右偏
}

void UCombatComponent::StartFireTimer()
{
	if (EquippedWeapon == nullptr || Character == nullptr) return;
	Character->GetWorldTimerManager().SetTimer(
		FireTimer,
		this,
		&UCombatComponent::FireTimerFinished,
		EquippedWeapon->FireDelay
	);
}

void UCombatComponent::FireTimerFinished()
{
	if (EquippedWeapon == nullptr) return;
	bCanFire = true;
	if (bFireButtonPressed && EquippedWeapon->bAutomatic)
	{
		Fire();
	}
	ReloadEmptyWeapon();
}

bool UCombatComponent::CanFire()
{
	if (EquippedWeapon == nullptr) return false;
	if (bLocallyReloading) return false;
	return !EquippedWeapon->IsEmpty() && bCanFire && CombatState == ECombatState::ECS_Unoccupied;
}

//从屏幕正中心的准星位置，向 3D 世界里射出一根长达 800 米的"激光指针"，
//看看它碰到了什么，把碰撞点的世界坐标存到 TraceHitResult 里。
void UCombatComponent::TraceUnderCrosshairs(FHitResult& TraceHitResult)
{
	FVector2D ViewportSize;//获取屏幕尺寸
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->GetViewportSize(ViewportSize);
	}
	//算出准星在屏幕上的位置
	FVector2D CrosshairLocation(ViewportSize.X / 2.f, ViewportSize.Y / 2.f);
	FVector CrosshairWorldPosition;
	FVector CrosshairWorldDirection;

	//把屏幕上的 2D 点，转成 3D 世界里的射线
	bool bScreenToWorld = UGameplayStatics::DeprojectScreenToWorld(
		UGameplayStatics::GetPlayerController(this, 0),
		CrosshairLocation,
		CrosshairWorldPosition,//CrosshairWorldPosition：摄像机所在的位置（射线起点）
		CrosshairWorldDirection//CrosshairWorldDirection：摄像机朝向的方向（射线方向）
	);

	if (bScreenToWorld)
	{

		FVector Start = CrosshairWorldPosition;//CrosshairWorldPosition是摄像机的位置

		if (Character)
		{
			float DistanceToCharacter = (Character->GetActorLocation() - Start).Size();

			//把射线起点往前推，跳过玩家自己的身体
			Start += CrosshairWorldDirection * (DistanceToCharacter + 100.f);//CrosshairWorldDirection 是瞄准的方向向量（单位向量）
		}

		//发射射线
		FVector End = Start + CrosshairWorldDirection * 80000.f;

		GetWorld()->LineTraceSingleByChannel(
			TraceHitResult,      // 输出：命中结果（撞到了谁、撞在哪）
			Start,               // 起点
			End,                 // 终点
			ECC_Visibility       // 只检测"可见"通道上的物体
		);

		if (!TraceHitResult.bBlockingHit)//如果射线没有碰到任何物体，就把终点当作命中点
		{
			TraceHitResult.ImpactPoint = End;
			HitTarget = End;
		}
		else
		{
			HitTarget = TraceHitResult.ImpactPoint;
			// DrawDebugSphere(GetWorld(), TraceHitResult.ImpactPoint, 16.f, 12, FColor::Red, false, 2.f);
		}

		// 策略1：取消准星变红。原判定只看"射线撞到角色"，隔墙也会命中角色胶囊体，
		// 泄漏"墙后有人"的信息（玩家本不该知道）。敌我分辨靠头顶名字显示，与准星颜色无关。
		// 恒白即可；HitTarget 的计算在上一段，不受影响。
		HUDPackage.CrosshairsColor = FLinearColor::White;
	}
}

// 每帧将当前武器的准星纹理打包传给 HUD，用于绘制十字准心
void UCombatComponent::SetHUDCrosshairs(float DeltaTime)
{
	if(Character == nullptr) return;

	// 惰性缓存：只在第一次调用时获取 PlayerController，后续复用，避免每帧 Cast
	Controller = Controller == nullptr ? Cast<ABlasterPlayerController>(Character->GetController()) : Controller;

	if(Controller)
	{
		// 从 Controller 拿到 HUD 并转为自定义类型
		HUD = HUD == nullptr ? Cast<ABlasterHud>(Controller->GetHUD()) : HUD;
		if(HUD)
		{

			
			if(EquippedWeapon)
			{
				// 把武器上的五块准星纹理填入数据包，HUD 会根据武器散布动态偏移每块的位置
				HUDPackage.CrosshairsCenter = EquippedWeapon->CrosshairsCenter;
				HUDPackage.CrosshairsLeft = EquippedWeapon->CrosshairsLeft;
				HUDPackage.CrosshairsRight = EquippedWeapon->CrosshairsRight;
				HUDPackage.CrosshairsBottom = EquippedWeapon->CrosshairsBottom;
				HUDPackage.CrosshairsTop = EquippedWeapon->CrosshairsTop;
			}
			else{
				// 没有武器时使用默认准星（可以是全透明的占位图）
				HUDPackage.CrosshairsCenter = nullptr;
				HUDPackage.CrosshairsLeft = nullptr;
				HUDPackage.CrosshairsRight = nullptr;
				HUDPackage.CrosshairsBottom = nullptr;
				HUDPackage.CrosshairsTop = nullptr;
			}
			//计算准星散布
			FVector2D WalkSpreadRange(0.f,Character->GetCharacterMovement()->MaxWalkSpeed);
			FVector2D VelocityMultiplierRange(0.f,1.f);
			FVector Velocity = Character->GetVelocity();
			Velocity.Z = 0.f;//只考虑水平速度对准星散布的影响，垂直速度不影响
			CrosshairVelocityFactor = FMath::GetMappedRangeValueClamped(WalkSpreadRange, VelocityMultiplierRange, Velocity.Size());

			if(Character->GetCharacterMovement()->IsFalling())//如果角色在空中，增加散布
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor, 2.25f, DeltaTime, 2.25f);
			}
			else
			{
				CrosshairInAirFactor = FMath::FInterpTo(CrosshairInAirFactor, 0.f, DeltaTime, 2.25f);
			}

			if(bAiming)//如果角色在瞄准，减少散布
			{
				CrosshairAimFactor = FMath::FInterpTo(CrosshairAimFactor, 0.58f, DeltaTime, 30.f);
			}
			else
			{
				CrosshairAimFactor = FMath::FInterpTo(CrosshairAimFactor, 0.f, DeltaTime, 30.f);
			}

			// 射击散布从当前值平滑回 0，模拟后坐力恢复（不用 timer 而用插值，帧率无关）
			CrosshairShootingFactor = FMath::FInterpTo(CrosshairShootingFactor, 0.f, DeltaTime, 40.f);

			// 最终准星散布 = 基础值 + 移动 + 腾空 - 瞄准 + 射击
			// 瞄准是减项（收束准星），其余是加项（扩大准星）
			HUDPackage.CrosshairsSpread = 0.5f +
			CrosshairVelocityFactor+
			CrosshairInAirFactor-
			CrosshairAimFactor+
			CrosshairShootingFactor;

			// 将数据包交给 HUD，DrawHUD() 下一帧就会用新的纹理绘制准星
			HUD->SetHUDPackage(HUDPackage);
		}
	}
}

void UCombatComponent::InterpFOV(float DeltaTime)
{
	if (EquippedWeapon == nullptr || Character == nullptr) return;
	if (bAiming)
	{
		// 开镜：当前 FOV → 武器的瞄准视野，速度由武器决定（每把枪手感不同）
		CurrentFOV = FMath::FInterpTo(CurrentFOV, EquippedWeapon->GetZoomedFOV(), DeltaTime, EquippedWeapon->GetZoomInterpSpeed());
	}
	else
	{
		// 收镜：当前 FOV → 腰射基准视野，速度由 CombatComponent 统一控制
		CurrentFOV = FMath::FInterpTo(CurrentFOV, DefaultFOV, DeltaTime, ZoomInterpSpeed);
	}
	// 将插值后的结果写入相机
	if(Character && Character->GetFollowCamera())
	{
		Character->GetFollowCamera()->SetFieldOfView(CurrentFOV);
	}
}

void UCombatComponent::MulticastFire_Implementation(const FVector_NetQuantize& TraceHitTarget)
{
	if (Character && Character->IsLocallyControlled()) return;
	LocalFire(TraceHitTarget);
}

void UCombatComponent::ServerFire_Implementation(const FVector_NetQuantize& TraceHitTarget, float FireDelay, float ClientShotTime, const FVector_NetQuantize& ClientMuzzle)
{
	// ── P3 服务器端校验：令牌桶限频 + 参数校验（先于一切处理，拒绝伪造射击）──
	if (!Character) return;

	// 1) 令牌桶限频：恶意客户端绕过客户端 bCanFire 后能以任意频率刷包，服务端必须自行限频
	if (!FireRateBucket.TryConsume(GetWorld()->GetTimeSeconds()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerFire] RATE-LIMITED | %s | tokens=%.2f"),
			*GetNameSafe(Character), FireRateBucket.Tokens);
		return;
	}
	if (!EquippedWeapon) return;

	// 2) 参数校验（状态/弹药/FireDelay/冷却/时间窗/枪口/射程），拒绝即 return 不走 SSR
	TArray<FVector_NetQuantize> Targets;
	Targets.Add(TraceHitTarget);
	if (!ValidateServerFire(FireDelay, ClientShotTime, ClientMuzzle, Targets)) return;

	// ── SSR 延迟补偿命中判定（先于 MulticastFire，避免二次伤害）──
	AHitScanWeapon* HSWeapon = Cast<AHitScanWeapon>(EquippedWeapon); // 只有 hitscan 系武器走 SSR（投射物有物理飞行时间，不走回退）
	ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>(); // 从 GameState 拿 SSR 管理器（帧历史/回退的持有者）
	// 进入 SSR 的四个条件（缺一不可）：
	// ① HSWeapon：武器是 hitscan 家族（Cast 失败 = 投射物 → 跳过 SSR，走旧路径）
	// ② GS：GameState 存在
	// ③ GetSSRRewindManager()：SSR 管理器已初始化（初始化失败/未启用 → 走旧路径）
	// ④ !ClientMuzzle.IsZero()：客户端上报了枪口位置（没上报 → 无法对齐射线起点 → 不走 SSR）
	if (HSWeapon && GS && GS->GetSSRRewindManager() && !ClientMuzzle.IsZero())
	{
		// 诊断日志：记录进入 SSR 的完整输入（射击者/两端位置/客户端时间戳），排查两端偏差用
		UE_LOG(LogTemp, Verbose, TEXT("[SSR] ENTER ServerFire | Shooter=%s | Muzzle=(%.0f,%.0f,%.0f) | HitTarget=(%.0f,%.0f,%.0f) | ClientShotTime=%.3f"),
			*GetNameSafe(Character), ClientMuzzle.X, ClientMuzzle.Y, ClientMuzzle.Z,
			TraceHitTarget.X, TraceHitTarget.Y, TraceHitTarget.Z, ClientShotTime);

		// 核心：回退历史帧 + 纯数学命中判定
		// 内部流程：OneWayDelay = 服务器当前时间 - 客户端预估的开枪时刻
		//           → Clamp 到 [0, 0.25s] → 找到"客户端开枪那一刻"的历史帧
		//           → 用该帧里所有玩家的胶囊体/骨骼快照做射线相交（不碰物理引擎）
		FSSR_TraceResult Result = GS->GetSSRRewindManager()->ProcessHitScanShot(
			Character, HSWeapon, ClientMuzzle, TraceHitTarget, ClientShotTime);

		if (Result.bHit) // 回退帧命中 → SSR 直接结算伤害（这才是"延迟补偿"的成果）
		{
			GS->GetSSRRewindManager()->ApplySSRDamage(Character, EquippedWeapon, Result); // 按命中骨骼名算爆头/平伤，ApplyDamage
			HSWeapon->bSSRHandledShot = true; // 置锁：下面的 MulticastFire→服务器兜底路径看到此标记就跳过，防止同一发结算两次
		}
		else
		{
			// 回退帧没命中 → 什么都不做，落到 MulticastFire 的旧路径
			//（旧路径用"当前帧"射线在服务器再打一次——保证即使回退没中，也有一次判定兜底）
			UE_LOG(LogTemp, Verbose, TEXT("[SSR] EXIT ServerFire | Result: MISS — falling back to original Fire() path"));
		}
	}

	// 视觉效果多播：放在 SSR 之后，bSSRHandledShot 先生效再触发 Fire()
	MulticastFire(TraceHitTarget);
}

void UCombatComponent::ServerShotgunFire_Implementation(const TArray<FVector_NetQuantize>& TraceHitTargets, float FireDelay, float ClientShotTime, const FVector_NetQuantize& ClientMuzzle)
{
	// ── P3 服务器端校验：令牌桶限频 + 参数校验（与 ServerFire 同款）──
	if (!Character) return;

	// 1) 令牌桶限频：与 ServerFire 共用同一桶（同一时刻只装备一把武器，天然互斥）
	if (!FireRateBucket.TryConsume(GetWorld()->GetTimeSeconds()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ServerShotgunFire] RATE-LIMITED | %s | tokens=%.2f"),
			*GetNameSafe(Character), FireRateBucket.Tokens);
		return;
	}
	if (!EquippedWeapon) return;

	// 2) 参数校验，拒绝即 return 不走 SSR
	if (!ValidateServerFire(FireDelay, ClientShotTime, ClientMuzzle, TraceHitTargets)) return;

	// ── SSR 霰弹枪延迟补偿（先于 MulticastShotgunFire，避免二次伤害）──
	AShotgun* ShotgunWeap = Cast<AShotgun>(EquippedWeapon);
	ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>();
	if (ShotgunWeap && GS && GS->GetSSRRewindManager() && !ClientMuzzle.IsZero())
	{
		UE_LOG(LogTemp, Verbose, TEXT("[SSR] ENTER ServerShotgunFire | Shooter=%s | Pellets=%d | ClientShotTime=%.3f"),
			*GetNameSafe(Character), TraceHitTargets.Num(), ClientShotTime);

		TArray<FSSR_TraceResult> Results = GS->GetSSRRewindManager()->ProcessShotgunPellets(
			Character, ShotgunWeap, ClientMuzzle, TraceHitTargets, ClientShotTime);

			// 聚合伤害：与 Shotgun::FireShotgun 相同的按目标累加逻辑
			TMap<ABlasterCharacter*, float> DamageMap;
			for (const FSSR_TraceResult& Result : Results)
			{
				if (!Result.bHit || !Result.HitActor.IsValid()) continue;
				ABlasterCharacter* HitChar = Cast<ABlasterCharacter>(Result.HitActor.Get());
				if (!HitChar) continue;

				const bool bHeadShot = (Result.BoneName == FName("head"));
				const float Dmg = bHeadShot ? EquippedWeapon->GetHeadShotDamage() : EquippedWeapon->GetDamage();
				DamageMap.FindOrAdd(HitChar) += Dmg;
			}

			// 对每个被命中的目标一次性 ApplyDamage
			for (const auto& Pair : DamageMap)
			{
				if (Pair.Key)
				{
					UGameplayStatics::ApplyDamage(
						Pair.Key,
						Pair.Value,
						Character->GetController(),
						EquippedWeapon,
						UDamageType::StaticClass()
					);
				}
			}

			// 仅当至少一颗弹丸命中才置锁（与 hitscan 对齐）：
			// Results 恒有 Num()==弹丸数（含全 miss），若按原逻辑恒置锁，会吞掉
			// MulticastShotgunFire→FireShotgun 的当前帧兜底——全弹丸被墙挡时 0 伤害。
			const bool bAnyPelletHit = Results.ContainsByPredicate(
				[](const FSSR_TraceResult& R) { return R.bHit; });
			if (bAnyPelletHit)
			{
				ShotgunWeap->bSSRHandledShot = true; // 阻止 MulticastShotgunFire→ShotgunLocalFire→FireShotgun() 的二次伤害
			}

			// 汇总日志：几个弹丸命中、命中哪些目标
			int32 HitCount = 0;
			for (const FSSR_TraceResult& R : Results) { if (R.bHit) HitCount++; }
			UE_LOG(LogTemp, Log, TEXT("[SSR] EXIT ServerShotgunFire | %d/%d pellets hit | %d unique targets damaged"),
				HitCount, Results.Num(), DamageMap.Num());
	}

	// 视觉效果多播：放在 SSR 之后，bSSRHandledShot 先生效再触发 FireShotgun()
	MulticastShotgunFire(TraceHitTargets);
}

// ── P3 服务端开火参数校验 ──
// 在 SSR 处理之前调用，全部校验通过才返回 true。被拒绝的射击不会造成任何伤害/特效。
// 设计原则：客户端传的一切参数都不受信任；伤害值本身来自服务端武器配置（GetDamage），
// 但射击频率、射速参数、开火时机、枪口/目标位置都可以被伪造，需要逐项核对。
bool UCombatComponent::ValidateServerFire(float FireDelay, float ClientShotTime,
	const FVector_NetQuantize& ClientMuzzle,
	const TArray<FVector_NetQuantize>& Targets)
{
	if (!Character || !EquippedWeapon) return false;

	// 1) 状态校验：死亡 / 换弹·投掷中 / 安包·回合准备 → 拒绝
	if (Character->IsElimmed()) return false;
	if (CombatState != ECombatState::ECS_Unoccupied) return false;
	if (CVarBlasterFireRejectWhileInputDisabled.GetValueOnGameThread() > 0
		&& Character->bDisableGameplayInput) return false;

	// 2) 弹药校验（服务端权威 Ammo）：空弹匣拒绝——客户端本地 IsEmpty 可被绕过
	if (EquippedWeapon->IsEmpty()) return false;

	// 3) FireDelay 一致性：客户端传的射速必须等于武器配置（篡改射速加速射击 → 拒绝）
	// 什么情况下会不等？客户端修改射速
	if (FMath::Abs(FireDelay - EquippedWeapon->FireDelay) > 0.01f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s FireDelay mismatch %.3f != %.3f"),
			*GetNameSafe(Character), FireDelay, EquippedWeapon->FireDelay);
		return false;
	}

	const float ServerNow = GetWorld()->GetTimeSeconds();

	// 4) 服务端开火冷却：服务端自维护，替代客户端本地 bCanFire/FireTimer（无法绕过）
	if (ServerNow - LastServerFireTime
		< EquippedWeapon->FireDelay - CVarBlasterFireCooldownTolerance.GetValueOnGameThread())
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s cooldown: %.3fs < %.3fs"),
			*GetNameSafe(Character), ServerNow - LastServerFireTime, EquippedWeapon->FireDelay);
		return false;
	}

	//检查 3（FireDelay 一致性）：抓"你报的射速不对"     → 抓说谎
	//检查 4（服务端冷却）：     无视你报什么，自己卡时间 → 抓事实


	// ── 5) ClientShotTime 时间窗校验（硬边界）：拒未来 / 拒超期 ──
	// 前提：必须先完成时钟同步（HasSyncedServerTime）才启用本校验——
	// 否则 ClientShotTime 尚未校准（热身期/刚进场），直接校验会误杀合法射击。
	ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(Character->GetController());
	if (PC && PC->HasSyncedServerTime())
	{
		// MaxPast = 0.25s（回退上限）+ 0.05s（附加容差）= "允许的最旧时刻"
		// 依据：超过回退窗口的时间戳无收益（反正会被 SSR Clamp 到 0.25），
		// 传更早的时间戳 = 纯伪造 → 直接拒（这就是"硬边界"，区别于回退时的软 Clamp）
		const float MaxPast = CVarSSRMaxPingCompensation.GetValueOnGameThread()
			+ CVarBlasterFireTimePastExtra.GetValueOnGameThread();

		// 拒绝"未来时间"：ClientShotTime > 服务器现在 + 容差
		// 客户端不可能在"未来"开枪（FutureTolerance 兜住时钟同步误差）
		// 报未来 = 伪造（想用回退打"未来位置"的敌人）
		if (ClientShotTime > ServerNow + CVarBlasterFireTimeFutureTolerance.GetValueOnGameThread())
		{
			UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s future shot time %.3f"),
				*GetNameSafe(Character), ClientShotTime);
			return false;   // 拒这枪
		}

		// 拒绝"超期时间"：ClientShotTime < 服务器现在 - MaxPast
		// 超过 0.25+容差 = 正常网络不可能这么旧（伪造/时钟严重错乱）
		if (ClientShotTime < ServerNow - MaxPast)
		{
			UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s stale shot time %.3f"),
				*GetNameSafe(Character), ClientShotTime);
			return false;   // 拒这枪
		}
	}

	// 6) 枪口合理性（仅 SSR 路径 ClientMuzzle 非零时）：枪口必须贴近角色（防隔墙开枪）
	if (!ClientMuzzle.IsZero())
	{
		const float MuzzleToActorDist = (ClientMuzzle - Character->GetActorLocation()).Size();
		if (MuzzleToActorDist > CVarBlasterFireMaxMuzzleDist.GetValueOnGameThread())
		{
			UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s muzzle far from actor %.0fcm"),
				*GetNameSafe(Character), MuzzleToActorDist);
			return false;
		}
	}

	// 7) 瞄准合理性（防子弹拐弯，默认 log-only）：射击方向必须在玩家视角附近
	// 作弊者可报任意不在准星里的 HitTarget（身侧/身后敌人）让子弹"拐弯"；
	// 服务器旋转由客户端移动复制喂入、可被伪造，此校验是"防呆网"不是硬反作弊——
	// 拦 90~180° 的离谱开火，配合 SSR 视线复核覆盖主要拐弯场景。
	// 默认只记录不拒绝（AimAngleLogOnly=1），跑局确认无误杀后再切硬拒绝。
	// 仅 SSR 路径（ClientMuzzle 非零）生效：投射物不进 SSR、由服务器持有碰撞，天然跳过。
	if (!ClientMuzzle.IsZero() && PC && PC->HasSyncedServerTime())
	{
		const FVector ViewForward = Character->GetControlRotation().Vector();
		const float MaxAimAngle = FMath::DegreesToRadians(CVarBlasterFireMaxAimAngleDeg.GetValueOnGameThread());
		for (const FVector_NetQuantize& T : Targets)
		{
			const FVector Dir = T - ClientMuzzle;
			if (Dir.IsNearlyZero()) continue;
			const float Angle = FMath::Acos(FMath::Clamp(Dir.GetSafeNormal() | ViewForward, -1.f, 1.f));
			if (Angle > MaxAimAngle)
			{
				const bool bReject = CVarBlasterFireAimAngleLogOnly.GetValueOnGameThread() <= 0;
				UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s aim off-view %.1f° (max %.0f°) %s"),
					*GetNameSafe(Character), FMath::RadiansToDegrees(Angle),
					CVarBlasterFireMaxAimAngleDeg.GetValueOnGameThread(),
					bReject ? TEXT("REJECTED") : TEXT("LOG-ONLY"));
				if (bReject)
				{
					return false;
				}
			}
		}
	}

	// 8) 射程校验（可选）：所有目标到枪口距离 ≤ 射程
	if (CVarBlasterFireRangeCheckEnabled.GetValueOnGameThread() > 0 && !ClientMuzzle.IsZero())
	{
		const float MaxRange = CVarBlasterFireMaxRange.GetValueOnGameThread();
		for (const FVector_NetQuantize& T : Targets)
		{
			if ((T - ClientMuzzle).Size() > MaxRange)
			{
				UE_LOG(LogTemp, Warning, TEXT("[FireValidation] %s target beyond range %.0fcm"),
					*GetNameSafe(Character), (T - ClientMuzzle).Size());
				return false;
			}
		}
	}

	// 全部通过 → 推进服务端冷却（仅最终放行时推进；被拒不阻塞后续合法射击）
	LastServerFireTime = ServerNow;
	LastServerFireClientTime = ClientShotTime;
	return true;
}

void UCombatComponent::MulticastShotgunFire_Implementation(const TArray<FVector_NetQuantize>& TraceHitTargets)
{
	if (Character && Character->IsLocallyControlled() && !Character->HasAuthority()) return;
	ShotgunLocalFire(TraceHitTargets);
}

void UCombatComponent::ServerSetAiming_Implementation(bool bIsAiming)
{
	bAiming = bIsAiming;
	if(Character){
		Character->GetCharacterMovement()->MaxWalkSpeed = bIsAiming ? AimWalkSpeed : BaseWalkSpeed;
	}
}

void UCombatComponent::BeginPlay()
{
	Super::BeginPlay();
	if(Character){
		Character->GetCharacterMovement()->MaxWalkSpeed = BaseWalkSpeed;

		if(Character->GetFollowCamera())
		{
			// 记录相机原始 FOV 作为腰射基准，后续收镜时恢复到此值
			DefaultFOV = Character->GetFollowCamera()->FieldOfView;
			CurrentFOV = DefaultFOV;
		}
	}
	
}


void UCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	

	// 只对本地玩家运行：射线检测、准星绘制、FOV 平滑
	if (Character && Character->IsLocallyControlled() && EquippedWeapon)
	{
		SetHUDCrosshairs(DeltaTime);
		FHitResult HitResult;
		TraceUnderCrosshairs(HitResult);
		HitTarget = HitResult.ImpactPoint;
		
		InterpFOV(DeltaTime);
	}

}

void UCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty> &OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UCombatComponent, EquippedWeapon);
	DOREPLIFETIME(UCombatComponent, bAiming);
	DOREPLIFETIME(UCombatComponent, CombatState);
}

void UCombatComponent::OnRep_CombatState()
{
	if (Character == NULL) return;
	switch (CombatState)
	{
	case ECombatState::ECS_Reloading:
		if (Character && !Character->IsLocallyControlled()) HandReload();
		break;
	case ECombatState::ECS_Unoccupied:
		if (bFireButtonPressed)
		{
			Fire();
		}
		break;
	}
}
void  UCombatComponent::HandReload()
{
	Character->PlayReloadMontage();
}

int32 UCombatComponent::AmountToReload()
{
	if (EquippedWeapon == nullptr) return 0;
	int32 RoomInMag = EquippedWeapon->GetMagCapacity() - EquippedWeapon->GetAmmo();
	int32 AmountSpare = EquippedWeapon->GetSpareAmmo();
	return FMath::Min(RoomInMag, AmountSpare);
}

bool UCombatComponent::PickupAmmo(EWeaponType WeaponType, int32 AmmoAmount)
{
	// 只有装备了同类型武器才添加备弹（CS:GO 规则：子弹类型必须匹配当前枪支）
	if (EquippedWeapon && EquippedWeapon->GetWeaponType() == WeaponType)
	{
		EquippedWeapon->AddToSpare(AmmoAmount);

		// 类型匹配：如果弹匣为空，自动触发换弹（用户体验优化）
		if (EquippedWeapon->IsEmpty())
		{
			Reload();
		}
		return true; // 拾取成功，通知调用方可以销毁拾取物
	}

	return false; // 拾取失败（无装备武器或类型不匹配），通知调用方显示提示
}
