#include "Blaster/BombMode/BombInteractionComponent.h"
#include "Blaster/BombMode/BombActor.h"
#include "Blaster/BombMode/BombSite.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/BlasterTypes/TeamTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"

UBombInteractionComponent::UBombInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UBombInteractionComponent::BeginPlay()
{
	Super::BeginPlay();
	OwnerCharacter = Cast<ABlasterCharacter>(GetOwner());
}

// ========================================================================
// Tick
// ========================================================================
void UBombInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bIsInteracting)
	{
		TickInteractionProgress(DeltaTime);

		// 服务器权威校验：交互被打断（死亡/距离过远/炸弹被别人交互）
		if (GetOwnerRole() == ROLE_Authority)
		{
			if (!OwnerCharacter || OwnerCharacter->IsElimmed()) { ForceCancelInteraction(); return; }
			if (InteractionTarget)
			{
				float Dist = FVector::Dist(GetOwner()->GetActorLocation(),
					InteractionTarget->GetActorLocation());
				if (Dist > MaxInteractDistance + 50.f) { ForceCancelInteraction(); return; }
			}
			ABombActor* TargetBomb = Cast<ABombActor>(InteractionTarget);
			if (!TargetBomb)
			{
				TArray<AActor*> FoundBombs;
				UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombActor::StaticClass(), FoundBombs);
				for (AActor* Actor : FoundBombs)
				{
					ABombActor* Bomb = Cast<ABombActor>(Actor);
					if (Bomb && Bomb->GetOwner() == OwnerCharacter) { TargetBomb = Bomb; break; }
				}
			}
			if (TargetBomb && !TargetBomb->IsInteracting())
			{
				bIsInteracting = false;
				CurrentInteraction = EBombInteractionType::EBIT_None;
				InteractionElapsed = 0.f;
				InteractionTarget = nullptr;
				if (OwnerCharacter) OwnerCharacter->bDisableGameplayInput = false;
			}
		}
		// 本地玩家：交互中持续推送进度到 HUD（Listen Server 宿主 + 远程客户端）
		if (OwnerCharacter && OwnerCharacter->IsLocallyControlled())
		{
			PushInteractUI();
		}
	}
	// 本地玩家：非交互时检测附近目标 → 驱动世界空间拾取提示 → 推送安包/拆包 HUD 文字
	else if (OwnerCharacter && OwnerCharacter->IsLocallyControlled())
	{
		DetectNearbyTarget();
		UpdatePickupWidget();
		PushInteractUI();
	}
}

// ========================================================================
// 驱动掉落炸弹的世界空间 PickupWidget 显隐（武器同款拾取提示）
// ========================================================================
// 只在本机 Tick 中调用：命中可拾取的掉落炸弹 → 显示；目标变化/不再可拾取 → 隐藏。
// Widget 可见性是本机状态，不复制；服务器校验仍由 Server_PickupBomb 保证权威。
void UBombInteractionComponent::UpdatePickupWidget()
{
	ABombActor* Bomb = Cast<ABombActor>(InteractionTarget);
	const bool bShow = Bomb && Bomb->GetBombState() == EBombState::EBS_Carried && Bomb->GetOwner() == nullptr;

	// 隐藏上一帧的目标：目标换了，或同一目标不再满足可拾取条件（已被拾取/被安放）
	if (LastPickupTarget && (LastPickupTarget != Bomb || !bShow))
	{
		LastPickupTarget->ShowPickupWidget(false);
	}

	// 命中可拾取的掉落炸弹 → 显示世界空间拾取提示
	if (bShow && Bomb)
	{
		Bomb->ShowPickupWidget(true);
	}

	LastPickupTarget = bShow ? Bomb : nullptr;
}

// 返回当前可拾取的掉落炸弹；无命中返回 nullptr（E 键拾取分支使用）
ABombActor* UBombInteractionComponent::GetPickupBomb() const
{
	ABombActor* Bomb = Cast<ABombActor>(InteractionTarget);
	return (Bomb && Bomb->GetBombState() == EBombState::EBS_Carried && Bomb->GetOwner() == nullptr) ? Bomb : nullptr;
}

// E 键拾取入口：由 BlasterCharacter::EquipButtonPressed 调用。
// Server RPC 在权威端直接执行，远程端转发服务器；服务器校验见 Server_PickupBomb_Implementation
void UBombInteractionComponent::PickupDroppedBomb()
{
	ABombActor* Bomb = GetPickupBomb();
	if (!Bomb) return;
	Server_PickupBomb(Bomb);
}

