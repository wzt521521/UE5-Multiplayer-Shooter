#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Blaster/BombMode/BombTypes.h"
#include "BombActor.generated.h"

class ABombSite;
class UStaticMeshComponent;
class USphereComponent;
class UWidgetComponent;

// 炸弹实体：封装生命周期状态机 + 倒计时 + 事件广播。
// 不依赖任何 GameMode/Character 类型，只通过 Delegate 通知外部订阅者。
// 服务器权威：BombState 和 RemainingTime 由服务器 Tick 驱动，客户端通过 OnRep 同步。

// ── 事件委托：GameMode 订阅这些事件来判定胜负 ──
DECLARE_MULTICAST_DELEGATE_OneParam(FOnBombPlantedSignature, ABombSite* Site);
DECLARE_MULTICAST_DELEGATE(FOnBombExplodedSignature);
DECLARE_MULTICAST_DELEGATE(FOnBombDefusedSignature);

UCLASS(BlueprintType, Blueprintable)
class BLASTER_API ABombActor : public AActor
{
	GENERATED_BODY()

public:
	ABombActor();
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ── 状态查询 ──
	UFUNCTION(BlueprintPure)
	EBombState GetBombState() const { return BombState; }
	UFUNCTION(BlueprintPure)
	float GetRemainingTime() const { return RemainingTime; }
	UFUNCTION(BlueprintPure)
	ABombSite* GetPlantedSite() const { return PlantedSite; }
	UFUNCTION(BlueprintPure)
	bool IsInteracting() const { return bIsInteracting; }

	// ── 交互入口（由 UBombInteractionComponent 的 RPC 调用，已在服务器端）──

	// 开始安包：记录点位，启动 5 秒 FTimerHandle
	void Server_StartPlant(ABombSite* Site);

	// 安包完成：进入 Planted 状态，启动炸弹倒计时
	void Server_CompletePlant();

	// 开始拆包：启动 5 秒 FTimerHandle
	void Server_StartDefuse();

	// 拆包完成：进入 Defused 状态
	void Server_CompleteDefuse();

	// 取消当前交互（被打断：移动/死亡/受击），不改变状态
	void Server_CancelInteraction();

	// ── 携带逻辑（由 GameMode 调用）──

	// GameMode 分配炸弹给攻方：Attach 到 Character，进入 Carried 状态
	void AssignToCarrier(class ABlasterCharacter* Carrier);

	// 携带者死亡时：从身上 Detach，掉落在地面
	void DropAtLocation(const FVector& Location);

	// 显示/隐藏掉落拾取提示（与 AWeapon::ShowPickupWidget 同款接口）
	void ShowPickupWidget(bool bShowWidget);

	// ── 事件订阅（GameMode BeginPlay 时绑定）──
	FOnBombPlantedSignature OnBombPlanted;
	FOnBombExplodedSignature OnBombExploded;
	FOnBombDefusedSignature OnBombDefused;

	// ── 可配置参数 ──

	// 安包时长（秒），默认 5 秒
	UPROPERTY(EditDefaultsOnly, Category = "Bomb Config")
	float PlantDuration = 5.f;

	// 拆包时长（秒），默认 5 秒
	UPROPERTY(EditDefaultsOnly, Category = "Bomb Config")
	float DefuseDuration = 5.f;

	// 爆炸倒计时总长（秒），默认 40 秒
	UPROPERTY(EditDefaultsOnly, Category = "Bomb Config")
	float BombCountdown = 40.f;

	// 交互最大距离（cm），InteractionComponent 检查此值
	UPROPERTY(EditDefaultsOnly, Category = "Bomb Config")
	float MaxInteractDistance = 200.f;

protected:
	virtual void BeginPlay() override;

	// ── 子组件 ──

	// 炸弹静态模型：Cube 占位，蓝图可替换
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	UStaticMeshComponent* BombMesh;

	// 交互检测球：守方进入此范围才能拆包
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	USphereComponent* InteractSphere;

	// 拾取提示 Widget：掉落可拾取时显示（武器同款 WBP_PickupWidget，蓝图里赋 WidgetClass）
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	UWidgetComponent* PickupWidget;

private:
	// ── 网络复制属性 ──

	// 核心状态：服务器权威，OnRep 通知所有客户端刷新 UI
	UPROPERTY(ReplicatedUsing = OnRep_BombState)
	EBombState BombState = EBombState::EBS_Idle;

	// 剩余爆炸时间：仅 Planted 时有效，每 0.5s 更新一次（减少带宽）
	UPROPERTY(ReplicatedUsing = OnRep_RemainingTime)
	float RemainingTime = 0.f;

	// 被安放在哪个点位
	UPROPERTY(Replicated)
	ABombSite* PlantedSite = nullptr;

	// ── 服务器私有状态（不需要复制）──

	// 当前交互定时器句柄（5 秒安包 / 5 秒拆包）
	FTimerHandle InteractionTimer;

	// 交互锁：InteractionComponent / GameMode 检查此标志决定是否允许操作
	bool bIsInteracting = false;

	// 当前交互类型（Plant/Defuse）。用于 Complete 时区分调用 Plant 还是 Defuse 完成逻辑
	EBombInteractionType CurrentInteractionType = EBombInteractionType::EBIT_None;

	// ── OnRep 回调 ──
	UFUNCTION()
	void OnRep_BombState();

	UFUNCTION()
	void OnRep_RemainingTime();

	// ── 内部逻辑 ──
	void TickBombCountdown(float DeltaTime);   // Planted 状态下每帧递减 RemainingTime
	void Explode();                            // 倒计时归零 → Exploded → 广播事件
	void SetBombState(EBombState NewState);    // 统一状态变更入口（服务器调用）

	// 配置可视化碰撞体在编辑器中可见
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
