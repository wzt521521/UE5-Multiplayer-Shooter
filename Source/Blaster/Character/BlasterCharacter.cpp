// Fill out your copyright notice in the Description page of Project Settings.
#include "BlasterCharacter.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/CapsuleComponent.h"
#include "Net/UnrealNetwork.h"
#include "../WeaponSystem/Weapon/Weapon.h"
#include "../BlasterComponents/CombatComponent.h"
#include "Blaster/BlasterComponents/BuffComponent.h"
#include "Blaster/BlasterComponents/ThrowableComponent.h"
#include "Blaster/BombMode/BombInteractionComponent.h"  // Q键安包/拆包
#include "Kismet/GameplayStatics.h"
#include "Blaster/Blaster.h"
#include "Kismet/KismetMathLibrary.h"
#include "BlasterAnimInstance.h"
#include "../PlayerController/BlasterPlayerController.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Blaster/GameMode/BombDefusalGameMode.h"
#include "TimerManager.h"
#include "Blaster/WeaponSystem/Weapon/WeaponTypes.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/Pickups/AmmoPickup.h"  // OverlappingAmmo 追踪 + ServerPickupAmmo RPC
#include "Blaster/SSR/SSR_FrameHistory.h"  // GetRelevantBoneNames()

// static 成员定义（连接级共享的到达间隔累积缓冲区：OnRep_ReplicatedMovement 喂入、PC 读取）
TArray<float> ABlasterCharacter::PendingIntervals;

// ── 采样间隔过滤阈值（Phase 2 数据清洗）──
// WHY：服务器对静止的 simulated proxy 不发移动复制（实测静止→移动首包间隔 18.5s/129s），
// 若入缓冲会把第一秒 jitter 假顶到 1204~3601ms（实测），τ 被顶到上限再回落。
// 同时挡住"切图幻影"（Lobby 人齐瞬间给最后加入者生成的角色随 ServerTravel 立即销毁，
// 其唯一复制包仍会到达已在旧世界的客户端，实测 07.53.32:543 一次性到达）。
// 500ms 远大于注入 150ms/抖动 30ms 下相邻包最大合法间隔（≈75ms），不会误伤真抖动。
TAutoConsoleVariable<float> CVarBlasterNetSmoothMaxIntervalMs(
	TEXT("blaster.NetSmooth.Adaptive.MaxIntervalMs"),
	500.f,
	TEXT("采样间隔上限（ms）：超过视为移动间隙/切图幻影，只重置时间戳不入缓冲"),
	ECVF_Default
);

ABlasterCharacter::ABlasterCharacter(const FObjectInitializer& ObjectInitializer)
	// P3 移动校验：把引擎默认 UCharacterMovementComponent 替换为子类。
	// 必须用 SetDefaultSubobjectClass（不能用同名 CreateDefaultSubobject 重建，
	// 会触发 Development 重复构造保护），这样 ACharacter 基类构造时创建的就是校验子类。
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UBlasterCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(GetMesh());
	CameraBoom->TargetArmLength = 600.f;
	CameraBoom->bUsePawnControlRotation = true;

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	bUseControllerRotationYaw=false;
	GetCharacterMovement()->bOrientRotationToMovement = true;

	OverheadWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("OverheadWidget"));
	OverheadWidget->SetupAttachment(RootComponent);

	Combat= CreateDefaultSubobject<UCombatComponent>(TEXT("CombatComponent"));
	Combat->SetIsReplicated(true);

	Buff = CreateDefaultSubobject<UBuffComponent>(TEXT("BuffComponent"));
	Buff->SetIsReplicated(true);

	Throwable = CreateDefaultSubobject<UThrowableComponent>(TEXT("ThrowableComponent"));
	Throwable->SetIsReplicated(true);

	// 炸弹交互组件：Q键安包/拆包（BombMode Phase 2）
	BombInteraction = CreateDefaultSubobject<UBombInteractionComponent>(TEXT("BombInteractionComponent"));

	GetCharacterMovement()->NavAgentProps.bCanCrouch = true;
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
	// 胶囊体必须阻断 Visibility 射线——骨骼物理体之间有缝隙，胶囊体作为兜底确保射线不会穿透角色
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block);
	GetMesh()->SetCollisionObjectType(ECC_SkeletalMesh);
	GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Camera, ECollisionResponse::ECR_Ignore);
	GetMesh()->SetCollisionResponseToChannel(ECollisionChannel::ECC_Visibility, ECollisionResponse::ECR_Block);
	// DS 上无渲染，默认 OnlyTickPoseWhenRendered 会导致动画不更新，武器 Socket 位置错误
	// 设为 AlwaysTick 确保服务器端骨骼姿态与客户端一致，SSR 射线才准确
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	GetCharacterMovement()->RotationRate = FRotator(0.f, 0.f, 850.f);
	TurningInPlace = ETurningInPlace::ETIP_NotTurning;

	NetUpdateFrequency = 66.f;
	MinNetUpdateFrequency = 33.f;


}


