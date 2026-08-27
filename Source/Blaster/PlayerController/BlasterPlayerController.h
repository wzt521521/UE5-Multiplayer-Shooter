// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Blaster/BlasterTypes/TeamTypes.h"
#include "BlasterPlayerController.generated.h"

class ABlasterHud;
class ABlasterCharacter;
class USoundCue;
enum class EThrowableType : uint8;

UCLASS()
class BLASTER_API ABlasterPlayerController : public APlayerController
{
	GENERATED_BODY()
public:
	void SetHUDHealth(float Health, float MaxHealth);
	void SetHUDShield(float Shield, float MaxShield);
	void SetHUDWeaponAmmo(int32 Ammo);
	void SetHUDCarriedAmmo(int32 Ammo);
void SetHUDMatchCountdown(float CountdownTime);
	void SetHUDPing(int32 Ping);

	// 弹药类型不匹配提示：显示绿色消息2秒后自动隐藏
	// 由 BlasterCharacter::ClientAmmoMismatchNotification RPC 调用
	void SetHUDMismatchNotification(const FString& Message);
	void SetHUDAnnouncementCountdown(float CountdownTime);
	virtual void OnPossess(APawn* InPawn) override;
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual float GetServerTime();
	// 仅当下行采样已完成切图预热，且调用者属于当前采样 World 时返回 true。
	// Character::OnRep_ReplicatedMovement 用它阻止切图期间继续写入旧的静态样本缓冲区。
	bool IsNetSmoothSamplingActive(const UWorld* SampleWorld) const;
	// P3 开火时间窗校验：是否已完成与服务端的时钟同步（服务端本地标志，不复制）。
	// 客户端每 3s 自动发起 ServerRequestServerTime，服务端收到请求即置 true；
	// 同步前跳过 ClientShotTime 窗口校验，避免热身期/刚进场误杀。
	FORCEINLINE bool HasSyncedServerTime() const { return bHasSyncedTime; }
	virtual void ReceivedPlayer() override;
	void OnMatchStateSet(FName State, bool bTeamsMatch = false);
	void HandleMatchHasStarted(bool bTeamsMatch = false);
	void HandleCooldown();

	// 回合制阵营模式：MatchState 处理
	void HandleAssignTeams();
	void HandleRoundPrepare();
	void HandleRoundInProgress();
	void HandleRoundEnd();
	void HandleMatchEnd();
	void HandleHalftimeSwap();

	// 回合信息 HUD 推送

	// 购买菜单生命周期：热身开始自动打开，B 键切换，比赛开始强制关闭
	void OpenBuyMenuOnWarmup();
	void ShowBuyMenu();
	void HideBuyMenu();
	void ToggleBuyMenu();

	// 投掷物选择面板：按住 G 键显示，点击图标确认选择
	void ShowThrowablePanel();
	void HideThrowablePanel();

	// 点击选中回调：由 ThrowableSelectionWheel 的 OnTypeClicked 委托触发
	UFUNCTION()
	void OnThrowableTypeClicked(EThrowableType Type);

	// 投掷物烹饪倒计时 HUD 推送：每帧由 ThrowableComponent::TickComponent 调用
	// bIsCooking=true → RemainingSeconds 为剩余秒数（如 1.3），显示倒计时文本
	// bIsCooking=false → 隐藏倒计时文本
	void SetHUDThrowableCooking(bool bIsCooking, float RemainingSeconds);

	// 闪光弹致盲 Client RPC：服务器调用，客户端触发全屏白屏淡出
	UFUNCTION(Client, Reliable)
	void ClientApplyFlashEffect(float Duration);

	// ── 会话 token RPC（P6 断线重连）──
	// 客户端重连出示：BeginPlay(IsLocalController) 调用，携带本地持久 token 请求服务器认证。
	// 服务器只查待重连表不写 PS token（防止覆盖 PostLogin 已签发的新 token，见 P0 计划 2.5）。
	UFUNCTION(Server, Reliable)
	void ServerAuthenticateSession(const FString& InToken);

	// 服务器签发 token 下发：LobbyGameMode::PostLogin 调用，客户端保存到本地文件（重连时出示）
	UFUNCTION(Client, Reliable)
	void ClientReceiveSessionToken(const FString& InToken);

	// P1 死亡观战 Client RPC：服务器在 OnPlayerKilled 调用，客户端本地进入观战状态，
	// 生成自由飞行 SpectatorPawn（仅本机）。Corpse = 死亡角色（死亡镜头视角锁它，不依赖 GetPawn()，
	// 因进入观战时 PC 已 UnPossess 尸体）。下轮重生时 ClientRestart → 自动退出观战恢复第三人称。
	UFUNCTION(Client, Reliable)
	void ClientEnterSpectator(ABlasterCharacter* Corpse);

