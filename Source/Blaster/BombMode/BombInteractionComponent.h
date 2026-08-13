#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Blaster/BombMode/BombTypes.h"
#include "BombInteractionComponent.generated.h"

class ABombActor;
class ABombSite;
class ABlasterCharacter;

// 炸弹交互：E 键拾取掉落炸弹（武器同款 PickupWidget + 服务器校验），
// Q 键安包/拆包，同一套"条件检测→RPC→进度→完成/打断"流水线。
// 优先级：攻方没包时靠近掉落炸弹 → 世界空间拾取提示（E 键拾取）；有包时靠近下包点 → 安包；守方靠近已安放炸弹 → 拆包。
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class BLASTER_API UBombInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UBombInteractionComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	void OnInteractKeyPressed();
	void OnInteractKeyReleased();

	// E 键拾取入口：客户端调用，内部走私有 Server_PickupBomb 完成服务器权威校验
	void PickupDroppedBomb();

	UFUNCTION(BlueprintPure)
	bool IsInteracting() const { return bIsInteracting; }
	UFUNCTION(BlueprintPure)
	float GetInteractionProgress() const;
	UFUNCTION(BlueprintPure)
	EBombInteractionType GetCurrentInteraction() const { return CurrentInteraction; }
	UFUNCTION(BlueprintPure)
	AActor* GetInteractionTarget() const { return InteractionTarget; }
	UFUNCTION(BlueprintPure)
	bool CanPlant() const;
	UFUNCTION(BlueprintPure)
	bool CanDefuse() const;
	UFUNCTION(BlueprintPure)
	bool CanPickup() const;

	// 返回当前可拾取的掉落炸弹（InteractionTarget 命中时），供 E 键拾取分支使用
	ABombActor* GetPickupBomb() const;

	void ForceCancelInteraction();

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY()
	ABlasterCharacter* OwnerCharacter = nullptr;

	EBombInteractionType CurrentInteraction = EBombInteractionType::EBIT_None;
	AActor* InteractionTarget = nullptr;
	bool bIsInteracting = false;
	float InteractionDuration = 5.f;
	float InteractionElapsed = 0.f;

	// 上一帧显示了拾取提示的炸弹：用于目标变化/不再可拾取时隐藏（本地客户端状态）
	ABombActor* LastPickupTarget = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "Bomb Config")
	float MaxInteractDistance = 200.f;

	// 客户端检测
	void DetectNearbyTarget();
	void UpdatePickupWidget();                 // 驱动掉落炸弹的世界空间 PickupWidget 显隐
	void PushInteractUI();                     // 推送交互提示到 HUD InteractWidget
	ABombSite* FindNearestBombSite() const;
	ABombActor* FindNearestPlantedBomb() const;
	ABombActor* FindNearestDroppedBomb() const;
	bool IsCarryingBomb() const;

	void TickInteractionProgress(float DeltaTime);

	// RPC
	UFUNCTION(Server, Reliable)
	void Server_PickupBomb(ABombActor* Bomb);
	UFUNCTION(Server, Reliable)
	void Server_StartPlant(ABombSite* Site);
	UFUNCTION(Server, Reliable)
	void Server_StartDefuse(ABombActor* Bomb);
	UFUNCTION(Server, Reliable)
	void Server_CancelInteraction();
};
