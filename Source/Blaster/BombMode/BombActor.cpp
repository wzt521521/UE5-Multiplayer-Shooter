#include "Blaster/BombMode/BombActor.h"
#include "Blaster/BombMode/BombSite.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/WidgetComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/Engine.h"

ABombActor::ABombActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	// 初始在网络中存在但不做任何事，等 GameMode 分配后激活
	SetReplicatingMovement(false);

	// 根组件：炸弹模型（引擎内置 Cube 占位，蓝图可替换）
	BombMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BombMesh"));
	BombMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BombMesh->SetIsReplicated(true);
	SetRootComponent(BombMesh);

	// 碰撞体用于检测守方是否靠近已安放的炸弹
	InteractSphere = CreateDefaultSubobject<USphereComponent>(TEXT("InteractSphere"));
	InteractSphere->SetupAttachment(BombMesh);
	InteractSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	InteractSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	InteractSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	InteractSphere->SetSphereRadius(MaxInteractDistance);

	// 拾取提示 Widget：与 AWeapon 同款机制，默认隐藏，
	// 掉落可拾取时由 BombInteractionComponent 驱动显示（WidgetClass 在蓝图里指向 WBP_PickupWidget）
	PickupWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("PickupWidget"));
	PickupWidget->SetupAttachment(BombMesh);

	// Tick 始终开启，在 Tick 内部用 HasAuthority() 过滤（UE 5.0 兼容）
}

void ABombActor::BeginPlay()
{
	Super::BeginPlay();

	if (PickupWidget)
	{
		PickupWidget->SetVisibility(false); // 默认隐藏，仅掉落可拾取时显示
	}
}

void ABombActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABombActor, BombState);
	DOREPLIFETIME(ABombActor, RemainingTime);
	DOREPLIFETIME(ABombActor, PlantedSite);
}

// ========================================================================
// Tick：仅 Planted 状态下递减倒计时
// ========================================================================
void ABombActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (HasAuthority() && BombState == EBombState::EBS_Planted)
	{
		TickBombCountdown(DeltaTime);
	}
}

void ABombActor::TickBombCountdown(float DeltaTime)
{
	RemainingTime -= DeltaTime;
	if (RemainingTime <= 0.f)
	{
		RemainingTime = 0.f;
		Explode();
	}
}

// ========================================================================
// 服务器端交互入口 — 安包
// ========================================================================
void ABombActor::Server_StartPlant(ABombSite* Site)
{
	if (!HasAuthority()) return;
	if (BombState != EBombState::EBS_Carried) return; // 只能从携带状态安包
	if (!Site) return;

	PlantedSite = Site;
	bIsInteracting = true;
	CurrentInteractionType = EBombInteractionType::EBIT_Planting;

	// 启动 5 秒定时器，到期调用 CompletePlant
	GetWorldTimerManager().SetTimer(InteractionTimer,
		this, &ABombActor::Server_CompletePlant,
		PlantDuration, false);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Plant started at site %s (%.1fs)"),
		*Site->SiteName, PlantDuration);
}

// 安包完成：进入 Planted 状态，启动炸弹倒计时
void ABombActor::Server_CompletePlant()
{
	if (!HasAuthority()) return;
	if (BombState != EBombState::EBS_Carried) return;

	bIsInteracting = false;
	CurrentInteractionType = EBombInteractionType::EBIT_None;
	GetWorldTimerManager().ClearTimer(InteractionTimer);

	// 标记点位被占用
	if (PlantedSite)
	{
		PlantedSite->bIsBombPlantedHere = true;
	}

	RemainingTime = BombCountdown;
	SetBombState(EBombState::EBS_Planted);

	// Detach 从携带者身上脱离，置于安放点位的地面上
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Planted! Countdown %.0fs at site %s"),
		RemainingTime, PlantedSite ? *PlantedSite->SiteName : TEXT("Unknown"));

	// 广播：GameMode 订阅 → 全服文字公告
	OnBombPlanted.Broadcast(PlantedSite);
}

// ========================================================================
// 服务器端交互入口 — 拆包
// ========================================================================
void ABombActor::Server_StartDefuse()
{
	if (!HasAuthority()) return;
	if (BombState != EBombState::EBS_Planted) return; // 只能从已安放状态拆包

	bIsInteracting = true;
	CurrentInteractionType = EBombInteractionType::EBIT_Defusing;

	GetWorldTimerManager().SetTimer(InteractionTimer,
		this, &ABombActor::Server_CompleteDefuse,
		DefuseDuration, false);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Defuse started (%.1fs)"), DefuseDuration);
}