	// P1 死亡观战（服务器端，GameMode::OnPlayerKilled 调用）：
	// 服务器 PC 也进入 Spectating 状态 + 下发 ClientEnterSpectator 让客户端进入。
	// 必须两侧状态一致 —— 否则引擎的 ServerSetSpectatorLocation → ClientGotoState 会把客户端
	// 强制同步回服务器状态（Playing），观战 ~200ms 即被拉回（P1 实测发现的坑）。
	// 尸体 3s 后由 BlasterCharacter::DestroyCorpse 销毁（本函数不销毁）。
	void EnterDeathSpectator(ABlasterCharacter* Corpse);

	// P2 中途加入观战（服务器端，BombDefusalGameMode::HandleMidRoundJoin 调用）：
	// 与死亡观战不同：无死亡镜头、无团队锁定 —— 新加入者自由飞行看比赛（中立观察者）。
	// 服务器 PC 也进入 Spectating（P1 约束：两侧一致，否则 ClientGotoState 拉回）。
	void EnterJoinSpectator();

	// P2 中途加入观战 Client RPC：客户端本地进入观战状态 → 自由飞行 SpectatorPawn + 隐藏 HUD。
	UFUNCTION(Client, Reliable)
	void ClientEnterJoinSpectator();

	// P3：真实 Bomb 登录的中途加入候选（服务器侧，不复制，BombDefusalGameMode::PostLogin 置位）。
	// Bomb PostLogin（bTeamsAssigned）置 true，ServerAuthenticateSession 新玩家分支消费后清 false。
	// 无缝切图带过来的 PC 此标志为 false → authenticate 每图重发时不会误触发 HandleMidRoundJoin（P3 问题 1 修复）。
	bool bIsMidJoinCandidate = false;

	// P3 主流方案：断开瞬间玩家是否存活（PawnPendingDestroy 捕获）。
	// 存活断开 → GameMode::Logout 递减 AliveCount；已死断开 → 死亡时已递减，勿重复。
	bool bWasAliveAtDisconnect = false;

	// ── 炸弹 UI 推送（BombMode Phase 4）──
	void UpdateBombStatusUI(float RemainingTime, float TotalTime, const FString& StatusText, const FString& SiteName);
	void UpdateBombInteractUI(float Progress, const FString& PromptText, bool bVisible, bool bShowProgress = false);
	void ShowBombPlantedAnnouncement(const FString& SiteName);
	void UpdateBombStatusFromWorld();  // Tick 中检查已安放炸弹 → 推 StatusWidget

	float SingleTripTime = 0.f;

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

	// P3 主流方案（致命修复）：AController::Destroyed 在 Logout 后调 CleanupPlayerState → 销毁 PS。
	// 若该 PS 已注册进待重连表（对局中断线），保留供重连换绑（经济/统计续存）；否则引擎默认销毁。
	virtual void CleanupPlayerState() override;
	// P3 主流方案：存活角色被断线销毁时捕获存活状态（已死尸体销毁前已 UnPossess，不走这里）
	virtual void PawnPendingDestroy(APawn* inPawn) override;

	void SetHUDTime();
	void PollInit();

	UFUNCTION(Server, Reliable)
	void ServerRequestServerTime(float TimeOfClientRequest);

	// 客户端上报本地持久 PlayerId（GUID，见 FBlasterPlayerIdentity）：
	// 服务器写入 PlayerState，供比赛结算时持久化到 SQLite（按人归集的键）
	UFUNCTION(Server, Reliable)
	void ServerSetPlayerId(const FString& InPlayerId);

	UFUNCTION(Client, Reliable)
	void ClientReportServerTime(float TimeOfClientRequest, float TimeServerReceivedClientRequest);

	float ClientServerDelta = 0.f;

	// 中位数滤波样本窗口：累积每次时间同步测得的偏移，取中位抗主机延迟尖峰。
	// 窗口大小由 CVar blaster.TimeSync.MedianWindowSize 控制（默认 7）
	TArray<float> DeltaSamples;

	// P3 开火时间窗校验：服务端收到过时间同步请求 → true（见 HasSyncedServerTime）
	bool bHasSyncedTime = false;

	UPROPERTY(EditAnywhere, Category = Time)
	float TimeSyncFrequency = 3.f;

	float TimeSyncRunningTime = 0.f;
	void CheckTimeSync(float DeltaTime);

	// ── 客户端插值平滑自适应（Phase 2 补下行）──
	// 采样"远端角色快照到达间隔"算抖动（纯下行），~1s 粒度驱动平滑时间 τ。
	// 间隔由 Character 的 OnRep_ReplicatedMovement 喂入 static PendingIntervals。
	// jitter 是连接级属性，共享一个 CurrentTau 应用到所有 simulated proxy。
	float NetSmoothLastSampleTime = 0.f;     // 距上次采样的累计秒数
	float CurrentTau = 0.05f;                // 平滑后的 τ（持久状态，向 TargetTau 匀速逼近）
	TWeakObjectPtr<UWorld> NetSmoothSamplingWorld; // PC 无缝切图保留时，用 World 变化识别采样生命周期边界
	float NetSmoothWarmupRemaining = 0.f;    // 新 World 建立后暂停采样，等待复制流稳定
	bool bNetSmoothSamplingActive = false;
	void UpdateNetSmoothAdaptive(float DeltaTime);