// ════════════════════════════════════════════════════════════════
// Phase 2 采样源：客户端 simulated proxy 收到移动复制（ReplicatedMovement）时调用
// 记录到达墙钟时间，算"本角色自己的到达间隔"喂入 static 累积缓冲区。
// ★ 用实例成员 LastSnapshotArrivalTime（每角色独立），不能用 static 串所有角色——
//   同批 UDP 包里多个角色的到达只差 ~0.3ms（处理时间），与跨 tick 的真实间隔交替，
//   会产生确定性伪抖动淹没真实网络抖动（多人场景定时炸弹）。
// ════════════════════════════════════════════════════════════════
void ABlasterCharacter::OnRep_ReplicatedMovement()
{
	Super::OnRep_ReplicatedMovement();

	// 只统计远端角色（simulated proxy）的到达；本地角色走预测不走此路径。
	// 实测整场零 Role=2 触发：主人连接的自己角色 ReplicatedMovement 不会到达，此守卫是双保险。
	if (GetLocalRole() != ROLE_SimulatedProxy) return;

	const double Now = GetWorld()->GetTimeSeconds();
	if (LastSnapshotArrivalTime >= 0.0)
	{
		const float Interval = (float)(Now - LastSnapshotArrivalTime);

		// ── 间隔过滤：>上限 的不是网络抖动，而是"静止→开始移动"的停顿 ──
		// 服务器对静止角色不发移动复制（实测 18.5s/129s），恢复移动的首包混入超长间隔
		// → jitter 假值（实测 1204/1597/3601ms）→ τ 被顶到上限。
		// 策略：丢弃该样本、但照常重置时间戳——停顿不算抖动，时钟基准照走。
		// Interval<=0（同帧多包突发/切图加载期世界时间冻结）静默丢弃不打日志——实测会刷屏。
		const float MaxInterval = CVarBlasterNetSmoothMaxIntervalMs.GetValueOnGameThread() / 1000.f;
		if (Interval > 0.f && Interval <= MaxInterval)
		{
			// 本角色自己的间隔，不受其他角色交错污染
			PendingIntervals.Add(Interval);
		}
		else if (Interval > MaxInterval)
		{
			// 过滤生效证据（低频，保留）：挡掉了一次移动间隙/切图幻影
			UE_LOG(LogTemp, Log, TEXT("[NetSmooth] 丢弃超长间隔：%s | 间隔=%.3fs（上限 %.0fms，视为移动间隙/切图幻影）"),
				*GetName(), Interval, CVarBlasterNetSmoothMaxIntervalMs.GetValueOnGameThread());
		}
	}
	LastSnapshotArrivalTime = Now;
}
void ABlasterCharacter::BeginPlay()
{
	Super::BeginPlay();
	UpdateHUDHealth();
	UpdateHUDShield();
	// ————————————————————————————————————————————
	// 血量 UI 初始化：把初始血量发送到屏幕上的血条
	// 这里的调用时机是 BeginPlay（只执行一次），所以只负责 UI 初始显示
	// 后续血量变化由 OnRep_Health 驱动
	// ————————————————————————————————————————————
	
	if(HasAuthority()){
		OnTakeAnyDamage.AddDynamic(this,&ABlasterCharacter::ReceiveDamage);
	}

	// 初始化 SSR 骨骼追踪列表（从 USSR_FrameHistory 的静态方法获取）
	RelevantBoneNames = USSR_FrameHistory::GetRelevantBoneNames();
}

void ABlasterCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	PlayerInputComponent->BindAction("Jump", IE_Pressed, this, &ABlasterCharacter::Jump);

	PlayerInputComponent->BindAxis("MoveForward", this, &ABlasterCharacter::MoveForward);
	PlayerInputComponent->BindAxis("MoveRight", this, &ABlasterCharacter::MoveRight);
	PlayerInputComponent->BindAxis("Turn", this, &ABlasterCharacter::Turn);
	PlayerInputComponent->BindAxis("LookUp", this, &ABlasterCharacter::LookUp);

	PlayerInputComponent->BindAction("Equip", IE_Pressed, this, &ABlasterCharacter::EquipButtonPressed);

	PlayerInputComponent->BindAction("Crouch", IE_Pressed, this, &ABlasterCharacter::CrouchButtonPressed);
	PlayerInputComponent->BindAction("Aim", IE_Pressed, this, &ABlasterCharacter::AimButtonPressed);
	PlayerInputComponent->BindAction("Aim", IE_Released, this, &ABlasterCharacter::AimButtonReleased);
	PlayerInputComponent->BindAction("Fire", IE_Pressed, this, &ABlasterCharacter::FireButtonPressed);
	PlayerInputComponent->BindAction("Fire", IE_Released, this, &ABlasterCharacter::FireButtonReleased);

	PlayerInputComponent->BindAction("Reload", IE_Pressed, this, &ABlasterCharacter::ReloadButtonPressed);

	PlayerInputComponent->BindAction("ThrowableWheel", IE_Pressed, this, &ABlasterCharacter::ThrowableWheelToggle);

	// Q键：安包/拆包交互（Hold-to-interact：按下开始、松开取消）
	PlayerInputComponent->BindAction("BombInteract", IE_Pressed, this, &ABlasterCharacter::BombInteractPressed);
	PlayerInputComponent->BindAction("BombInteract", IE_Released, this, &ABlasterCharacter::BombInteractReleased);
}

void ABlasterCharacter::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	if(Combat)
	{
		Combat->Character = this;//让Combat组件知道它所属的角色是谁
	}

	// 记录角色原始移动属性，Buff 到期后恢复
	if (Buff && GetCharacterMovement())
	{
		Buff->SetInitialSpeeds(GetCharacterMovement()->MaxWalkSpeed, GetCharacterMovement()->MaxWalkSpeedCrouched);
		Buff->SetInitialJumpVelocity(GetCharacterMovement()->JumpZVelocity);
	}

	if (Throwable)
	{
		Throwable->Character = this;
	}
}

