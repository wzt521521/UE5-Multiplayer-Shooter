// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blaster/BlasterTypes/TurningInPlace.h"
#include "Blaster/Interafaces/InteractWithCrosshairsInterface.h"
#include "GameFramework/Character.h"
#include "Blaster/BlasterTypes/CombatState.h"
#include "Blaster/BlasterTypes/ThrowableTypes.h"
#include "Blaster/BombMode/BombTypes.h"
#include "Blaster/SSR/SSRTypes.h"     // FSSR_PlayerFrameEntry
#include "Blaster/Character/BlasterCharacterMovementComponent.h" // P3 服务器端移动校验：替换默认移动组件
#include "BlasterCharacter.generated.h"
class ABlasterPlayerController;
class ABlasterPlayerState;
class AAmmoPickup;
UCLASS()
class BLASTER_API ABlasterCharacter : public ACharacter, public IInteractWithCrosshairsInterface
{
	GENERATED_BODY()

public:

	// 带 FObjectInitializer 的构造：在初始化列表用 SetDefaultSubobjectClass 把默认移动组件
	// 替换为 UBlasterCharacterMovementComponent（引擎官方换组件类方式，见 Character.h 注释）
	ABlasterCharacter(const FObjectInitializer& ObjectInitializer);
	virtual void Tick(float DeltaTime) override;

	// ── 客户端插值平滑采样（Phase 2 补下行）──
	// static 累积缓冲区（连接级共享）：PC 每 1s 读取后清空。
	// 由 OnRep_ReplicatedMovement 喂入"各角色自己的快照到达间隔"（秒）。
	static TArray<float> PendingIntervals;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	//函数内部注册要复制的变量
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;//在这个函数中访问combat组件
	void PlayFireMontage(bool bAiming);
	void PlayReloadMontage();
	void PlayHitReactMontage();
	void PlayElimMontage();


	void Elim();

	void DropOrDestroyWeapon(AWeapon* Weapon);
	void DropOrDestroyWeapons();

	void SpawDefaultWeapon();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastElim();

	UFUNCTION(BlueprintImplementableEvent)
	void ShowSniperScopeWidget(bool bShowScope);

	ABlasterPlayerState* BlasterPlayerState;

protected:

	virtual void BeginPlay() override;

	// 客户端收到移动复制（ReplicatedMovement）时由属性复制系统调用（AActor 虚函数）。
	// 记录到达墙钟时间、算本角色自己的到达间隔，喂入 static PendingIntervals（Phase 2 采样源）。
	virtual void OnRep_ReplicatedMovement() override;

	void MoveForward(float Value);
	void MoveRight(float Value);
	void Turn(float Value);
	void LookUp(float Value);
	void EquipButtonPressed();
	void CrouchButtonPressed();
	void ReloadButtonPressed();
	void AimButtonPressed();
	void AimButtonReleased();
	void AimOffset(float DeltaTime);
	virtual void Jump() override;
	void FireButtonPressed();
	void FireButtonReleased();
	void ThrowableWheelToggle();    // G 键按下 → 切换投掷物选择面板开/关
	void BombInteractPressed();     // Q 键按下 → 安包/拆包（BombMode Phase 2）
	void BombInteractReleased();    // Q 键松开 → 取消安包/拆包（Hold-to-interact）
public:
	// 由径向面板 Widget 调用（通过 PlayerController → Pawn）
	void SelectThrowableType(EThrowableType Type);
protected:
	UFUNCTION()
	void ReceiveDamage(AActor* DamagedActor, float Damage, const UDamageType* DamageType, 
		AController* InstigatorController, AActor* DamageCauser);
	void UpdateHUDHealth();
	void PollInit();
private:
	// ── 客户端插值平滑采样状态（仅客户端 simulated proxy 有意义）──
	// ★ 实例成员（每角色独立），不能用 static：同批 UDP 包里多个角色的到达只差 ~0.3ms（处理时间），
	//   与跨 tick 的 ~16ms 真实间隔交替，会产生确定性伪抖动淹没真实网络抖动。
	double LastSnapshotArrivalTime = -1.0; // 本角色上次快照到达时间（客户端墙钟，秒）

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	class USpringArmComponent* CameraBoom;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	class UCameraComponent* FollowCamera;