// ========================================================================
// 推送交互 UI 到 HUD InteractWidget
// ========================================================================
void UBombInteractionComponent::PushInteractUI()
{
	ABlasterPlayerController* PC = OwnerCharacter
		? Cast<ABlasterPlayerController>(OwnerCharacter->GetController()) : nullptr;
	if (!PC) return;

	bool bShow = false;
	FString Prompt;
	float Progress = 0.f;

	// 从 InteractionTarget 解析点位名，用于拼接到提示文字中
	auto GetSiteName = [](AActor* Target) -> FString
	{
		if (!Target) return TEXT("");
		// InteractionTarget 可能是 BombSite（安包）或 BombActor（拆包目标=已安放炸弹 / 拾取目标=掉落炸弹）
		if (ABombSite* Site = Cast<ABombSite>(Target))
			return Site->SiteName;
		if (ABombActor* Bomb = Cast<ABombActor>(Target))
		{
			ABombSite* PlantedSite = Bomb->GetPlantedSite();
			if (PlantedSite) return PlantedSite->SiteName;
		}
		return TEXT("");
	};

	const FString SiteName = GetSiteName(InteractionTarget);
	const FString SiteSuffix = SiteName.IsEmpty() ? TEXT("") : FString::Printf(TEXT(" - %s点"), *SiteName);

	if (bIsInteracting)
	{
		bShow = true;
		Progress = GetInteractionProgress();
		Prompt = (CurrentInteraction == EBombInteractionType::EBIT_Defusing)
			? FString::Printf(TEXT("[Q] 拆除炸弹%s"), *SiteSuffix)
			: FString::Printf(TEXT("[Q] 安放炸弹%s"), *SiteSuffix);
	}
	else if (InteractionTarget)
	{
		// 掉落炸弹拾取提示已移交给世界空间 PickupWidget（UpdatePickupWidget），
		// HUD 文字只保留安包/拆包，避免双提示
		if (CurrentInteraction == EBombInteractionType::EBIT_Planting)
		{
			bShow = true;
			Prompt = FString::Printf(TEXT("[Q] 安放炸弹%s"), *SiteSuffix);
		}
		else if (CurrentInteraction == EBombInteractionType::EBIT_Defusing)
		{
			bShow = true;
			Prompt = FString::Printf(TEXT("[Q] 拆除炸弹%s"), *SiteSuffix);
		}
	}

	// bIsInteracting 控制进度条显隐：靠近时隐藏，按住 Q 时显示
	PC->UpdateBombInteractUI(Progress, Prompt, bShow, bIsInteracting);
}

// ========================================================================
// 客户端扫描附近目标
// ========================================================================
void UBombInteractionComponent::DetectNearbyTarget()
{
	InteractionTarget = nullptr;
	CurrentInteraction = EBombInteractionType::EBIT_None;
	if (!OwnerCharacter) return;
	ABlasterPlayerState* PS = OwnerCharacter->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	if (PS->TeamID == ETeamID::ETI_Attacker)
	{
		// 未携带炸弹：只关心可拾取的掉落炸弹（拾取走 E 键 + 世界空间 PickupWidget）。
		// 不在该分支检测安包点位——Q 键对无包者无效，避免显示"[Q] 安放"空提示。
		if (!IsCarryingBomb())
		{
			ABombActor* DroppedBomb = FindNearestDroppedBomb();
			if (DroppedBomb)
			{
				InteractionTarget = DroppedBomb;
				CurrentInteraction = EBombInteractionType::EBIT_None;
				return;
			}
			InteractionTarget = nullptr;
			CurrentInteraction = EBombInteractionType::EBIT_None;
			return;
		}

		// 已携带炸弹：只关心安包点位；没找到就清空目标，避免上一帧的旧目标残留提示
		ABombSite* Site = FindNearestBombSite();
		if (Site && !Site->bIsBombPlantedHere)
		{
			InteractionTarget = Site;
			CurrentInteraction = EBombInteractionType::EBIT_Planting;
		}
		else
		{
			InteractionTarget = nullptr;
			CurrentInteraction = EBombInteractionType::EBIT_None;
		}
	}
	else if (PS->TeamID == ETeamID::ETI_Defender)
	{
		ABombActor* Bomb = FindNearestPlantedBomb();
		if (Bomb) { InteractionTarget = Bomb; CurrentInteraction = EBombInteractionType::EBIT_Defusing; }
	}
}

// ========================================================================
// 辅助
// ========================================================================
bool UBombInteractionComponent::IsCarryingBomb() const
{
	TArray<AActor*> FoundBombs;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombActor::StaticClass(), FoundBombs);
	for (AActor* Actor : FoundBombs)
	{
		ABombActor* Bomb = Cast<ABombActor>(Actor);
		if (Bomb && Bomb->GetBombState() == EBombState::EBS_Carried && Bomb->GetOwner() == OwnerCharacter)
			return true;
	}
	return false;
}