void ABlasterCharacter::PlayFireMontage(bool bAiming)
{
	if(Combat==NULL||Combat->EquippedWeapon==NULL)return;
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();//AnimInstance 是角色网格体的动画实例，负责播放动画蒙太奇
	if(AnimInstance && FireWeaponMontage)
	{
		AnimInstance->Montage_Play(FireWeaponMontage);
		FName SectionName = bAiming ? FName("RifleAim") : FName("RifleHip");
		AnimInstance->Montage_JumpToSection(SectionName);
	}

}

void ABlasterCharacter::PlayReloadMontage()
{
	if(Combat==NULL||Combat->EquippedWeapon==NULL)return;
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
	if(AnimInstance && ReloadMontage)
	{
		AnimInstance->Montage_Play(ReloadMontage);
		FName SectionName;
		switch(Combat->EquippedWeapon->GetReloadType()){
			case EWeaponType::EWT_AssaultRifle:
				SectionName = FName("Rifle");
				break;
			case EWeaponType::EWT_RocketLauncher:
				SectionName = FName("RocketLauncher");
				break;
			case EWeaponType::EWT_SubmachineGun:
				SectionName = FName("Pistol");
				break;
			case EWeaponType::EWT_SniperRifle:
				SectionName = FName("SniperRifle");
				break;
			case EWeaponType::EWT_GrenadeLauncher:
				SectionName = FName("GrenadeLauncher");
				break;
			default:
				SectionName = FName("Rifle");
				break;
		}
		AnimInstance->Montage_JumpToSection(SectionName);
	}
}

void ABlasterCharacter::PlayHitReactMontage()
{
	if(Combat == NULL) return; // 受击动画不应要求武器——重生后未拾取武器时也需要播放
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
	if(AnimInstance && HitReactMontage)
	{
		AnimInstance->Montage_Play(HitReactMontage);
		FName SectionName = FName("FromFront");
		AnimInstance->Montage_JumpToSection(SectionName);
	}
}

void ABlasterCharacter::PlayElimMontage()//只负责播放动画
{
	UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
	if(AnimInstance && ElimMontage)
	{
		AnimInstance->Montage_Play(ElimMontage);
	}
}

void ABlasterCharacter::Elim()
{
	// 掉落所有武器
	DropOrDestroyWeapons();
	MulticastElim();
	// 回合制模式不自动复活：由 GameMode 在新回合 CleanupBodiesAndRespawn 中统一处理
	// Deathmatch 模式依然走 ElimTimer → ElimTimerFinished → RequestRespawn
	ABombDefusalGameMode* TDMGameMode = GetWorld()->GetAuthGameMode<ABombDefusalGameMode>();
	if (!TDMGameMode)
	{
		GetWorldTimerManager().SetTimer(
			ElimTimer,
			this,
			&ABlasterCharacter::ElimTimerFinsished,
			ElimDelay
		);
	}
	else
	{
		// Bomb 模式：死亡 ElimDelay（3s）后销毁尸体。
		// 配合死亡镜头：客户端看自己尸体 3s → 尸体销毁（复制到客户端）→ 切队友视角。
		// 若不销毁，尸体残留到 CleanupBodiesAndRespawn（下回合）才消失。
		GetWorldTimerManager().SetTimer(
			ElimTimer,
			this,
			&ABlasterCharacter::DestroyCorpse,
			ElimDelay
		);
	}
}

// Bomb 模式：尸体销毁（服务器权威，Elim 仅在服务器执行）。
// 先 UnPossess（尸体仍被服务器 PC Possess —— ShouldKeepCurrentPawnUponSpectating=true 保留），
// 再 Destroy → 复制到客户端 → 客户端 GetPawn() 变 null → 死亡镜头结束自动切队友视角。
void ABlasterCharacter::DestroyCorpse()
{
	if (AController* C = Controller)
	{
		C->UnPossess();
	}
	Destroy();
}

void ABlasterCharacter::MulticastElim_Implementation()//MulticastElim只负责多播，其他逻辑由另一个Elim函数处理
{
	if(BlasterPlayerController){
		BlasterPlayerController->SetHUDWeaponAmmo(0);
	}
	bElimmed = true;
	PlayElimMontage();

	// 死亡时关闭狙击镜 Scope Widget
	if (IsLocallyControlled() && Combat && Combat->EquippedWeapon && Combat->EquippedWeapon->GetWeaponType() == EWeaponType::EWT_SniperRifle)
	{
		ShowSniperScopeWidget(false);
	}

	//禁用碰撞
	GetCharacterMovement()->DisableMovement();
	GetCharacterMovement()->StopMovementImmediately();
	if(BlasterPlayerController){
		DisableInput(BlasterPlayerController);
	}
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);//禁用碰撞
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);//禁用碰撞
}

void ABlasterCharacter::ElimTimerFinsished()
{
	ABlasterGameMode* BlasterGameMode = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
	if(BlasterGameMode)
	{
		BlasterGameMode->RequestRespawn(this, Controller);//调用复活
	}
}

void ABlasterCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty> &OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(ABlasterCharacter, OverlappingWeapon, COND_OwnerOnly);//注册要复制的变量
	DOREPLIFETIME_CONDITION(ABlasterCharacter, OverlappingAmmo, COND_OwnerOnly);  // 弹药重叠仅持有者感知
	DOREPLIFETIME(ABlasterCharacter, Health);
	DOREPLIFETIME(ABlasterCharacter, bDisableGameplayInput);
	DOREPLIFETIME(ABlasterCharacter, Shield);
	DOREPLIFETIME(ABlasterCharacter, bWaitingForNextRound);
}

void ABlasterCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	AimOffset(DeltaTime);
	HideCameraIfCharacterClose();
	PollInit();
}



void ABlasterCharacter::MoveForward(float Value)
{
		if (bDisableGameplayInput) return;
	if(Controller != nullptr && Value != 0.f)
	{
		const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
		const FVector Direction = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		AddMovementInput(Direction, Value);
	}
}

void ABlasterCharacter::MoveRight(float Value)
{
		if (bDisableGameplayInput) return;
	if(Controller != nullptr && Value != 0.f)
	{
		const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
		const FVector Direction = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);
		AddMovementInput(Direction, Value);
	}
}

void ABlasterCharacter::Turn(float Value)
{
	AddControllerYawInput(Value);
}

void ABlasterCharacter::LookUp(float Value)
{
	AddControllerPitchInput(Value);
}

void ABlasterCharacter::EquipButtonPressed()
{
		if (bDisableGameplayInput) return;
	// E 键统管武器拾取和弹药拾取，战斗状态空闲时才允许操作
	if (Combat && Combat->CombatState == ECombatState::ECS_Unoccupied)
	{
		// 捡武器时退出投掷模式
		if (OverlappingWeapon && Throwable && Throwable->IsThrowableEquipped())
		{
			Throwable->UnequipThrowable();
		}

		if (OverlappingWeapon)
		{
			ServerEquipButtonPressed();
		}
		else if (OverlappingAmmo)
		{
			// 客户端预测：乐观添加备弹，避免等 RTT 才看到数字变化
			if (!HasAuthority() && Combat && Combat->EquippedWeapon && Combat->EquippedWeapon->GetWeaponType() == OverlappingAmmo->GetWeaponType())
			{
				Combat->EquippedWeapon->AddToSpare(OverlappingAmmo->GetAmmoAmount());
			}
			ServerPickupAmmo();
		}
		else if (BombInteraction && BombInteraction->CanPickup())
		{
			// 炸弹拾取：E 键与武器/弹药拾取统一入口，
			// 服务器权威校验（攻方身份/未带包/掉落状态/距离）交给组件内部 Server_PickupBomb
			BombInteraction->PickupDroppedBomb();
		}
	}
}

void ABlasterCharacter::CrouchButtonPressed()
{
		if (bDisableGameplayInput) return;
	if(bIsCrouched){
		UnCrouch();
	}
	else{
		Crouch();
	}
	
}

void ABlasterCharacter::ReloadButtonPressed()
{
		if (bDisableGameplayInput) return;
	if(Combat)
	{
		Combat->Reload();
	}
}

void ABlasterCharacter::AimButtonPressed()
{
		if (bDisableGameplayInput) return;
	if(Combat)
	{
		Combat->SetAiming(true);
	}
}

void ABlasterCharacter::AimButtonReleased()
{
		if (bDisableGameplayInput) return;
	if(Combat)
	{
		Combat->SetAiming(false);
	}
}

void ABlasterCharacter::AimOffset(float DeltaTime)//AimOffset 在每台机器上都跑
//包括服务器和所有客户端。所以每台机器都在本地算自己的 AO_Pitch，都在本地做同样的修正，最终都修正到了正确的值。
{
	if(Combat && Combat->EquippedWeapon ==NULL)return;
	FVector Velocity = GetVelocity();
	Velocity.Z = 0;
	//计算 Speed（水平速度）和 bIsInAir
	float Speed = Velocity.Size();
	bool bIsInAir = GetCharacterMovement()->IsFalling();


	if(Speed == 0.f ||! bIsInAir)//不在空中
	{
		//CurrentAimRotation = 当前瞄准 Yaw
		FRotator CurrentAimRotation = FRotator(0.f,GetBaseAimRotation().Yaw,0.f);
		//DeltaAimRotation = CurrentAimRotation - StartAimRotation
		FRotator DeltaAimRotation = UKismetMathLibrary::NormalizedDeltaRotator(CurrentAimRotation,StartAimRotation);
		//AO_Yaw = DeltaAimRotation.Yaw          ← 瞄准方向偏离基准的角度
		AO_Yaw = DeltaAimRotation.Yaw;
		//if (不在转身中): InterpAO_Yaw = AO_Yaw ← 保存原始值
		if(TurningInPlace==ETurningInPlace::ETIP_NotTurning){
			InterpAO_Yaw = AO_Yaw;
		}
		bUseControllerRotationYaw = true;//角色旋转模式：控制器旋转，不使用角色旋转
		TurnInPlace(DeltaTime);//判断是否触发转身
	}
	if(Speed>0.f||bIsInAir){
		StartAimRotation = FRotator(0.f,GetBaseAimRotation().Yaw,0.f);
		AO_Yaw=0.f;
		bUseControllerRotationYaw = true;
		TurningInPlace=ETurningInPlace::ETIP_NotTurning;
	}
	AO_Pitch=GetBaseAimRotation().Pitch;
	//角色上下镜头移动的时候，ue会自动复制底层的pitch角度，然后再自动复制底层角度到其他的机器
	//其他的机器再通过AO_Pitch=GetBaseAimRotation().Pitch;来赋值AO_Pitch，以期望更新角度
	//但是GetBaseAimRotation().Pitch在其他机器的解码环节会出现问题，导致AO_Pitch的值不对
	//fix 做的就是这件事：把 270°-360° 这种"无符号低头"映射回正确的 -90°-0°。
	if(AO_Pitch>90.f&&!IsLocallyControlled()){
		FVector2D InRange(270.f,360.f);
		FVector2D OutRange(-90.f,0.f);
		AO_Pitch=FMath::GetMappedRangeValueClamped(InRange,OutRange,AO_Pitch);
	}
}