// 拆包完成：进入 Defused 状态
void ABombActor::Server_CompleteDefuse()
{
	if (!HasAuthority()) return;
	if (BombState != EBombState::EBS_Planted) return;

	bIsInteracting = false;
	CurrentInteractionType = EBombInteractionType::EBIT_None;
	GetWorldTimerManager().ClearTimer(InteractionTimer);

	// 释放点位占用：与 Explode() 对称，确保下回合该点位可重新安包
	if (PlantedSite)
	{
		PlantedSite->bIsBombPlantedHere = false;
	}

	SetBombState(EBombState::EBS_Defused);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Defused!"));

	// 广播：GameMode 订阅 → 守方胜利
	OnBombDefused.Broadcast();
}

// ========================================================================
// 取消交互（被打断）
// ========================================================================
void ABombActor::Server_CancelInteraction()
{
	if (!HasAuthority()) return;
	if (!bIsInteracting) return;

	bIsInteracting = false;
	EBombInteractionType CancelledType = CurrentInteractionType;
	CurrentInteractionType = EBombInteractionType::EBIT_None;
	GetWorldTimerManager().ClearTimer(InteractionTimer);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] %s cancelled"),
		CancelledType == EBombInteractionType::EBIT_Planting ? TEXT("Plant") : TEXT("Defuse"));
}

// ========================================================================
// 携带逻辑
// ========================================================================
void ABombActor::AssignToCarrier(ABlasterCharacter* Carrier)
{
	if (!HasAuthority() || !Carrier) return;

	// Attach 到角色上（挂载到右手位置或背部）
	AttachToComponent(Carrier->GetMesh(),
		FAttachmentTransformRules::SnapToTargetNotIncludingScale,
		FName("hand_rSocket")); // 尝试挂手部骨骼，无此Socket则挂Root

	SetOwner(Carrier);
	SetBombState(EBombState::EBS_Carried);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Assigned to %s"), *Carrier->GetName());
}

// 携带者死亡：掉落到地面
void ABombActor::DropAtLocation(const FVector& Location)
{
	if (!HasAuthority()) return;

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	SetActorLocation(Location);
	SetOwner(nullptr);
	// 保持 Carried 状态 + 置空 Owner：掉落后仍可被其他攻方靠近，按 E 键通过
	// Server_PickupBomb 重新拾取（拾取提示由世界空间 WBP_PickupWidget 呈现）

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Dropped at %s"), *Location.ToString());
}

// 显示/隐藏掉落拾取提示：与 AWeapon::ShowPickupWidget 完全一致的接口，
// 只控制本地客户端对 World Widget 的可见性，不涉及任何复制状态
void ABombActor::ShowPickupWidget(bool bShowWidget)
{
	if (PickupWidget)
	{
		PickupWidget->SetVisibility(bShowWidget);
	}
}

// ========================================================================
// 爆炸
// ========================================================================
void ABombActor::Explode()
{
	SetBombState(EBombState::EBS_Exploded);
	bIsInteracting = false;
	GetWorldTimerManager().ClearTimer(InteractionTimer);

	if (PlantedSite)
	{
		PlantedSite->bIsBombPlantedHere = false;
	}

	UE_LOG(LogTemp, Log, TEXT("[Bomb] EXPLODED!"));

	// 广播：GameMode 订阅 → 攻方胜利
	OnBombExploded.Broadcast();
}

// ========================================================================
// 统一状态变更入口：只在服务器端调用，触发复制
// ========================================================================
void ABombActor::SetBombState(EBombState NewState)
{
	if (!HasAuthority()) return;
	if (BombState == NewState) return;

	EBombState OldState = BombState;
	BombState = NewState;

	UE_LOG(LogTemp, Log, TEXT("[Bomb] State: %d -> %d"), (int32)OldState, (int32)NewState);

	// 终态时不需要 Tick，但 HasAuthority() 检查已足够（UE 5.0 兼容）
}

// ========================================================================
// OnRep 回调：客户端收到复制后更新 UI
// ========================================================================
void ABombActor::OnRep_BombState()
{
	// 客户端收到状态变更 → UI Widget 可通过绑定此 Actor 的事件来刷新
	// Widget 在 Tick 或绑定中读取 BombState + RemainingTime 即可
}

void ABombActor::OnRep_RemainingTime()
{
	// 客户端收到服务器同步的倒计时值 → UI 刷新
}

// ========================================================================
// 编辑器辅助
// ========================================================================
#if WITH_EDITOR
void ABombActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 修改交互距离后同步更新碰撞体大小
	const FName PropName = PropertyChangedEvent.GetPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(ABombActor, MaxInteractDistance))
	{
		if (InteractSphere)
		{
			InteractSphere->SetSphereRadius(MaxInteractDistance);
		}
	}
}
#endif