ABombSite* UBombInteractionComponent::FindNearestBombSite() const
{
	if (!GetOwner()) return nullptr;
	TArray<AActor*> FoundSites;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombSite::StaticClass(), FoundSites);
	ABombSite* Nearest = nullptr;
	float NearestDist = MaxInteractDistance;
	FVector MyLoc = GetOwner()->GetActorLocation();
	for (AActor* Actor : FoundSites)
	{
		ABombSite* Site = Cast<ABombSite>(Actor);
		if (!Site || Site->bIsBombPlantedHere) continue;
		float Dist = FVector::Dist(MyLoc, Site->GetActorLocation());
		if (Dist < NearestDist) { NearestDist = Dist; Nearest = Site; }
	}
	return Nearest;
}

ABombActor* UBombInteractionComponent::FindNearestPlantedBomb() const
{
	if (!GetOwner()) return nullptr;
	TArray<AActor*> FoundBombs;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombActor::StaticClass(), FoundBombs);
	ABombActor* Nearest = nullptr;
	float NearestDist = MaxInteractDistance;
	FVector MyLoc = GetOwner()->GetActorLocation();
	for (AActor* Actor : FoundBombs)
	{
		ABombActor* Bomb = Cast<ABombActor>(Actor);
		if (!Bomb || Bomb->GetBombState() != EBombState::EBS_Planted) continue;
		float Dist = FVector::Dist(MyLoc, Bomb->GetActorLocation());
		if (Dist < NearestDist) { NearestDist = Dist; Nearest = Bomb; }
	}
	return Nearest;
}

ABombActor* UBombInteractionComponent::FindNearestDroppedBomb() const
{
	if (!GetOwner()) return nullptr;
	TArray<AActor*> FoundBombs;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombActor::StaticClass(), FoundBombs);
	ABombActor* Nearest = nullptr;
	float NearestDist = MaxInteractDistance;
	FVector MyLoc = GetOwner()->GetActorLocation();
	for (AActor* Actor : FoundBombs)
	{
		ABombActor* Bomb = Cast<ABombActor>(Actor);
		if (!Bomb || Bomb->GetBombState() != EBombState::EBS_Carried) continue;
		if (Bomb->GetOwner() != nullptr) continue;
		float Dist = FVector::Dist(MyLoc, Bomb->GetActorLocation());
		if (Dist < NearestDist) { NearestDist = Dist; Nearest = Bomb; }
	}
	return Nearest;
}

// ========================================================================
// Q 键输入
// ========================================================================
void UBombInteractionComponent::OnInteractKeyPressed()
{
	if (bIsInteracting) return;
	if (!OwnerCharacter || OwnerCharacter->IsElimmed()) return;
	ABlasterPlayerState* PS = OwnerCharacter->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	if (PS->TeamID == ETeamID::ETI_Attacker)
	{
		// Q 键只负责安包：未携带炸弹时不触发任何交互（拾取已移到 E 键）
		if (!IsCarryingBomb()) return;

		ABombSite* Site = FindNearestBombSite();
		if (Site && !Site->bIsBombPlantedHere)
		{
			bIsInteracting = true;
			CurrentInteraction = EBombInteractionType::EBIT_Planting;
			InteractionTarget = Site;
			InteractionElapsed = 0.f;
			OwnerCharacter->bDisableGameplayInput = true;
			Server_StartPlant(Site);
		}
		return;
	}

	if (PS->TeamID == ETeamID::ETI_Defender)
	{
		ABombActor* Bomb = FindNearestPlantedBomb();
		if (Bomb && !Bomb->IsInteracting())
		{
			bIsInteracting = true;
			CurrentInteraction = EBombInteractionType::EBIT_Defusing;
			InteractionTarget = Bomb;
			InteractionDuration = Bomb->DefuseDuration;
			InteractionElapsed = 0.f;
			OwnerCharacter->bDisableGameplayInput = true;
			Server_StartDefuse(Bomb);
		}
	}
}

void UBombInteractionComponent::OnInteractKeyReleased()
{
	// Hold-to-interact：松开 Q 键即取消当前交互
	if (!bIsInteracting) return;

	ForceCancelInteraction(); // 本地重置：bIsInteracting、InteractionTarget、禁用输入

	if (GetOwnerRole() != ROLE_Authority)
	{
		Server_CancelInteraction(); // RPC 通知服务器取消 BombActor 端 Timer
	}
}

// ========================================================================
// 进度
// ========================================================================
void UBombInteractionComponent::TickInteractionProgress(float DeltaTime)
{
	InteractionElapsed = FMath::Min(InteractionElapsed + DeltaTime, InteractionDuration);
}

float UBombInteractionComponent::GetInteractionProgress() const
{
	return (InteractionDuration > 0.f)
		? FMath::Clamp(InteractionElapsed / InteractionDuration, 0.f, 1.f) : 0.f;
}