void ABlasterCharacter::Jump()
{
		if (bDisableGameplayInput) return;
	if(bIsCrouched){
		UnCrouch();
	}
	else{
		Super::Jump();
	}
}

void ABlasterCharacter::FireButtonPressed()
{
		if (bDisableGameplayInput) return;
	// 投掷模式优先：按住左键蓄力而非开火
	if (Throwable && Throwable->IsThrowableEquipped())
	{
		Throwable->StartCooking();
		return;
	}
	if (Combat)
	{
		Combat->FireButtonPressed(true);
	}
}

void ABlasterCharacter::FireButtonReleased()
{
		if (bDisableGameplayInput) return;
	// 投掷模式优先：松开左键投出
	if (Throwable && Throwable->IsCooking())
	{
		Throwable->ExecuteThrow();
		return;
	}
	if (Combat)
	{
		Combat->FireButtonPressed(false);
	}
}

//这个函数只会在服务器上面调用
void ABlasterCharacter::ReceiveDamage(AActor *DamagedActor, float Damage, const UDamageType *DamageType, AController *InstigatorController, AActor *DamageCauser)
{
	// ————————————————————————————————————————————
	// 护盾存在 → 20% 免伤 + 抵挡破碎一击（溢出不扣血）
	// 护盾已破 → 全额扣血
	// ————————————————————————————————————————————
	float DamageToHealth = Damage;
	if (Shield > 0.f)
	{
		const float ReducedDamage = Damage * 0.8f;
		Shield = FMath::Clamp(Shield - ReducedDamage, 0.f, MaxShield);
		DamageToHealth = 0.f;  // 溢出不扣血，护盾扛下全部
	}

	Health = FMath::Clamp(Health - DamageToHealth, 0.f, MaxHealth);
	UpdateHUDHealth();
	UpdateHUDShield();

	// 受伤打断安包/拆包（BombMode：服务器权威取消交互）
	if (BombInteraction && BombInteraction->IsInteracting())
	{
		BombInteraction->ForceCancelInteraction();
	}

	PlayHitReactMontage();

	if(Health <= 0.f){
		BlasterPlayerController = BlasterPlayerController == nullptr ? Cast<ABlasterPlayerController>(Controller) : BlasterPlayerController;
		ABlasterPlayerController* AttackerController = Cast<ABlasterPlayerController>(InstigatorController);
		// 优先路由到回合制 GameMode（新逻辑），退回到 Deathmatch GameMode（旧逻辑）
		ABombDefusalGameMode* TDMGameMode = GetWorld()->GetAuthGameMode<ABombDefusalGameMode>();
		if (TDMGameMode)
		{
			TDMGameMode->OnPlayerKilled(this, BlasterPlayerController, AttackerController);
		}
		else
		{
			ABlasterGameMode* BlasterGameMode = GetWorld()->GetAuthGameMode<ABlasterGameMode>();
			if (BlasterGameMode)
			{
				BlasterGameMode->PlayerEliminated(this, BlasterPlayerController, AttackerController);
			}
		}
	}
	
}

void ABlasterCharacter::UpdateHUDHealth()
{
	BlasterPlayerController = BlasterPlayerController == nullptr ? Cast<ABlasterPlayerController>(Controller) : BlasterPlayerController;
	if (BlasterPlayerController)
	{
		BlasterPlayerController->SetHUDHealth(Health, MaxHealth);
	}
}

void ABlasterCharacter::PollInit()
{
	if (BlasterPlayerController == nullptr)
	{
		BlasterPlayerController = BlasterPlayerController == nullptr ? Cast<ABlasterPlayerController>(Controller) : BlasterPlayerController;
		if (BlasterPlayerController)
		{
			SpawDefaultWeapon();
			UpdateHUDHealth();
			UpdateHUDShield();
		}
	}
}

void ABlasterCharacter::OnRep_OverlappingWeapon(AWeapon* LastWeapon)//参数自动传入
//当客户端收到服务器传来的新值时，客户端会执行这个函数
{
	if(OverlappingWeapon)
	{
		OverlappingWeapon->ShowPickupWidget(true);
	}

	if(LastWeapon)
	{
		LastWeapon->ShowPickupWidget(false);
	}
}

// 弹药拾取物 OnRep：复制驱动客户端显示/隐藏拾取提示 UI
// 与 OnRep_OverlappingWeapon 相同的模式：新拾取物显示，旧拾取物隐藏
void ABlasterCharacter::OnRep_OverlappingAmmo(AAmmoPickup* LastAmmo)
{
	if (OverlappingAmmo)
	{
		OverlappingAmmo->ShowPickupWidget(true);
	}
	if (LastAmmo)
	{
		LastAmmo->ShowPickupWidget(false);
	}
}

void ABlasterCharacter::ServerEquipButtonPressed_Implementation()
{
	if (Combat && OverlappingWeapon)
	{
		Combat->EquipWeapon(OverlappingWeapon);
	}
}