	UPROPERTY(EditAnywhere,BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UWidgetComponent* OverheadWidget;

	UPROPERTY(ReplicatedUsing=OnRep_OverlappingWeapon)
	class AWeapon* OverlappingWeapon;//可以复制


	//LastWeapon 是 Unreal Engine 复制系统自动管理的，不需要你自己更新。

	// 工作原理：当 ReplicatedUsing 标记的属性通过网络复制到客户端时，引擎会：

	// 先保存该属性复制前的旧值
	// 用新值覆盖属性
	// 调用 OnRep 回调，把旧值作为参数传入（即 LastWeapon）
	// 所以你只需要在 GetLifetimeReplicatedProps 中注册好：


	// DOREPLIFETIME_CONDITION(ABlasterCharacter, OverlappingWeapon, COND_OwnerOnly);
	// 然后引擎会自动追踪旧值并在复制发生时传给你的 OnRep_OverlappingWeapon。
	UFUNCTION()
	void OnRep_OverlappingWeapon(AWeapon* LastWeapon);//被复制时才会调用的函数，服务器永远不会被复制，所以服务器永远不会调用

	// 弹药拾取追踪：与 OverlappingWeapon 同款复制模式（COND_OwnerOnly）
	// AAmmoPickup::OnSphereOverlap → SetOverlappingAmmo → 复制到客户端 → OnRep → 显示 PickupWidget
	UPROPERTY(ReplicatedUsing = OnRep_OverlappingAmmo)
	class AAmmoPickup* OverlappingAmmo;

	UFUNCTION()
	void OnRep_OverlappingAmmo(AAmmoPickup* LastAmmo);

	UPROPERTY(VisibleAnywhere, Category = "Combat",BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UCombatComponent* Combat;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UBuffComponent* Buff;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UThrowableComponent* Throwable;

	// 炸弹交互：E 键拾取掉落炸弹 + Q 键安包/拆包，在构造函数中 CreateDefaultSubobject
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	class UBombInteractionComponent* BombInteraction;

	UFUNCTION(Server, Reliable)
	void ServerEquipButtonPressed();

	// 弹药拾取 Server RPC：由 EquipButtonPressed 在无武器重叠时调用
	// 服务器验证 OverlappingAmmo 是否有效 + Combat->PickupAmmo 类型匹配检查
	UFUNCTION(Server, Reliable)
	void ServerPickupAmmo();

	// 弹药不匹配通知 Client RPC：仅在类型不匹配时由服务器单向通知持有者客户端
	// 调用链：ServerPickupAmmo → PickupAmmo(false) → 本 RPC → PlayerController→HUD绿色提示
	UFUNCTION(Client, Reliable)
	void ClientAmmoMismatchNotification(const FString& Message);
	float AO_Pitch;
	float AO_Yaw;
	float InterpAO_Yaw;
	FRotator StartAimRotation;//开始瞄准时的旋转值

	ETurningInPlace TurningInPlace;//翻转状态
	void TurnInPlace(float DeltaTime);

	UPROPERTY(EditAnywhere, Category = "Combat")
	class UAnimMontage* FireWeaponMontage;

	UPROPERTY(EditAnywhere, Category = "Combat")
	UAnimMontage* HitReactMontage;

	UPROPERTY(EditAnywhere, Category = "Combat")
	UAnimMontage* ElimMontage;

	UPROPERTY(EditAnywhere, Category = "Combat")
	UAnimMontage* ReloadMontage;

	void HideCameraIfCharacterClose();

	UPROPERTY(EditAnywhere, Category = "Camera")
	float CameraThreshold = 200.f;//摄像机与角色的距离阈值，单位为厘米

	UPROPERTY(EditAnywhere, Category = "Player Stats")
	float MaxHealth = 100.f;
	UPROPERTY(ReplicatedUsing=OnRep_Health, EditAnywhere, Category = "Player Stats")
	float Health = 100.f;

	UFUNCTION()
	void OnRep_Health();

	// 护盾：默认 0，受伤时先吸收伤害，通过 ShieldPickup 获取
	UPROPERTY(EditAnywhere, Category = "Player Stats")
	float MaxShield = 100.f;
	UPROPERTY(ReplicatedUsing=OnRep_Shield, EditAnywhere, Category = "Player Stats")
	float Shield = 0.f;

	UFUNCTION()
	void OnRep_Shield(float LastShield);

	ABlasterPlayerController* BlasterPlayerController;

	bool bElimmed = false;//是否死亡

	// SSR 骨骼追踪列表：BeginPlay 时从 USSR_FrameHistory 拷贝
	TArray<FName> RelevantBoneNames;

	FTimerHandle ElimTimer;
	UPROPERTY(EditDefaultsOnly, Category = "Player Stats")
	float ElimDelay = 3.f;

	UPROPERTY(EditAnywhere)
	TSubclassOf<AWeapon> DefaultWeaponClass;

	void ElimTimerFinsished();

	// Bomb 模式：死亡 ElimDelay（3s）后销毁尸体（死亡镜头结束，尸体不残留到下一回合）。
	// 服务器权威执行（Elim 仅服务器）；先 UnPossess（尸体仍被服务器 PC Possess，见 P1 观战设计）再 Destroy。
	void DestroyCorpse();

	// 中途加入等待标记：回合进行中不生成 Pawn，下回合恢复
	UPROPERTY(Replicated)
	bool bWaitingForNextRound = false;

public:
	void SetOverlappingWeapon(AWeapon* Weapon) ;

	// 弹药拾取追踪 Setter：由 AAmmoPickup::OnSphereOverlap/EndOverlap 调用
	// 设置 OverlappingAmmo（触发复制）→ 客户端 OnRep → ShowPickupWidget
	void SetOverlappingAmmo(AAmmoPickup* Ammo);
	void Heal(float HealAmount);
	void UpdateHUDShield();

	bool IsWeaponEquipped();
	bool IsAiming();
	FORCEINLINE float GetAO_Pitch() const { return AO_Pitch; }
	FORCEINLINE float GetAO_Yaw() const { return AO_Yaw; }
	FORCEINLINE ETurningInPlace GetTurningInPlace() const { return TurningInPlace; }
	FVector GetHitTarget() const;
	class AWeapon* GetEquippedWeapon() const;
	FORCEINLINE UCameraComponent* GetFollowCamera() const { return FollowCamera; }
	FORCEINLINE bool IsElimmed() const { return bElimmed; }

	// ── SSR 延迟补偿：碰撞体捕获/恢复 ──
	// FrameHistory 每帧调用 CaptureHitboxState 录制快照
	// RewindManager 在回退/恢复时调用 ApplyHitboxState 临时修改碰撞体
	void CaptureHitboxState(FSSR_PlayerFrameEntry& OutEntry);
	void ApplyHitboxState(const FSSR_PlayerFrameEntry& Entry);
	FORCEINLINE float GetHealth() const { return Health; }
	FORCEINLINE float GetMaxHealth() const { return MaxHealth; }
	// ↑ 供 BlasterPlayerController::OnPossess 拉取血量，复活时重置 HUD 血条用
	FORCEINLINE float GetShield() const { return Shield; }
	FORCEINLINE void SetShield(float Amount) { Shield = Amount; }
	FORCEINLINE float GetMaxShield() const { return MaxShield; }
	ECombatState GetCombatState() const;
	FORCEINLINE class UCombatComponent* GetCombat() const { return Combat; }
	FORCEINLINE class UBuffComponent* GetBuff() const { return Buff; }
	FORCEINLINE class UThrowableComponent* GetThrowable() const { return Throwable; }
	// 炸弹交互组件 getter（BombMode：Q键安包/拆包）
	FORCEINLINE class UBombInteractionComponent* GetBombInteraction() const { return BombInteraction; }
	FORCEINLINE UAnimMontage* GetReloadMontage() const { return ReloadMontage; }

	// 准备阶段禁止移动/战斗输入，只允许转视角和购买（RoundPrepare 期间）
	UPROPERTY(Replicated)
	bool bDisableGameplayInput = false;

};