bool UBombInteractionComponent::CanPlant() const
{ return CurrentInteraction == EBombInteractionType::EBIT_Planting && InteractionTarget; }
bool UBombInteractionComponent::CanDefuse() const
{ return CurrentInteraction == EBombInteractionType::EBIT_Defusing && InteractionTarget; }
bool UBombInteractionComponent::CanPickup() const
{
	ABombActor* Bomb = Cast<ABombActor>(InteractionTarget);
	return Bomb && Bomb->GetBombState() == EBombState::EBS_Carried && Bomb->GetOwner() == nullptr;
}

// ========================================================================
// 强制取消
// ========================================================================
void UBombInteractionComponent::ForceCancelInteraction()
{
	if (!bIsInteracting) return;
	bIsInteracting = false;
	CurrentInteraction = EBombInteractionType::EBIT_None;
	InteractionElapsed = 0.f;
	InteractionTarget = nullptr;
	if (OwnerCharacter) OwnerCharacter->bDisableGameplayInput = false;
	if (GetOwnerRole() == ROLE_Authority)
	{
		TArray<AActor*> FoundBombs;
		UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombActor::StaticClass(), FoundBombs);
		for (AActor* Actor : FoundBombs)
		{
			ABombActor* Bomb = Cast<ABombActor>(Actor);
			if (Bomb && Bomb->IsInteracting()) { Bomb->Server_CancelInteraction(); break; }
		}
	}
}

// ========================================================================
// RPC
// ========================================================================
void UBombInteractionComponent::Server_PickupBomb_Implementation(ABombActor* Bomb)
{
	if (!OwnerCharacter || !Bomb) return;
	ABlasterPlayerState* PS = OwnerCharacter->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->TeamID != ETeamID::ETI_Attacker) return;
	// 加固：服务器侧确认调用者当前未携带炸弹，堵住恶意客户端已带包仍抢包的缺口
	if (IsCarryingBomb()) return;
	if (Bomb->GetBombState() != EBombState::EBS_Carried || Bomb->GetOwner() != nullptr) return;
	float Dist = FVector::Dist(OwnerCharacter->GetActorLocation(), Bomb->GetActorLocation());
	if (Dist > MaxInteractDistance) return;
	Bomb->AssignToCarrier(OwnerCharacter);
}

void UBombInteractionComponent::Server_StartPlant_Implementation(ABombSite* Site)
{
	if (!OwnerCharacter || !Site) return;
	ABlasterPlayerState* PS = OwnerCharacter->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->TeamID != ETeamID::ETI_Attacker) return;
	if (Site->bIsBombPlantedHere) return;
	float Dist = FVector::Dist(OwnerCharacter->GetActorLocation(), Site->GetActorLocation());
	if (Dist > MaxInteractDistance) return;
	TArray<AActor*> FoundBombs;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABombActor::StaticClass(), FoundBombs);
	ABombActor* MyBomb = nullptr;
	for (AActor* Actor : FoundBombs)
	{
		ABombActor* Bomb = Cast<ABombActor>(Actor);
		if (Bomb && Bomb->GetBombState() == EBombState::EBS_Carried && Bomb->GetOwner() == OwnerCharacter)
			{ MyBomb = Bomb; break; }
	}
	if (!MyBomb) return;
	bIsInteracting = true;
	CurrentInteraction = EBombInteractionType::EBIT_Planting;
	InteractionTarget = Site;
	InteractionDuration = MyBomb->PlantDuration;
	InteractionElapsed = 0.f;
	OwnerCharacter->bDisableGameplayInput = true;
	MyBomb->Server_StartPlant(Site);
}

void UBombInteractionComponent::Server_StartDefuse_Implementation(ABombActor* Bomb)
{
	if (!OwnerCharacter || !Bomb) return;
	ABlasterPlayerState* PS = OwnerCharacter->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->TeamID != ETeamID::ETI_Defender) return;
	if (Bomb->GetBombState() != EBombState::EBS_Planted || Bomb->IsInteracting()) return;
	float Dist = FVector::Dist(OwnerCharacter->GetActorLocation(), Bomb->GetActorLocation());
	if (Dist > MaxInteractDistance) return;
	bIsInteracting = true;
	CurrentInteraction = EBombInteractionType::EBIT_Defusing;
	InteractionTarget = Bomb;
	InteractionDuration = Bomb->DefuseDuration;
	InteractionElapsed = 0.f;
	OwnerCharacter->bDisableGameplayInput = true;
	Bomb->Server_StartDefuse();
}

void UBombInteractionComponent::Server_CancelInteraction_Implementation()
{
	ForceCancelInteraction();
}