// 弹药拾取 Server RPC：由客户端按 E 键触发，服务器权威验证
// 流程：验证 OverlappingAmmo 有效 → PickupAmmo 检查类型匹配 →
//   - 匹配成功：Destroy 拾取物 + 清除追踪
//   - 匹配失败：ClientAmmoMismatchNotification RPC → 客户端绿色提示
void ABlasterCharacter::ServerPickupAmmo_Implementation()
{
	if (Combat == nullptr || OverlappingAmmo == nullptr) return;

	EWeaponType AmmoType = OverlappingAmmo->GetWeaponType();
	int32 AmmoAmount = OverlappingAmmo->GetAmmoAmount();

	// PickupAmmo 返回 true=类型匹配已添加备弹，false=无装备武器或类型不匹配
	if (Combat->PickupAmmo(AmmoType, AmmoAmount))
	{
		// 拾取成功：服务器销毁弹药 Actor（复制到客户端清理），清除重叠追踪
		OverlappingAmmo->Destroy();
		SetOverlappingAmmo(nullptr);
	}
	else
	{
		// 拾取失败：单向通知持有者客户端显示绿色提示，不销毁拾取物
		ClientAmmoMismatchNotification(TEXT("枪械与子弹不匹配！"));
	}
}

void ABlasterCharacter::TurnInPlace(float DeltaTime)
{
	if(AO_Yaw>90.f){
		TurningInPlace=ETurningInPlace::ETIP_Right;
	}
	else if(AO_Yaw<-90.f){
		TurningInPlace=ETurningInPlace::ETIP_Left;
	}
	else{
		TurningInPlace=ETurningInPlace::ETIP_NotTurning;
	}

	//转身的平滑收尾阶段。
	//不直接跳变，而是让 AO_Yaw 从当前值插值逼近 0（速度 4.0），等转到 15° 以内就判定转身完成，重置基准方向。
	if(TurningInPlace!=ETurningInPlace::ETIP_NotTurning){
		InterpAO_Yaw=FMath::FInterpTo(InterpAO_Yaw,0.f,DeltaTime,4.f);
		AO_Yaw=InterpAO_Yaw;
		if(FMath::Abs(AO_Yaw)<15.f){
			TurningInPlace=ETurningInPlace::ETIP_NotTurning;
			StartAimRotation=FRotator(0.f,GetBaseAimRotation().Yaw,0.f);
		}
	}

}


void ABlasterCharacter::HideCameraIfCharacterClose()
{
	// ————————————————————————————————————————————
	// 摄像机近身透视防护：当摄像机离角色太近时（如被墙壁挤压、滚轮拉近），
	// 隐藏角色身体和武器模型，防止摄像机穿透到模型内部看到"空心"或"眼部穿模"
	// 只在本地玩家执行——远程玩家看到的第三人称视角不需要此处理
	// ————————————————————————————————————————————
	if (!IsLocallyControlled()) return;                                                     // 只处理本地控制端，不需要画蛇添足管别人的画面

	// 计算摄像机到角色中心点之间的距离，如果小于阈值（200cm）则认为摄像机进入了角色体内
	// CameraThreshold = 200.f，约2米，足以覆盖角色胶囊体半径（约34cm）+ 弹簧臂最小长度
	float DistanceToCamera = (FollowCamera->GetComponentLocation() - GetActorLocation()).Size();
	if (DistanceToCamera < CameraThreshold)
	{
		// 摄像机太近 → 隐藏角色本体，否则画面会被角色的脑袋/肩膀挡住
		GetMesh()->SetVisibility(false);

		// 同时隐藏武器模型（对所有者不可见），否则枪口会"怼进"摄像机镜头里
		if (Combat && Combat->EquippedWeapon && Combat->EquippedWeapon->GetWeaponMesh())
		{
			Combat->EquippedWeapon->GetWeaponMesh()->bOwnerNoSee = true;
		}
	}
	else
	{
		// 摄像机已经退回到安全距离 → 恢复显示角色和武器
		GetMesh()->SetVisibility(true);

		if (Combat && Combat->EquippedWeapon && Combat->EquippedWeapon->GetWeaponMesh())
		{
			Combat->EquippedWeapon->GetWeaponMesh()->bOwnerNoSee = false;
		}
	}
}


void ABlasterCharacter::OnRep_Health()
{
	UpdateHUDHealth();
	PlayHitReactMontage();
}

void ABlasterCharacter::OnRep_Shield(float LastShield)
{
	UpdateHUDShield();
	// 护盾减少（受伤）时播放受击动画
	if (Shield < LastShield)
	{
		PlayHitReactMontage();
	}
}

void ABlasterCharacter::UpdateHUDShield()
{
	BlasterPlayerController = BlasterPlayerController == nullptr ? Cast<ABlasterPlayerController>(Controller) : BlasterPlayerController;
	if (BlasterPlayerController)
	{
		BlasterPlayerController->SetHUDShield(Shield, MaxShield);
	}
}

void ABlasterCharacter::Heal(float HealAmount)
{
	// 仅服务器修改复制属性 Health，客户端通过 OnRep_Health 更新 HUD
	if (!HasAuthority()) return;
	Health = FMath::Clamp(Health + HealAmount, 0.f, MaxHealth);
	UpdateHUDHealth();
}

void ABlasterCharacter::SetOverlappingWeapon(AWeapon *Weapon)
{
	if (OverlappingWeapon)
	{
		OverlappingWeapon->ShowPickupWidget(false);
	}

	OverlappingWeapon = Weapon;//此变量被标记为复制变量，ue会先调用GetLifetimeReplicatedProps(TArray<FLifetimeProperty> &OutLifetimeProps)函数进行复制
	//然后调用OnRep_OverlappingWeapon(AWeapon* LastWeapon)函数处理后续逻辑


	if(IsLocallyControlled() && OverlappingWeapon)//只有本地玩家才显示拾取提示UI
	{
		OverlappingWeapon->ShowPickupWidget(true);
	}
}