	UFUNCTION(Server, Reliable)
	void ServerCheckMatchState();

	// 购买请求 RPC（客户端 -> 服务器）
	// 客户端只传 ItemID，服务器从 DataTable 查表获取 Price/Category/Class
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestPurchase(int32 ItemID);
	friend class UBuyMenu;

	UFUNCTION(Client, Reliable)
	void ClientJoinMidgame(FName StateOfMatch, float Warmup, float Match, float Cooldown, float StartingTime);

private:
	UPROPERTY()
	ABlasterHud* BlasterHud;

	// 客户端兜底：无缝切图时服务器的 ClientSetHUD RPC 可能丢失（时序问题），
	// 导致 GetHUD() 仍是默认 AHUD、BlasterHud 为 null → 无 HUD。
	// 此方法在客户端直接加载并生成 BP_BlasterHUD，不依赖服务器 RPC。
	void EnsureBlasterHud();

	UPROPERTY()
	class UCharacteroverlay* CharacterOverlay;

	UPROPERTY()
	class ABlasterGameMode* BlasterGameMode;

	float LevelStartingTime = 0.f;
	float MatchTime = 0.f;
	float WarmupTime = 0.f;
	float CooldownTime = 0.f;
	uint32 CountdownInt = 0;

	UPROPERTY(ReplicatedUsing = OnRep_MatchState)
	FName MatchState;

	UFUNCTION()
	void OnRep_MatchState();

	float HUDHealth;
	bool bInitializeHealth = false;
	float HUDMaxHealth;
	float HUDShield;
	bool bInitializeShield = false;
	float HUDMaxShield;
	float HUDMatchCountdown;
	bool bInitializeMatchCountdown = false;
	int32 HUDCarriedAmmo;
	bool bInitializeCarriedAmmo = false;
	int32 HUDWeaponAmmo;
	bool bInitializeWeaponAmmo = false;

	// 不匹配提示自动隐藏 Timer（2秒）
	FTimerHandle MismatchNotificationTimer;
	void HideMismatchNotification();

	// 购买菜单是否正在显示，ShowBuyMenu/HideBuyMenu 维护此标志
	bool bBuyMenuOpen = false;

	// 客户端 GameState 复制延迟补偿：PollInit 中公告阶段每帧刷新公告文本

	// 投掷物径向选择面板是否正在显示，ShowThrowablePanel/HideThrowablePanel 维护此标志
	bool bThrowablePanelOpen = false;

	// P1 观战退出检测标志：ClientEnterSpectator 置 true，Tick 检测状态离开 NAME_Spectating 时记日志。
	// 仅客户端有意义（服务器 PC 不进入观战状态，恒为 false 无副作用）。
	bool bWasSpectating = false;

	// P1 死亡镜头阶段：进入观战后先看自己尸体（视角锁在尸体上），
	// 服务器 3s 销毁尸体 → 结束死亡镜头切队友视角/自由飞行。
	// 该阶段屏蔽自由飞行兜底的每帧 SetViewTarget（否则会把死亡镜头抢成 SpectatorPawn 视角）。
	bool bDeathCamPhase = false;

	// 死亡镜头锁定的尸体（RPC 传入，客户端弱引用）。服务器 3s 销毁后 IsValid() 为 false
	// → Tick 结束死亡镜头切队友视角。不能用 GetPawn()（进入观战时 PC 已 UnPossess 尸体）。
	TWeakObjectPtr<ABlasterCharacter> DeathCamCorpse;

	// ── P1 观战：锁定存活同阵营队友（客户端本地，不复制）──
	// 当前观战锁定的队友 Pawn；nullptr = 自由飞行中（无存活队友兜底）。
	// 视角通过客户端本地 SetViewTarget(队友Pawn) 锁定，重生 ClientRestart 自动接管。
	TWeakObjectPtr<ABlasterCharacter> SpectateTarget;

	// 收集存活同阵营队友（!IsElimmed && BlasterPlayerState->TeamID 相同；bElimmed/TeamID 均已复制）
	TArray<ABlasterCharacter*> CollectAliveTeammates() const;
	// 观战进入/目标死亡时调用：锁定第一个存活队友，无则自由飞行兜底
	void UpdateSpectateTarget();
	// 箭头键 ↑↓←→ 按下 → 切换下一个存活队友（循环）
	void CycleSpectateTarget();
	// 当前观战目标是否仍存活（未销毁且未 elimmed）
	bool IsSpectateTargetAlive() const;

	// 闪光弹配置已迁移到 BlasterHud

	FString GetInfoText(const TArray<class ABlasterPlayerState*>& Players);

	// ── 运行时 tick 率日志（多客户端失焦节流诊断）──
	// Tick 里每 5s 打一次实际帧率，确认失焦窗口的游戏逻辑 tick 是否满速
	int32 TickRateLogFrameCount = 0;
	float TickRateLogElapsed = 0.f;
};