// 弹药不匹配通知 Client RPC：仅由服务器在拾取失败时调用
// 转发到 PlayerController → CharacterOverlay → 绿色文本显示 2 秒后自动隐藏
void ABlasterCharacter::ClientAmmoMismatchNotification_Implementation(const FString& Message)
{
	ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(Controller);
	if (PC)
	{
		PC->SetHUDMismatchNotification(Message);
	}
}

// 弹药拾取追踪 Setter：与 SetOverlappingWeapon 同款模式
// 服务器端设置 OverlappingAmmo（触发 CONDOwnerOnly 复制）→ 客户端 OnRep → ShowPickupWidget
// 本地控制角色直接显示 Widget（不等待复制，响应更快）
void ABlasterCharacter::SetOverlappingAmmo(AAmmoPickup* Ammo)
{
	if (OverlappingAmmo)
	{
		OverlappingAmmo->ShowPickupWidget(false);
	}

	OverlappingAmmo = Ammo; // 复制变量赋值，引擎自动触发 OnRep_OverlappingAmmo

	if (IsLocallyControlled() && OverlappingAmmo)
	{
		OverlappingAmmo->ShowPickupWidget(true);
	}
}

bool ABlasterCharacter::IsWeaponEquipped()
{
	return (Combat && Combat->EquippedWeapon);
}

AWeapon* ABlasterCharacter::GetEquippedWeapon() const
{
	if (Combat == nullptr) return nullptr;
	return Combat->EquippedWeapon;
}

ECombatState ABlasterCharacter::GetCombatState() const
{
	// 投掷忙碌时返回 Throwing，阻止武器系统开枪/换弹
	if (Throwable && !Throwable->IsIdle()) return ECombatState::ECS_Throwing;
	if (Combat == nullptr) return ECombatState::ECS_Unoccupied;
	return Combat->CombatState;
}

bool ABlasterCharacter::IsAiming()
{
    return (Combat && Combat->bAiming);
}

FVector ABlasterCharacter::GetHitTarget() const
{
	if(Combat==NULL)
    return FVector();
	return Combat->HitTarget;
}

void ABlasterCharacter::SpawDefaultWeapon()
{
	// 检查任意 GameMode（回合制 ABombDefusalGameMode 继承 AGameMode 而非 ABlasterGameMode）
	AGameMode* GameMode = GetWorld()->GetAuthGameMode<AGameMode>();
	UWorld* World = GetWorld();
	if (GameMode && World && !bElimmed && DefaultWeaponClass)
	{
		AWeapon* StartingWeapon = World->SpawnActor<AWeapon>(DefaultWeaponClass);
		StartingWeapon->bDestroyWeapon = true;
		if (Combat)
		{
			Combat->EquipWeapon(StartingWeapon);
		}
	}
}

// ===== 投掷物径向选择面板 =====

void ABlasterCharacter::ThrowableWheelToggle()
{
		if (bDisableGameplayInput) return;
	if (!Throwable || Throwable->IsCooking()) return;

	BlasterPlayerController = BlasterPlayerController == nullptr
		? Cast<ABlasterPlayerController>(Controller) : BlasterPlayerController;

	if (BlasterPlayerController)
	{
		BlasterPlayerController->ShowThrowablePanel();
	}
}

void ABlasterCharacter::SelectThrowableType(EThrowableType Type)
{
	// 由选择面板 Widget 点击 → PlayerController → 此处调用
	// 装备投掷物并播放拿在手里的动画
	if (Throwable && Type != EThrowableType::ETT_None && Type != EThrowableType::ETT_MAX)
	{
		Throwable->EquipThrowable(Type);
	}
}

void ABlasterCharacter::DropOrDestroyWeapon(AWeapon* Weapon)
{
	if (Weapon == nullptr) return;
	if (Weapon->bDestroyWeapon)
	{
		Weapon->Destroy();
	}
	else
	{
		Weapon->Dropped();
	}
}

void ABlasterCharacter::DropOrDestroyWeapons()
{
	if (Combat && Combat->EquippedWeapon)
	{
		DropOrDestroyWeapon(Combat->EquippedWeapon);
	}
}

// Q键安包/拆包：转发到 BombInteractionComponent（Hold-to-interact）
void ABlasterCharacter::BombInteractPressed()
{
	if (bDisableGameplayInput) return;
	if (BombInteraction)
	{
		BombInteraction->OnInteractKeyPressed();
	}
}

void ABlasterCharacter::BombInteractReleased()
{
	if (BombInteraction)
	{
		BombInteraction->OnInteractKeyReleased();
	}
}

// ════════════════════════════════════════════════════════════════
// SSR：捕获当前碰撞体快照（胶囊体 + 关键骨骼的世界空间 Transform）
// FrameHistory 每帧调用 → 写入环形缓冲区；RewindManager 备份时调用
// ════════════════════════════════════════════════════════════════
//[已废弃-死代码]：录制逻辑已由 FrameHistory::CapturePlayerEntry 内联实现，本函数无调用点
void ABlasterCharacter::CaptureHitboxState(FSSR_PlayerFrameEntry& OutEntry)
{
	OutEntry.Character = this;

	const UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (Capsule)//记录胶囊体的世界位置、旋转、半高和半径
	{
		OutEntry.CapsuleLocation   = Capsule->GetComponentLocation();
		OutEntry.CapsuleRotation   = Capsule->GetComponentQuat();
		OutEntry.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		OutEntry.CapsuleRadius     = Capsule->GetScaledCapsuleRadius();
	}


	//准备进行骨骼的录制
	OutEntry.BoneSnapshots.Reset();
	USkeletalMeshComponent* SkMesh = GetMesh();
	if (!SkMesh) return;

	const FTransform MeshWorldTM = SkMesh->GetComponentTransform();

	// [已废弃] 物理回退遗留：纯数学判定不消费这两字段（读取方 ApplyHitboxState 是死代码），
	// 保留写入仅作演进痕迹 / 将来如需物理恢复的备用。见 SSRTypes.h 同注释。
	OutEntry.MeshWorldLocation = MeshWorldTM.GetLocation();
	OutEntry.MeshWorldRotation = MeshWorldTM.GetRotation();

	// ── 骨骼录制：遍历关键骨骼名单，把每根骨骼的"名字 + 世界坐标"录进快照 ──
	// RelevantBoneNames = BuildRelevantBoneList 建的 14 根名单（head/spine/四肢…）
	for (const FName& BoneName : RelevantBoneNames)
	{
		// 骨骼名 → 骨骼索引：GetBoneIndex 用名字查角色骨架里的实际索引（名字不能直接当索引用）
		const int32 BoneIndex = SkMesh->GetBoneIndex(BoneName);
		// 防御：角色骨架可能没有这根骨骼（不同角色模型骨骼命名不同），找不到就跳过不录
		if (BoneIndex == INDEX_NONE) continue;

		// WHY：GetBoneTransform(Index) 已返回世界空间 Transform；不能再次乘
		// MeshWorldTM，否则会重复应用角色的世界平移/旋转并写入错误的骨骼球心。
		// HOW：本函数虽为遗留无调用路径，但与 FrameHistory 的正式录制语义保持一致，
		// 让未来若重新启用时仍能直接向世界空间 SSR 射线提供正确坐标。
		const FTransform BoneWS = SkMesh->GetBoneTransform(BoneIndex);

		// 填一条骨骼快照：
		FSSR_BoneSnapshot BoneSnap;
		BoneSnap.BoneName = BoneName;                 // 名字 → 判定时判爆头（BoneName=="head"）
		BoneSnap.Location = BoneWS.GetLocation();     // 世界坐标 → 判定时当球心（RaySphereIntersect）
		BoneSnap.Rotation = BoneWS.GetRotation();     // 朝向（备用，球体判定旋转不变，不读）

		OutEntry.BoneSnapshots.Add(BoneSnap);         // 塞进数组 → 这条就是 FSSR_PlayerFrameEntry 里 14 根之一
	}
}

// ════════════════════════════════════════════════════════════════
// ⚠ [已废弃-死代码] SSR 物理回退方案：将碰撞体恢复/回退到指定状态
// 全项目无调用点（RewindManager 实际走纯数学判定，不挪动物理体）。
// 早期设计：回退时把 Mesh/胶囊体挪回历史姿势、带动骨骼物理体；
// 后改为纯数学（MathTraceSingleRay 对快照做解析相交）→ 零物理回滚、确定性。
// 保留仅作演进痕迹 / 回退备用方案，勿调用；删除时需同步清理
// MeshWorldLocation/Rotation 字段与其写入点。
// （原注释：用 TeleportPhysics 避免触发网络复制和物理模拟）
// ════════════════════════════════════════════════════════════════

void ABlasterCharacter::ApplyHitboxState(const FSSR_PlayerFrameEntry& Entry)
{
	// 1. 胶囊体回退：直接操作 Collision Component，不走 SetActorTransform
	UCapsuleComponent* Capsule = GetCapsuleComponent();
	if (Capsule)
	{
		Capsule->SetWorldLocation(Entry.CapsuleLocation, false, nullptr, ETeleportType::TeleportPhysics);
		Capsule->SetWorldRotation(Entry.CapsuleRotation, false, nullptr, ETeleportType::TeleportPhysics);
	}

	// 2. SkeletalMeshComponent 世界位置回退
	//    直接移动 Mesh Component 到历史世界位置，带动所有骨骼物理体一起移动
	//    SetBodyTransform 对 PhysX kinematic articulation link 不生效，必须通过 Component 级别移动
	USkeletalMeshComponent* SkMesh = GetMesh();
	if (SkMesh)
	{
		SkMesh->SetWorldLocation(Entry.MeshWorldLocation, false, nullptr, ETeleportType::TeleportPhysics);
		SkMesh->SetWorldRotation(Entry.MeshWorldRotation, false, nullptr, ETeleportType::TeleportPhysics);
	}

	// 3. 骨骼物理体微调：逐个设置 BodyInstance 到历史位置（best-effort 精度修正）
	if (!SkMesh || Entry.BoneSnapshots.Num() == 0) return;

	for (const FSSR_BoneSnapshot& BoneSnap : Entry.BoneSnapshots)
	{
		FBodyInstance* BodyInst = SkMesh->GetBodyInstance(BoneSnap.BoneName);
		if (BodyInst)
		{
			const FTransform WorldTM(BoneSnap.Rotation, BoneSnap.Location);
			BodyInst->SetBodyTransform(WorldTM, ETeleportType::TeleportPhysics);
		}
	}
}
