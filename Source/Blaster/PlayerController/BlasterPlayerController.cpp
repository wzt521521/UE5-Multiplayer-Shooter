// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterPlayerController.h"
#include "Blaster/HUD/BlasterHud.h"
#include "Blaster/HUD/Characteroverlay.h"
#include "Blaster/HUD/Announcement.h"
#include "Blaster/HUD/BuyMenu.h"
#include "Blaster/HUD/ThrowableSelectionWheel.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Sound/SoundCue.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/BlasterComponents/ThrowableComponent.h"
#include "GameFramework/SpectatorPawn.h"   // P1 观战：GetSpectatorPawn() 转 AActor* 需要完整类型
#include "Net/UnrealNetwork.h"
#include "Blaster/GameMode/BlasterGameMode.h"
#include "Blaster/GameMode/BombDefusalGameMode.h"
#include "Blaster/HUD/RoundOverlay.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/BlasterTypes/Announcement.h"
#include "Kismet/GameplayStatics.h"
#include "Blaster/BlasterTypes/ShopTypes.h"
#include "Blaster/BombMode/BombStatusWidget.h"   // 炸弹状态 HUD
#include "Blaster/BombMode/BombInteractWidget.h" // 炸弹交互进度条
#include "Blaster/BombMode/BombActor.h"          // 查找已安放炸弹
#include "Blaster/BombMode/BombSite.h"           // 读取点位名
#include "Blaster/Persistence/PlayerIdentity.h"  // P4 持久身份：客户端本地 PlayerId
#include "Blaster/Session/SessionManagerSubsystem.h"  // P6 会话：token 落盘/读取/待重连表查询
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"              // TActorIterator（Phase 2 遍历远端角色应用 τ）
#include "Engine/NetConnection.h"     // GetAverageLag()（Phase 2 jitter 采样源）

void ABlasterPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (InputComponent)
	{
		// B 键绑定购买菜单开关，仅在热身阶段生效（ToggleBuyMenu 内部检查 MatchState）
		InputComponent->BindAction("OpenBuyMenu", IE_Pressed, this, &ABlasterPlayerController::ToggleBuyMenu);

		// P1 观战切换：箭头键 ↑↓←→ → 切换下一个存活队友（CycleSpectateTarget 内部检查观战状态）。
		// 用 ActionMapping（DefaultInput.ini 里 SpectateSwitch 映射到 Up/Down/Left/Right），
		// 箭头键不被 SpectatorPawn（ADefaultPawn 只绑 WASD/鼠标）占用，与自由飞行无冲突。
		InputComponent->BindAction("SpectateSwitch", IE_Pressed, this, &ABlasterPlayerController::CycleSpectateTarget);
	}
}

void ABlasterPlayerController::BeginPlay()
{
	Super::BeginPlay();
	BlasterHud = Cast<ABlasterHud>(GetHUD());
	EnsureBlasterHud(); // 兜底：无缝切图时 ClientSetHUD RPC 可能丢失，客户端自己生成 HUD
	//应该添加annocuncement
	//announcement已经通过ServerCheckMatchState()由客户端独自添加
	ServerCheckMatchState();

	// P4 持久身份上报：客户端读取本地持久 PlayerId 上报服务器（仅本地客户端执行；
	// 服务器端 BeginPlay 也会进入，但 IsLocalController() 为 false 跳过，避免服务器自己生成 ID）
	if (IsLocalController())
	{
		ServerSetPlayerId(FBlasterPlayerIdentity::GetPlayerId());
		// P6 会话：携带本地持久 token 请求认证。首次连接无 token（空串）→ 服务器忽略，
		// 登录后由 Lobby PostLogin 签发并通过 ClientReceiveSessionToken 落盘；
		// 断线重连时读出的旧 token 用于在待重连表中定位留场状态（P3）。
		ServerAuthenticateSession(UBlasterSessionManager::LoadLocalToken());
	}
}

// 客户端兜底：服务器 ClientSetHUD 在无缝切图时可能未送达客户端（RPC 时序），
// 导致 GetHUD() 仍是默认 AHUD、BlasterHud 为 null → 无 HUD。
// 这里客户端直接加载 BP_BlasterHUD 并调用 ClientSetHUD（本地执行）生成正确 HUD。
void ABlasterPlayerController::EnsureBlasterHud()
{
	if (!IsLocalController()) return;   // 仅客户端本机执行（服务器端 PC 跳过）
	if (BlasterHud) return;             // 已有正确 HUD，无需兜底

	BlasterHud = Cast<ABlasterHud>(GetHUD());
	if (BlasterHud) return;

	UClass* HudClass = StaticLoadClass(AHUD::StaticClass(), nullptr,
		TEXT("/Game/Blueprints/HUD/BP_BlasterHUD.BP_BlasterHUD_C"));
	if (HudClass)
	{
		// ClientSetHUD 是 Client RPC：客户端本地调用 = 直接执行实现（不产生网络流量），
		// 由引擎正确设置 MyHUD 并生成指定类 HUD
		ClientSetHUD(HudClass);
		BlasterHud = Cast<ABlasterHud>(GetHUD());
		if (BlasterHud)
		{
			UE_LOG(LogTemp, Log, TEXT("[HUD] EnsureBlasterHud → 客户端兜底生成 BlasterHud 成功"));
		}
	}
}

void ABlasterPlayerController::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	SetHUDTime();
	CheckTimeSync(DeltaTime);
	UpdateNetSmoothAdaptive(DeltaTime);
	PollInit();

	// ── 运行时 tick 率日志（仅客户端本机 PC）：每 5s 打一次实际帧率 ──
	// 用途：多客户端测试时确认失焦窗口的游戏逻辑 tick 是否满速。
	// GPU 饥饿可能只压渲染、也可能拖慢 tick（引擎等渲染）；日志标 [FPS]，客户端日志 grep 即见。
	if (IsLocalController())
	{
		++TickRateLogFrameCount;
		TickRateLogElapsed += DeltaTime;
		if (TickRateLogElapsed >= 5.f)
		{
			const float FPS = TickRateLogFrameCount / TickRateLogElapsed;
			UE_LOG(LogTemp, Warning, TEXT("[FPS] tick_rate=%.1f (frames=%d / %.1fs)"),
				FPS, TickRateLogFrameCount, TickRateLogElapsed);
			TickRateLogFrameCount = 0;
			TickRateLogElapsed = 0.f;
		}
	}

	// 每帧更新客户端 ping 显示，PlayerState::GetPingInMilliseconds 引擎内置复制
	if (GetPlayerState<APlayerState>())
	{
		SetHUDPing(FMath::RoundToInt(GetPlayerState<APlayerState>()->GetPingInMilliseconds()));
	}

	// P1 观战退出检测（客户端）：重生时服务器 RestartPlayer → ClientRestart → 客户端自动
	// ChangeState(NAME_Playing) → 状态离开 NAME_Spectating 即代表第三人称恢复。仅日志，不干预逻辑。
	if (bWasSpectating && !IsInState(NAME_Spectating))
	{
		bWasSpectating = false;
		bDeathCamPhase = false;
		DeathCamCorpse = nullptr;
		SpectateTarget = nullptr;   // 观战目标随退出清空（重生后由 OnPossess/ClientRestart 接管视角）
		UE_LOG(LogTemp, Log, TEXT("[Spectate] 客户端退出观战（重生恢复第三人称）| PC=%s"), *GetName());
	}

	// P1 死亡镜头结束：服务器 3s 销毁尸体 → 客户端 DeathCamCorpse（弱引用）失效
	// → 结束死亡镜头，切队友视角（无存活队友则自由飞行）。与尸体销毁精确同步。
	if (bWasSpectating && bDeathCamPhase && !DeathCamCorpse.IsValid())
	{
		bDeathCamPhase = false;
		DeathCamCorpse = nullptr;
		UpdateSpectateTarget();
	}

	// P1 v2 观战：当前锁定的队友死亡/销毁 → 自动切换下一个存活队友，无则自由飞行。
	// 死亡镜头阶段 SpectateTarget 恒为 null，不参与。
	if (bWasSpectating && !bDeathCamPhase && SpectateTarget.IsValid() && !IsSpectateTargetAlive())
	{
		UpdateSpectateTarget();
	}

	// P1 v2 观战：自由飞行兜底时确保视角在 SpectatorPawn 上。
	// SpectatorPawn 可能晚于进入观战生成（GameState->SpectatorClass 复制到达后才 BeginSpectatingState），
	// 此时 GetSpectatorPawn() 才非空 → 每帧补一次 SetViewTarget，避免视角滞留在死尸身上。
	// 死亡镜头阶段屏蔽（否则会把"看尸体"的视角每帧抢成 SpectatorPawn）。
	if (bWasSpectating && !bDeathCamPhase && !SpectateTarget.IsValid() && GetSpectatorPawn() && GetViewTarget() != GetSpectatorPawn())
	{
		SetViewTarget(GetSpectatorPawn());
	}

	// P1 方案2·正常第三人称环绕：相机在队友身后固定距离，观战者转视角时环绕队友。
	// 位置 = 队友位置 - 观战者视角方向 × OrbitDistance（600，与正常游戏相机 TargetArmLength 一致）
	// 相机点随视角方向移动 → 队友始终在画面中心，视角自由环绕，画面=正常第三人称。
	// ⚠ ADefaultPawn（SpectatorPawn 基类）无相机组件，相机即自身位置，故直接定位到目标相机点（无嵌入）。
	// 队友位置是服务器复制的（客户端已有最新值），纯本地操作，无网络/物理开销。
	// 死亡镜头阶段不锚定（视角锁尸体）；无存活队友时不锚定（自由飞行）。
	if (bWasSpectating && !bDeathCamPhase && SpectateTarget.IsValid() && GetSpectatorPawn())
	{
		const float OrbitDistance = 300.f;   // 身后距离（比正常相机近，观战更贴近）
		const float HeightOffset  = 100.f;   // 抬高（略俯视队友）
		const FVector AnchorPos = SpectateTarget->GetActorLocation()
			- GetSpectatorPawn()->GetActorForwardVector() * OrbitDistance
			+ FVector(0.f, 0.f, HeightOffset);
		GetSpectatorPawn()->SetActorLocation(AnchorPos);
	}

	// 炸弹状态 HUD：检查是否有已安放的炸弹 → 推送倒计时和点位名
	UpdateBombStatusFromWorld();
}

void ABlasterPlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ABlasterPlayerController, MatchState);
}

void ABlasterPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(InPawn);
	if (BlasterCharacter)
	{
		SetHUDHealth(BlasterCharacter->GetHealth(), BlasterCharacter->GetMaxHealth());
		SetHUDShield(BlasterCharacter->GetShield(), BlasterCharacter->GetMaxShield());
	}
}

void ABlasterPlayerController::SetHUDHealth(float Health, float MaxHealth)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay
		&& BlasterHud->CharacterOverlay->HealthBar && BlasterHud->CharacterOverlay->HealthText;

	if (bHUDValid)
	{
		const float HealthPercent = Health / MaxHealth;
		BlasterHud->CharacterOverlay->HealthBar->SetPercent(HealthPercent);
		FString HealthTextStr = FString::Printf(TEXT("%d/%d"), FMath::CeilToInt(Health), FMath::CeilToInt(MaxHealth));
		BlasterHud->CharacterOverlay->HealthText->SetText(FText::FromString(HealthTextStr));
	}
	else
	{
		bInitializeHealth = true;
		HUDHealth = Health;
		HUDMaxHealth = MaxHealth;
	}
}

void ABlasterPlayerController::SetHUDShield(float Shield, float MaxShield)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay
		&& BlasterHud->CharacterOverlay->ShieldBar && BlasterHud->CharacterOverlay->ShieldText;

	if (bHUDValid)
	{
		const float ShieldPercent = Shield / MaxShield;
		BlasterHud->CharacterOverlay->ShieldBar->SetPercent(ShieldPercent);
		FString ShieldTextStr = FString::Printf(TEXT("%d/%d"), FMath::CeilToInt(Shield), FMath::CeilToInt(MaxShield));
		BlasterHud->CharacterOverlay->ShieldText->SetText(FText::FromString(ShieldTextStr));
	}
	else
	{
		bInitializeShield = true;
		HUDShield = Shield;
		HUDMaxShield = MaxShield;
	}
}

void ABlasterPlayerController::SetHUDWeaponAmmo(int32 Ammo)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay && BlasterHud->CharacterOverlay->AmmoAmount;
	if (bHUDValid)
	{
		FString AmmoText = FString::Printf(TEXT("%d"), Ammo);
		BlasterHud->CharacterOverlay->AmmoAmount->SetText(FText::FromString(AmmoText));
	}
	else
	{
		// 延迟缓存：Overlay 尚未创建时缓存数据，PollInit 中推送
		bInitializeWeaponAmmo = true;
		HUDWeaponAmmo = Ammo;
	}
}

void ABlasterPlayerController::SetHUDCarriedAmmo(int32 Ammo)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay && BlasterHud->CharacterOverlay->CarriedAmmoAmount;
	if (bHUDValid)
	{
		FString AmmoText = FString::Printf(TEXT("%d"), Ammo);
		BlasterHud->CharacterOverlay->CarriedAmmoAmount->SetText(FText::FromString(AmmoText));
	}
	else
	{
		bInitializeCarriedAmmo = true;
		HUDCarriedAmmo = Ammo;
	}
}

// ------------------------------------------------------------
// 延迟显示：每帧 Tick 读取 PlayerState::GetPingInMilliseconds()
// 引擎内置复制，客户端直接读取即可，无需额外网络同步
// ------------------------------------------------------------
void ABlasterPlayerController::SetHUDPing(int32 Ping)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay && BlasterHud->CharacterOverlay->PingText;
	if (bHUDValid)
	{
		FString PingStr = FString::Printf(TEXT("%d ms"), Ping);
		BlasterHud->CharacterOverlay->PingText->SetText(FText::FromString(PingStr));
	}
}

void ABlasterPlayerController::SetHUDMismatchNotification(const FString& Message)
{
	// 获取 HUD 和 CharacterOverlay，直接设置 MismatchNotificationText 控件
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay
		&& BlasterHud->CharacterOverlay->MismatchNotificationText;

	if (bHUDValid)
	{
		// 设置绿色提示文本并显示
		BlasterHud->CharacterOverlay->MismatchNotificationText->SetText(FText::FromString(Message));
		BlasterHud->CharacterOverlay->MismatchNotificationText->SetVisibility(ESlateVisibility::Visible);

		// 启动2秒 Timer，到期后调用 HideMismatchNotification 隐藏文本
		// SetTimer 会覆盖已有的 Timer，重复触发时自动重置倒计时
		GetWorldTimerManager().SetTimer(
			MismatchNotificationTimer,
			this,
			&ABlasterPlayerController::HideMismatchNotification,
			2.0f
		);
	}
}

void ABlasterPlayerController::HideMismatchNotification()
{
	// Timer 到期：隐藏不匹配提示文本
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud && BlasterHud->CharacterOverlay
		&& BlasterHud->CharacterOverlay->MismatchNotificationText)
	{
		BlasterHud->CharacterOverlay->MismatchNotificationText->SetVisibility(ESlateVisibility::Hidden);
	}
}

void ABlasterPlayerController::SetHUDMatchCountdown(float CountdownTime)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud &&
		BlasterHud->CharacterOverlay &&
		BlasterHud->CharacterOverlay->MatchCountdownText;
	if (bHUDValid)
	{
		if (CountdownTime < 0.f)
		{
			BlasterHud->CharacterOverlay->MatchCountdownText->SetText(FText());
			return;
		}

		int32 Minutes = FMath::FloorToInt(CountdownTime / 60.f);
		int32 Seconds = CountdownTime - Minutes * 60;

		FString CountdownText = FString::Printf(TEXT("%02d:%02d"), Minutes, Seconds);
		BlasterHud->CharacterOverlay->MatchCountdownText->SetText(FText::FromString(CountdownText));
	}
	else
	{
		bInitializeMatchCountdown = true;
		HUDMatchCountdown = CountdownTime;
	}
}

void ABlasterPlayerController::SetHUDAnnouncementCountdown(float CountdownTime)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	// 公告面板现在"创建但不初始化"（见 InitializeHUD 注释）：写入倒计时前必须确保它已加入
	// viewport 完成初始化（BindWidget 解析 + NativeConstruct 绑定 GameState），否则 WarmupTime 为空
	if (BlasterHud)
	{
		BlasterHud->EnsureAnnouncement();
	}
	// 兜底：如果 Announcement 还未创建（ClientJoinMidgame 时 HUD 可能未就绪），在这里补创建
	bool bHUDValid = BlasterHud &&
		BlasterHud->Announcement &&
		BlasterHud->Announcement->WarmupTime;
	if (bHUDValid)
	{
		if (CountdownTime < 0.f)
		{
			BlasterHud->Announcement->WarmupTime->SetText(FText());
			return;
		}

		int32 Minutes = FMath::FloorToInt(CountdownTime / 60.f);
		int32 Seconds = CountdownTime - Minutes * 60;

		FString CountdownText = FString::Printf(TEXT("%02d:%02d"), Minutes, Seconds);
		BlasterHud->Announcement->WarmupTime->SetText(FText::FromString(CountdownText));
	}
}

// ------------------------------------------------------------
// 时间同步中位数滤波窗口（样本数）：每 5s 一个样本，N=7 需 35s 填满。
// 抗主机延迟尖峰：单次采样会被某次系统忙的读数带偏，取中位更稳。
TAutoConsoleVariable<int32> CVarTimeSyncMedianWindow(
	TEXT("blaster.TimeSync.MedianWindowSize"),
	7,
	TEXT("时间同步中位数滤波窗口（样本数），抗主机延迟尖峰"),
	ECVF_Default
);

// 时间同步：客户端定期向服务器请求时间，计算出 ClientServerDelta
// 然后 GetServerTime() 就能返回接近服务器的时间
// ------------------------------------------------------------
void ABlasterPlayerController::CheckTimeSync(float DeltaTime)
{
	TimeSyncRunningTime += DeltaTime;
	if (IsLocalController() && TimeSyncRunningTime > TimeSyncFrequency)
	{
		ServerRequestServerTime(GetWorld()->GetTimeSeconds());
		TimeSyncRunningTime = 0.f;
	}
}

void ABlasterPlayerController::ServerRequestServerTime_Implementation(float TimeOfClientRequest)
{
	float ServerTimeOfReceipt = GetWorld()->GetTimeSeconds();
	// P3 时间窗校验：收到同步请求即标记该玩家时钟可校验（客户端 GetServerTime 已基于本服务端时间）
	bHasSyncedTime = true;
	ClientReportServerTime(TimeOfClientRequest, ServerTimeOfReceipt);
}

// P4 持久身份：服务器收到客户端上报的 PlayerId → 存入 PlayerState，
// 比赛结算时 BombDefusalGameMode 读取并写入 SQLite。仅存数据，不做校验（写入时才校验）。
void ABlasterPlayerController::ServerSetPlayerId_Implementation(const FString& InPlayerId)
{
	// GetPlayerState 是模板方法，需显式指定类型
	if (ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>())
	{
		PS->SetPlayerId(InPlayerId);
	}
}

// P6 会话认证（服务器执行）：客户端重连时出示本地 token。
// 查 SessionManager 待重连表 → 命中表示断线留场（P3 恢复逻辑）；未命中 = 新玩家。
// 只查表不写 PS token —— 防止把 PostLogin 已签发的新 token 冲掉（见 P0 计划 2.5）。
// P3 重构：改成重连/新玩家的统一入口（命中待重连表 → 恢复；新玩家 → 中途加入 setup）。
void ABlasterPlayerController::ServerAuthenticateSession_Implementation(const FString& InToken)
{
	UBlasterSessionManager* Mgr = UBlasterSessionManager::Get();
	if (!Mgr) return;

	// ① 重连恢复：token 命中待重连表 → Bomb 图恢复（换绑 PS + Possess 或进观战）
	if (!InToken.IsEmpty())
	{
		if (FPendingSession* Pending = Mgr->FindPendingSession(InToken))
		{
			// 命中表 = 本连接已被分类为"重连者" → 归零中途加入候选标志。
			// 若不归零，PostLogin 已置的 true 会残留到下次地图切换：
			// authenticate 每图重发时 ② 分支误触发 HandleMidRoundJoin，把老玩家当中途加入重置（P3 问题 1 同类）。
			bIsMidJoinCandidate = false;
			if (ABombDefusalGameMode* GM = GetWorld()->GetAuthGameMode<ABombDefusalGameMode>())
			{
				GM->RestoreReconnectedPlayer(this, *Pending, InToken);
				return;
			}
			// 命中但非 Bomb 图（重连到 Lobby，比赛未开始）→ 消费 pending，走 Lobby 正常流程
			Mgr->RemovePendingSession(InToken);
			UE_LOG(LogTemp, Log, TEXT("[Session] 重连到 Lobby（比赛未开始）→ 消费待重连条目 | token=%s"), *InToken);
			return;
		}
	}

	// ② 新玩家：仅真实 Bomb 登录（flag 守卫，防无缝切图重发的 authenticate 误触发中途加入）。
	//    token 在此统一签发（幂等）+ 下发 —— 避开 PostLogin 下发新 token 覆盖客户端文件的竞态（P3 问题 3 加固）。
	if (bIsMidJoinCandidate)
	{
		bIsMidJoinCandidate = false;
		const FString Token = Mgr->IssueToken(this);
		ClientReceiveSessionToken(Token);
		if (ABombDefusalGameMode* GM = GetWorld()->GetAuthGameMode<ABombDefusalGameMode>())
		{
			GM->HandleMidRoundJoin(this);
		}
		UE_LOG(LogTemp, Log, TEXT("[Session] ServerAuthenticateSession → 新玩家（真实 Bomb 登录）| token=%s"), *Token);
	}
}

// P6 会话 token 下发（客户端执行）：服务器签发后推送，客户端保存到本地文件供重连出示。
void ABlasterPlayerController::ClientReceiveSessionToken_Implementation(const FString& InToken)
{
	if (InToken.IsEmpty()) return;
	UBlasterSessionManager::SaveLocalToken(InToken);
}

// P1 死亡观战（客户端执行）：服务器在 OnPlayerKilled 通知，Corpse = 死亡角色。
// 仅客户端本地进入观战状态 → BeginSpectatingState → 生成 SpectatorPawn（SetReplicates(false)，仅本机）自由飞行。
// 不置 bIsOnlyASpectator（否则 RestartPlayer/OnPossess 拒绝重生，见 P1 计划 2.1）；
// 不置 bIsSpectator（否则 GetPlayersInLogicalTeam/ExecuteHalftimeSwap 把死玩家当观战者，丢经济/不翻转，见 2.3）。
// 死亡镜头先锁尸体 3s，尸体被服务器 DestroyCorpse 销毁后再切队友视角/自由飞行。
void ABlasterPlayerController::ClientEnterSpectator_Implementation(ABlasterCharacter* Corpse)
{
	// 幂等：已在观战状态则不重复进入
	if (IsInState(NAME_Spectating)) return;

	// 观战 Pawn 出生点 = 死尸位置（视角从死亡点接续，而非回到出生点）
	if (Corpse)
	{
		SetSpawnLocation(Corpse->GetActorLocation());
	}

	// 客户端本地进入观战状态（APlayerController::ChangeState 是 public）→ BeginSpectatingState。
	// 重生时服务器 RestartPlayer → ClientRestart → 客户端自动 ChangeState(NAME_Playing) → 退出观战。
	ChangeState(NAME_Spectating);

	bWasSpectating = true;
	bDeathCamPhase = true;   // 死亡镜头阶段：先看自己尸体，尸体销毁后再切队友视角
	UE_LOG(LogTemp, Log, TEXT("[Spectate] 客户端进入观战状态（死亡镜头）| PC=%s"), *GetName());

	// 死亡镜头：视角锁在自己尸体上（Corpse 由 RPC 传入 —— 进入观战时服务器已 UnPossess 尸体，
	// 客户端 GetPawn() 可能已是 null，不能用它拿尸体）。尸体 3s 后被销毁 → DeathCamCorpse 失效 → 切队友视角。
	DeathCamCorpse = Corpse;
	if (Corpse)
	{
		SetViewTarget(Corpse);
	}

	// P1 观战 HUD：隐藏自己角色的战斗 HUD（血条/护盾/弹药），切换到观战状态。
	// 重生后由 MatchState 流程（RoundInProgress → HandleRoundInProgress → ShowCharacterOverlay）恢复。
	if (BlasterHud)
	{
		BlasterHud->HideCharacterOverlay();
		UE_LOG(LogTemp, Log, TEXT("[Spectate] 客户端隐藏角色 HUD（进入观战状态）| PC=%s"), *GetName());
	}
}

// P1 死亡观战（服务器端）：GameMode::OnPlayerKilled 调用，Corpse = 死亡角色。
// 服务器 PC 也进入 Spectating 状态 + 下发 ClientEnterSpectator。
// 必须两侧状态一致 —— 引擎 ServerSetSpectatorLocation_Implementation（PlayerController.cpp:2871）在
// 服务器 PC 非 Spectating 时走 else 分支 ClientGotoState(GetStateName()) 强制客户端同步回服务器状态
// （Playing），观战 ~200ms 即被拉回（P1 实测发现的坑）。服务器进入 Spectating 后该检查通过，
// 观战持续到下轮重生（ClientRestart 自动退出）。
// ⚠ 不保留尸体 Possess（默认 ShouldKeepCurrentPawnUponSpectating=false）：进入观战即 UnPossess，
//   尸体 3s 后由 BlasterCharacter::DestroyCorpse 销毁 —— 若保留 Possess，尸体销毁会触发客户端
//   PawnPendingDestroy → ChangeState(NAME_Inactive)，把观战状态踢掉（实测发现的坑）。
void ABlasterPlayerController::EnterDeathSpectator(ABlasterCharacter* Corpse)
{
	if (!IsInState(NAME_Spectating))
	{
		ChangeState(NAME_Spectating);
	}
	ClientEnterSpectator(Corpse);
}

// 主流方案（致命修复）：AController::Destroyed（Controller.cpp:557-566）在 Logout 后紧接着调
// CleanupPlayerState → 默认 OnDeactivated → Destroy() 销毁 PS。但 Logout 已把 PS 注册进待重连表，
// 若被销毁则待重连表的 TObjectPtr 变悬垂 → 重连访问是未定义行为。
// 修复：若该 PS 在待重连表中，保留（PendingSessions 强引用防 GC），仅清本 PC 引用；否则引擎默认销毁。
void ABlasterPlayerController::CleanupPlayerState()
{
	UBlasterSessionManager* Mgr = UBlasterSessionManager::Get();
	if (Mgr && PlayerState)
	{
		for (auto& Pair : Mgr->GetPendingSessions())
		{
			if (Pair.Value.PlayerState.Get() == PlayerState)
			{
				PlayerState = NULL;
				return;
			}
		}
	}
	Super::CleanupPlayerState();
}

// 存活角色被断线销毁时（pawn 被连接清理销毁、仍被 Possess），捕获存活状态。
// 已死玩家的尸体在 DestroyCorpse 时已先 UnPossess（Controller=null），不走这里。
// GameMode::Logout 据此决定是否递减 AliveCount（存活断开=队伍减员；已死断开=死亡时已递减）。


//在角色的 Pawn 即将被销毁的瞬间，
//把"断开时他还活着吗"这个信息记到 PC 的成员变量里，供后面 Logout 使用。它不做事，只"捕获现场"。
void ABlasterPlayerController::PawnPendingDestroy(APawn* inPawn)
{
	if (ABlasterCharacter* Char = Cast<ABlasterCharacter>(inPawn))
	{
		bWasAliveAtDisconnect = !Char->IsElimmed();
	}
	Super::PawnPendingDestroy(inPawn);
}

// 中途加入观战（服务器端）：BombDefusalGameMode::HandleMidRoundJoin 调用。
// 与死亡观战（EnterDeathSpectator）区别：无死亡镜头、无团队锁定 —— 新加入者自由飞行看比赛。
// 服务器 PC 也进入 Spectating（P1 约束：两侧一致，否则 ServerSetSpectatorLocation → ClientGotoState 拉回）。
// 下轮重生时 OnPossess → ChangeState(NAME_Playing) 自动退出观战（P1 验证的统一路径）。
void ABlasterPlayerController::EnterJoinSpectator()
{
	if (!IsInState(NAME_Spectating))
	{
		ChangeState(NAME_Spectating);
	}
	ClientEnterJoinSpectator();
}

// 中途加入观战（客户端执行）：自由飞行 SpectatorPawn + 隐藏角色 HUD。
// 无死亡镜头（没死过）；SpectateTarget 保持 null → 自由飞行兜底。
// 下轮重生时 ClientRestart → ChangeState(NAME_Playing) 退出观战 + HUD 恢复（RoundInProgress 处理器）。
void ABlasterPlayerController::ClientEnterJoinSpectator_Implementation()
{
	if (IsInState(NAME_Spectating)) return;

	ChangeState(NAME_Spectating);
	bWasSpectating = true;
	if (BlasterHud)
	{
		BlasterHud->HideCharacterOverlay();
	}
	UE_LOG(LogTemp, Log, TEXT("[Join] 中途加入者进入观战（自由飞行）| PC=%s"), *GetName());
}

// ========================================================================
// P1 v2 观战：锁定存活同阵营队友视角
// ========================================================================

// 收集存活同阵营队友（客户端执行）：
// 遍历世界 BlasterCharacter，筛 !IsElimmed() && GetPlayerState<ABlasterPlayerState>()->TeamID == 我方 TeamID。
// ⚠ 不能用 Char->BlasterPlayerState —— 该项目该成员从未被赋值（恒为 null），实测导致永远找不到队友。
// 改用 APawn 自带的复制属性 PlayerState（Pawn.h:154 replicatedUsing=OnRep_PlayerState，客户端有值）。
// bElimmed / TeamID 均已复制，客户端可直接判断；自己的尸体已 elimmed 自然被排除。
TArray<ABlasterCharacter*> ABlasterPlayerController::CollectAliveTeammates() const
{
	TArray<ABlasterCharacter*> Result;
	const ABlasterPlayerState* MyPS = GetPlayerState<ABlasterPlayerState>();
	if (!MyPS || !GetWorld()) return Result;

	TArray<AActor*> FoundChars;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABlasterCharacter::StaticClass(), FoundChars);
	for (AActor* Actor : FoundChars)
	{
		ABlasterCharacter* Char = Cast<ABlasterCharacter>(Actor);
		if (!Char || Char->IsElimmed()) continue;
		const ABlasterPlayerState* CPS = Char->GetPlayerState<ABlasterPlayerState>();
		if (CPS && CPS->TeamID == MyPS->TeamID)
		{
			Result.Add(Char);
		}
	}
	return Result;
}

// 观战进入 / 目标死亡时调用（客户端执行）：
// ① 无存活队友 → SetViewTarget(SpectatorPawn) 自由飞行兜底，SpectateTarget=nullptr
// ② 当前目标仍存活 → 保持
// ③ 否则锁定第一个存活队友（SetViewTarget 客户端本地，观战期间服务器不推 ClientSetViewTarget，稳定）
void ABlasterPlayerController::UpdateSpectateTarget()
{
	if (!IsInState(NAME_Spectating)) return;

	const TArray<ABlasterCharacter*> AliveTeammates = CollectAliveTeammates();

	// 无存活队友 → 自由飞行兜底
	if (AliveTeammates.Num() == 0)
	{
		SpectateTarget = nullptr;
		if (GetSpectatorPawn())
		{
			SetViewTarget(GetSpectatorPawn());
		}
		UE_LOG(LogTemp, Log, TEXT("[Spectate] 无存活队友 → 自由飞行 | PC=%s"), *GetName());
		return;
	}

	// 当前目标仍存活 → 保持
	if (SpectateTarget.IsValid() && AliveTeammates.Contains(SpectateTarget.Get()))
	{
		return;
	}

	// 锁定第一个存活队友（方案2：锚定自由视角）。
	// 视角用观战者自己的 SpectatorPawn（自由相机，鼠标控角度），Tick 每帧把其位置锚定到队友 → 跟随但不锁视角。
	// 不用 SetViewTarget(队友)—— 模拟角色的相机角度在别的客户端不同步，会导致角度锁死（见 P1 讨论）。
	SpectateTarget = AliveTeammates[0];
	if (GetSpectatorPawn())
	{
		SetViewTarget(GetSpectatorPawn());
	}
	UE_LOG(LogTemp, Log, TEXT("[Spectate] 观战锁定队友 %s（锚定自由视角）| Team=%d | PC=%s"),
		*GetNameSafe(SpectateTarget.Get()),
		(int32)GetPlayerState<ABlasterPlayerState>()->TeamID,
		*GetName());
}

// 箭头键 ↑↓←→ → 切换下一个存活队友（客户端执行，循环）。
// 非观战状态或无存活队友（自由飞行中）时忽略。
void ABlasterPlayerController::CycleSpectateTarget()
{
	if (!IsInState(NAME_Spectating)) return;

	const TArray<ABlasterCharacter*> AliveTeammates = CollectAliveTeammates();
	if (AliveTeammates.Num() == 0) return;   // 无队友不切换（自由飞行）

	// 当前目标下标 → 下一个（循环）；目标不在列表（如刚死亡）→ 从 0 开始
	// 方案2：视角保持 SpectatorPawn 不变，切换只是改锚定目标（Tick 会锚定新队友位置）。
	const int32 NextIndex = (AliveTeammates.IndexOfByKey(SpectateTarget.Get()) + 1) % AliveTeammates.Num();
	SpectateTarget = AliveTeammates[NextIndex];
	if (GetSpectatorPawn())
	{
		SetViewTarget(GetSpectatorPawn());
	}
	UE_LOG(LogTemp, Log, TEXT("[Spectate] 切换到队友 %s（锚定自由视角）| PC=%s"),
		*GetNameSafe(SpectateTarget.Get()), *GetName());
}

// 当前观战目标是否仍存活（未销毁 && 未 elimmed）
bool ABlasterPlayerController::IsSpectateTargetAlive() const
{
	return SpectateTarget.IsValid() && !SpectateTarget->IsElimmed();
}

void ABlasterPlayerController::ClientReportServerTime_Implementation(float TimeOfClientRequest, float TimeServerReceivedClientRequest)
{
	float RoundTripTime = GetWorld()->GetTimeSeconds() - TimeOfClientRequest;
	SingleTripTime = 0.5f * RoundTripTime;
	float CurrentServerTime = TimeServerReceivedClientRequest + SingleTripTime;
	const float RawDelta = CurrentServerTime - GetWorld()->GetTimeSeconds();

	// ── 中位数滤波：抗主机延迟尖峰 ──
	// 单次采样会被"某次系统忙/网络抖动"的读数带偏；累积样本取中位更稳。
	// 窗口大小由 CVar blaster.TimeSync.MedianWindowSize 控制（默认 7）。
	DeltaSamples.Add(RawDelta);
	const int32 MaxSamples = CVarTimeSyncMedianWindow.GetValueOnGameThread();
	while (DeltaSamples.Num() > MaxSamples)
	{
		DeltaSamples.RemoveAt(0);  // 滚动窗口：只保留最近 MaxSamples 个
	}

	TArray<float> Sorted = DeltaSamples;
	Sorted.Sort();
	ClientServerDelta = Sorted[Sorted.Num() / 2];  // 取中位（抗尖峰，偏移估计更稳）
}

// ════════════════════════════════════════════════════════════════
// Phase 2 客户端插值平滑自适应：采样抖动 → 映射目标 τ → ramp 平滑 → 应用到远端角色
// 与 SSR 构成"补上行 + 补下行"的双向延迟补偿。SSR 回退服务器到射击时刻（补射击者上行），
// 本函数降低远端角色渲染位置的平滑滞后 τ（补下行）。
// ════════════════════════════════════════════════════════════════

TAutoConsoleVariable<int32> CVarBlasterNetSmoothAdaptiveEnabled(
	TEXT("blaster.NetSmooth.Adaptive.Enabled"),
	1,
	TEXT("插值平滑自适应总开关\n0=禁用（只保留 Phase 1 静态值） 1=启用"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetSmoothAdaptiveMinTau(
	TEXT("blaster.NetSmooth.Adaptive.MinTau"),
	0.05f,
	TEXT("τ 下限（秒）：低抖动时的平滑时间，与 Phase 1 静态默认一致"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetSmoothAdaptiveMaxTau(
	TEXT("blaster.NetSmooth.Adaptive.MaxTau"),
	0.1f,
	TEXT("τ 上限（秒）：高抖动时回落到引擎默认 0.1s 兜底"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetSmoothAdaptiveLowJitterMs(
	TEXT("blaster.NetSmooth.Adaptive.LowJitterMs"),
	30.f,
	TEXT("抖动低于此值（ms）→ τ=MinTau（最跟手）"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetSmoothAdaptiveHighJitterMs(
	TEXT("blaster.NetSmooth.Adaptive.HighJitterMs"),
	80.f,
	TEXT("抖动高于此值（ms）→ τ=MaxTau（兜底），中间线性插值"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetSmoothAdaptiveRampRate(
	TEXT("blaster.NetSmooth.Adaptive.RampRate"),
	0.01f,
	TEXT("τ 每秒最大逼近量（FInterpConstantTo 的 InterpSpeed，0.01=10ms/s）\n防 τ 阈值跳变制造二次抖动"),
	ECVF_Default
);

void ABlasterPlayerController::UpdateNetSmoothAdaptive(float DeltaTime)
{
	// 仅客户端本机执行：远端角色平滑只发生在本地渲染，服务器/其它客户端无需此逻辑
	if (!IsLocalController()) return;
	if (!CVarBlasterNetSmoothAdaptiveEnabled.GetValueOnGameThread()) return;

	// ~1s 采样一次延迟。抖动是快变量，但"调整 τ"的决策 1s 粒度足够；
	// 平滑值的应用本身是引擎逐帧自动做的，不需要逐帧改。
	NetSmoothLastSampleTime += DeltaTime;
	if (NetSmoothLastSampleTime < 1.0f) return;
	const float SampleDt = NetSmoothLastSampleTime; // 实际采样间隔（≈1s），供 ramp 使用
	NetSmoothLastSampleTime = 0.f;

	// 采样源：NetConnection 本地可得、采样可控。GetPingInMilliseconds 是复制值，
	// 更新频率由 PlayerState 复制决定、不可控，故不用。
	const UNetConnection* Conn = GetNetConnection();
	// AvgLag 是 UNetConnection 的 public 成员（单位秒），乘 1000 转毫秒与 CVar 阈值对齐
	const float LagMs = Conn ? Conn->AvgLag * 1000.f : 0.f;

	// 滚动窗口：保留最近 N=10 个样本（1s 采样 → 10s 窗口），沿用时间同步 RemoveAt(0) 惯例
	RecentLagSamples.Add(LagMs);
	const int32 MaxSamples = 10;
	while (RecentLagSamples.Num() > MaxSamples)
	{
		RecentLagSamples.RemoveAt(0);
	}

	// jitter = 相邻样本差的绝对值平均，对单次尖峰稳健（与项目"抗尖峰"一贯思路一致）
	float JitterMs = 0.f;
	if (RecentLagSamples.Num() >= 2)
	{
		float SumDiff = 0.f;
		for (int32 i = 1; i < RecentLagSamples.Num(); ++i)
		{
			SumDiff += FMath::Abs(RecentLagSamples[i] - RecentLagSamples[i - 1]);
		}
		JitterMs = SumDiff / (RecentLagSamples.Num() - 1);
	}

	// 映射目标 τ：低抖动 → MinTau（更跟手），高抖动 → MaxTau（兜底），中间线性
	const float LowMs = CVarBlasterNetSmoothAdaptiveLowJitterMs.GetValueOnGameThread();
	const float HighMs = CVarBlasterNetSmoothAdaptiveHighJitterMs.GetValueOnGameThread();
	const float MinTau = CVarBlasterNetSmoothAdaptiveMinTau.GetValueOnGameThread();
	const float MaxTau = CVarBlasterNetSmoothAdaptiveMaxTau.GetValueOnGameThread();
	const float Alpha = FMath::Clamp((JitterMs - LowMs) / (HighMs - LowMs), 0.f, 1.f);
	const float TargetTau = FMath::Lerp(MinTau, MaxTau, Alpha);

	// τ 平滑（ramp）：TargetTau 直接写入会因阈值跳变让 τ 瞬跳、所有敌人逼近速度集体突变
	// （"突然集体变卡"一下），制造新的抖。用匀速逼近让 τ 缓慢滑向目标。
	const float RampRate = CVarBlasterNetSmoothAdaptiveRampRate.GetValueOnGameThread();
	CurrentTau = FMath::FInterpConstantTo(CurrentTau, TargetTau, SampleDt, RampRate);

	// 应用：遍历所有远端角色（simulated proxy），把平滑后的 CurrentTau 写进其 CMC。
	// jitter 是连接级属性，所有 simulated proxy 共享同一个 τ，不做 per-actor 差异化。
	// NetworkSimulatedSmoothLocationTime 是基类 public 成员，用基类指针直接写即可。
	for (TActorIterator<ABlasterCharacter> It(GetWorld()); It; ++It)
	{
		ABlasterCharacter* Ch = *It;
		if (!Ch || Ch->GetLocalRole() != ROLE_SimulatedProxy) continue;
		if (UCharacterMovementComponent* CMC = Ch->GetCharacterMovement())
		{
			CMC->NetworkSimulatedSmoothLocationTime = CurrentTau;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[NetSmooth] jitter=%.1fms targetTau=%.3f currentTau=%.3f | samples=%d"),
		JitterMs, TargetTau, CurrentTau, RecentLagSamples.Num());
}

float ABlasterPlayerController::GetServerTime()
{
	if (HasAuthority()) return GetWorld()->GetTimeSeconds();
	else return GetWorld()->GetTimeSeconds() + ClientServerDelta;
}

void ABlasterPlayerController::ReceivedPlayer()
{
	Super::ReceivedPlayer();
	if (IsLocalController())
	{
		ServerRequestServerTime(GetWorld()->GetTimeSeconds());
	}
}

// ------------------------------------------------------------
// 比赛状态：服务端检测状态发给客户端，客户端同步 Warmup/Match/Cooldown 时间
// ------------------------------------------------------------
// ------------------------------------------------------------
// 比赛状态同步：客户端 BeginPlay 时通过 RPC 向服务器请求当前比赛状态，
// 服务器从 GameMode 读取 Warmup/Match/Cooldown 时长和当前 MatchState，
// 再通过 ClientJoinMidgame RPC 发回客户端，驱动 HUD 初始化
// ------------------------------------------------------------
void ABlasterPlayerController::ServerCheckMatchState_Implementation()//客户端向服务器请求比赛状态，服务器从 GameMode 读取配置发回客户端
{
	// 从 GameMode 获取比赛配置和当前状态（GameMode 仅存在于服务器）
	ABlasterGameMode* GameMode = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
	if (GameMode)
	{
		WarmupTime = GameMode->WarmupTime;           // 热身阶段时长（默认10秒）
		MatchTime = GameMode->MatchTime;             // 比赛阶段时长（默认120秒）
		CooldownTime = GameMode->CooldownTime;       // 冷却阶段时长（默认10秒）
		LevelStartingTime = GameMode->LevelStartingTime; // 关卡开始的时间戳，用于计算剩余倒计时
		MatchState = GameMode->GetMatchState();      // 当前比赛状态（WaitingToStart/InProgress/Cooldown）
		// 将状态和时间打包发回客户端，客户端据此决定显示 Announcement 还是 CharacterOverlay
		ClientJoinMidgame(MatchState, WarmupTime, MatchTime, CooldownTime, LevelStartingTime);
	}
}

void ABlasterPlayerController::ClientJoinMidgame_Implementation(FName StateOfMatch, float Warmup, float Match, float Cooldown, float StartingTime)
{
	WarmupTime = Warmup;
	MatchTime = Match;
	CooldownTime = Cooldown;
	LevelStartingTime = StartingTime;
	// 只在 MatchState 尚未初始化时才设置，防止用 RPC 中的过时状态
	// 覆盖已通过属性复制到达的更新状态（竞态条件修复）
	if (MatchState == NAME_None)
	{
		MatchState = StateOfMatch;
	}
	// 根据当前实际的 MatchState 初始化 UI（而非 RPC 参数中的可能过时状态）
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	// 热身阶段自动打开购买菜单（仅首次，CreateBuyMenu 内部有重复创建保护）
	if (BlasterHud && MatchState == MatchState::WaitingToStart && BlasterHud->BuyMenu == nullptr)
	{
		OpenBuyMenuOnWarmup();
	}

	// 炸弹模式中途加入：RPC 设置 MatchState 不会触发 OnRep_MatchState
	// （因为后续属性复制到达时值相同，OnRep 判定无变化而跳过），
	// 需手动调用 Handle 函数初始化 Announcement/RoundOverlay 的显隐和 Tick 路径
	if (BlasterHud)
	{
		if (MatchState == MatchState::AssignTeams)
		{
			HandleAssignTeams();
		}
		else if (MatchState == MatchState::RoundPrepare)
		{
			HandleRoundPrepare();
		}
		else if (MatchState == MatchState::RoundInProgress)
		{
			HandleRoundInProgress();
		}
		else if (MatchState == MatchState::RoundEnd)
		{
			HandleRoundEnd();
		}
		else if (MatchState == MatchState::HalftimeSwap)
		{
			HandleHalftimeSwap();
		}
		else if (MatchState == MatchState::MatchEnd)
		{
			HandleMatchEnd();
		}
	}
}

void ABlasterPlayerController::OnMatchStateSet(FName State, bool bTeamsMatch)//负责初始化玩家状态
{
	MatchState = State;

	if (MatchState == MatchState::InProgress)
	{
		HandleMatchHasStarted(bTeamsMatch);
	}
	else if (MatchState == MatchState::Cooldown)
	{
		HandleCooldown();
	}
	// 回合制阵营模式状态处理
	else if (MatchState == MatchState::AssignTeams)
	{
		HandleAssignTeams();
	}
	else if (MatchState == MatchState::RoundPrepare)
	{
		HandleRoundPrepare();
	}
	else if (MatchState == MatchState::RoundInProgress)
	{
		HandleRoundInProgress();
	}
	else if (MatchState == MatchState::RoundEnd)
	{
		HandleRoundEnd();
	}
		else if (MatchState == MatchState::HalftimeSwap)
		{
			HandleHalftimeSwap();
		}
		else if (MatchState == MatchState::MatchEnd)
		{
			HandleMatchEnd();
		}
}

void ABlasterPlayerController::OnRep_MatchState()//负责同步玩家状态，与OnMatchStateSet配合，一个负责初始化，一个负责后续同步
{
	if (MatchState == MatchState::InProgress)
	{
		HandleMatchHasStarted();
	}
	else if (MatchState == MatchState::Cooldown)
	{
		HandleCooldown();
	}
	else if (MatchState == MatchState::WaitingToStart)
	{
		// 复制路径的 WaitingToStart：确保公告面板在热身阶段被创建
		BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
		// 热身阶段自动打开购买菜单（仅首次）
		if (BlasterHud && BlasterHud->BuyMenu == nullptr)
		{
			OpenBuyMenuOnWarmup();
		}
	}
	// 回合制阵营模式状态处理（复制路径）
	else if (MatchState == MatchState::AssignTeams)
	{
		HandleAssignTeams();
	}
	else if (MatchState == MatchState::RoundPrepare)
	{
		HandleRoundPrepare();
	}
	else if (MatchState == MatchState::RoundInProgress)
	{
		HandleRoundInProgress();
	}
	else if (MatchState == MatchState::RoundEnd)
	{
		HandleRoundEnd();
	}
	else if (MatchState == MatchState::MatchEnd)
	{
		HandleMatchEnd();
	}
}

void ABlasterPlayerController::HandleMatchHasStarted(bool bTeamsMatch)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud)
	{
		BlasterHud->ShowCharacterOverlay(); // 显示战斗 UI
		BlasterHud->Announcement->SetVisibility(ESlateVisibility::Hidden);
		// 比赛开始，关闭购买菜单（此后 B 键不再生效，ToggleBuyMenu 检查 MatchState）
		if (bBuyMenuOpen)
		{
			HideBuyMenu();
		}
	}
}

void ABlasterPlayerController::HandleCooldown()
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud)
	{
		// 比赛结束，移除战斗 HUD（如果存在），显示公告面板
		if (BlasterHud->CharacterOverlay)
		{
			BlasterHud->HideCharacterOverlay();
		}
		bool bHUDValid = BlasterHud->Announcement &&
			BlasterHud->Announcement->AnnouncementText &&
			BlasterHud->Announcement->InfoText;

		if (bHUDValid)
		{
			BlasterHud->Announcement->SetVisibility(ESlateVisibility::Visible);
			FString AnnouncementText = Announcement::NewMatchStartsIn;
			BlasterHud->Announcement->AnnouncementText->SetText(FText::FromString(AnnouncementText));

			// 从 GameState 读取服务器维护的 TopScoringPlayers，确保所有客户端显示一致的胜者
			ABlasterGameState* BlasterGameState = GetWorld()->GetGameState<ABlasterGameState>();
			if (BlasterGameState && BlasterGameState->TopScoringPlayers.Num() > 0)
			{
				FString InfoTextString = GetInfoText(BlasterGameState->TopScoringPlayers);
				BlasterHud->Announcement->InfoText->SetText(FText::FromString(InfoTextString));
			}
		}
	}
}

// ------------------------------------------------------------
// 购买菜单生命周期：热身自动打开，B 键切换开关，比赛开始强制关闭
// ------------------------------------------------------------
void ABlasterPlayerController::OpenBuyMenuOnWarmup()
{
	ShowBuyMenu();
}

void ABlasterPlayerController::ToggleBuyMenu()
{
	// 热身阶段和购买阶段允许开关购买菜单，比赛开始后 B 键无效果
	if (MatchState != MatchState::WaitingToStart && MatchState != MatchState::RoundPrepare) return;

	if (bBuyMenuOpen)
	{
		HideBuyMenu();
	}
	else
	{
		ShowBuyMenu();
	}
}

void ABlasterPlayerController::ShowBuyMenu()
{
	if (bBuyMenuOpen) return;

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud == nullptr) return;

	// Widget 已在 InitializeHUD 中预创建
	BlasterHud->ShowBuyMenu();

	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	SetShowMouseCursor(true);

	bBuyMenuOpen = true;
}

void ABlasterPlayerController::HideBuyMenu()
{
	if (!bBuyMenuOpen) return;

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud && BlasterHud->BuyMenu)
	{
		BlasterHud->HideBuyMenu();
	}

	FInputModeGameOnly InputMode;
	SetInputMode(InputMode);
	SetShowMouseCursor(false);

	bBuyMenuOpen = false;
}

// ------------------------------------------------------------
// 投掷物选择面板生命周期：按住 G 显示，点击图标确认选择，松开 G 关闭（取消）
// ------------------------------------------------------------
void ABlasterPlayerController::ShowThrowablePanel()
{
	// 仅在 RoundInProgress 阶段允许使用投掷物
	if (MatchState != MatchState::RoundInProgress) return;

	// Toggle：面板已打开则关闭（取消选择），未打开则打开
	if (bThrowablePanelOpen)
	{
		HideThrowablePanel();
		return;
	}

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud == nullptr) return;

	// Widget 已在 InitializeHUD 中预创建

	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(GetPawn());
	if (!BlasterCharacter) return;

	UThrowableComponent* ThrowableComp = BlasterCharacter->GetThrowable();
	if (!ThrowableComp) return;

	// 绑定点击委托：点击按钮 → OnThrowableTypeClicked → 选择类型 + 关闭面板
	BlasterHud->ThrowableWheel->OnTypeClicked.AddDynamic(this, &ABlasterPlayerController::OnThrowableTypeClicked);

	BlasterHud->ShowThrowableWheel();
	BlasterHud->ThrowableWheel->Show(ThrowableComp);

	// 显示鼠标用于点击选择
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	SetShowMouseCursor(true);

	bThrowablePanelOpen = true;
}

void ABlasterPlayerController::HideThrowablePanel()
{
	if (!bThrowablePanelOpen) return;

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud == nullptr) return;

	// 解除委托绑定，避免悬空引用
	BlasterHud->ThrowableWheel->OnTypeClicked.RemoveDynamic(this, &ABlasterPlayerController::OnThrowableTypeClicked);

	BlasterHud->ThrowableWheel->Hide();
	BlasterHud->HideThrowableWheel();

	// 恢复纯游戏输入
	FInputModeGameOnly InputMode;
	SetInputMode(InputMode);
	SetShowMouseCursor(false);

	bThrowablePanelOpen = false;
}

void ABlasterPlayerController::OnThrowableTypeClicked(EThrowableType Type)
{
	// 点击即确认：通知角色切换投掷物类型，然后关闭面板
	ABlasterCharacter* BlasterCharacter = Cast<ABlasterCharacter>(GetPawn());
	if (BlasterCharacter)
	{
		BlasterCharacter->SelectThrowableType(Type);
	}

	HideThrowablePanel();
}

void ABlasterPlayerController::SetHUDThrowableCooking(bool bIsCooking, float RemainingSeconds)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	bool bHUDValid = BlasterHud && BlasterHud->CharacterOverlay
		&& BlasterHud->CharacterOverlay->ThrowableCountdownText;

	if (bHUDValid)
	{
		if (bIsCooking && RemainingSeconds > 0.f)
		{
			FString CountdownText = FString::Printf(TEXT("%.1f"), RemainingSeconds);
			BlasterHud->CharacterOverlay->ThrowableCountdownText->SetText(FText::FromString(CountdownText));
			BlasterHud->CharacterOverlay->ThrowableCountdownText->SetVisibility(ESlateVisibility::Visible);
		}
		else
		{
			BlasterHud->CharacterOverlay->ThrowableCountdownText->SetVisibility(ESlateVisibility::Hidden);
		}
	}
}

// ========================================================================
// 闪光弹致盲 Client RPC + 白屏淡出
// ========================================================================

void ABlasterPlayerController::ClientApplyFlashEffect_Implementation(float Duration)
{
	if (ABlasterHud* BHud = Cast<ABlasterHud>(GetHUD()))
	{
		BHud->ShowFlashEffect(Duration);
	}
}

FString ABlasterPlayerController::GetInfoText(const TArray<class ABlasterPlayerState*>& Players)
{
	ABlasterPlayerState* BlasterPlayerState = GetPlayerState<ABlasterPlayerState>();
	if (BlasterPlayerState == nullptr) return FString();
	FString InfoTextString;
	if (Players.Num() == 1 && Players[0] == BlasterPlayerState)
	{
		InfoTextString = TEXT("你是冠军!");
	}
	else if (Players.Num() == 1)
	{
		InfoTextString = FString::Printf(TEXT("胜者: \n%s"), *Players[0]->GetPlayerName());
	}
	else if (Players.Num() > 1)
	{
		InfoTextString = TEXT("并列胜者:\n");
		for (auto TiedPlayer : Players)
		{
			InfoTextString.Append(FString::Printf(TEXT("%s\n"), *TiedPlayer->GetPlayerName()));
		}
	}
	return InfoTextString;
}

// ------------------------------------------------------------
// 每帧驱动 HUD 倒计时：计算剩余秒数 → 变化时更新对应 UI 控件
// 服务器和客户端计算逻辑不同：
//   服务器：直接读 GameMode->GetCountdownTime()（权威数据）
//   客户端：用 GetServerTime() + 偏移公式推算出接近服务器的时间
// ------------------------------------------------------------
void ABlasterPlayerController::SetHUDTime()
{
	float TimeLeft = 0.f;

	// 回合制倒计时阶段：从 GameState 读取 RemainingCountdown（服务器/客户端均可用）
	if (MatchState == MatchState::RoundPrepare ||
	    MatchState == MatchState::RoundEnd ||
	    MatchState == MatchState::MatchEnd)
	{
		if (ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>())
		{
			TimeLeft = GS->RemainingCountdown;
		}
	}
	// 回合战斗倒计时：从 GameState 读取，推送到 MatchCountdownText
	else if (MatchState == MatchState::RoundInProgress)
	{
		if (ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>())
		{
			TimeLeft = GS->RemainingCountdown;
		}
	}
	// AssignTeams 是瞬间过渡状态，无倒计时
	else if (MatchState == MatchState::AssignTeams)
	{
	}
	else
	{
		// 原有 Deathmatch 倒计时逻辑（WaitingToStart / InProgress / Cooldown）
		// 仅当 GameMode 是 ABlasterGameMode 时生效；爆破模式下这些状态不适用
		const bool bIsBombDefusal = Cast<ABombDefusalGameMode>(UGameplayStatics::GetGameMode(this)) != nullptr;
		if (!bIsBombDefusal)
		{
			if (MatchState == MatchState::WaitingToStart)
				TimeLeft = WarmupTime - GetServerTime() + LevelStartingTime;
			else if (MatchState == MatchState::InProgress)
				TimeLeft = WarmupTime + MatchTime - GetServerTime() + LevelStartingTime;
			else if (MatchState == MatchState::Cooldown)
				TimeLeft = CooldownTime + WarmupTime + MatchTime - GetServerTime() + LevelStartingTime;

			// 服务器端：直接使用 GameMode 里计算好的权威倒计时，保证精确
			if (HasAuthority())
			{
				if (BlasterGameMode == nullptr)
				{
					BlasterGameMode = Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this));
					if (BlasterGameMode)
					{
						LevelStartingTime = BlasterGameMode->LevelStartingTime;
					}
				}
				BlasterGameMode = BlasterGameMode == nullptr
					? Cast<ABlasterGameMode>(UGameplayStatics::GetGameMode(this))
					: BlasterGameMode;
				if (BlasterGameMode)
				{
					TimeLeft = BlasterGameMode->GetCountdownTime();
				}
			}
		}
	}

	uint32 SecondsLeft = FMath::CeilToInt(TimeLeft);

	// 仅在秒数变化时才更新 UI，避免每帧做无效的字符串格式化
	if (CountdownInt != SecondsLeft)
	{
		// 热身和冷却 → 更新公告面板倒计时
		if (MatchState == MatchState::WaitingToStart || MatchState == MatchState::Cooldown)
		{
			SetHUDAnnouncementCountdown(TimeLeft);
		}
		// 比赛中 → 更新战斗 HUD 倒计时（Deathmatch InProgress + 爆破 RoundInProgress）
		if (MatchState == MatchState::InProgress || MatchState == MatchState::RoundInProgress)
		{
			SetHUDMatchCountdown(TimeLeft);
		}
		// 回合制倒计时：所有准备/结束阶段均显示在公告面板上
		if (MatchState == MatchState::RoundPrepare
			|| MatchState == MatchState::RoundEnd
			|| MatchState == MatchState::MatchEnd)
		{
			SetHUDAnnouncementCountdown(TimeLeft);
		}
	}

	CountdownInt = SecondsLeft;
}

void ABlasterPlayerController::PollInit()//推送缓存数据
{
	EnsureBlasterHud(); // 每帧兜底：直到 BlasterHud 生成成功为止
	if (CharacterOverlay == nullptr)
	{
		if (BlasterHud && BlasterHud->CharacterOverlay)
		{
			CharacterOverlay = BlasterHud->CharacterOverlay;
			if (CharacterOverlay)
			{
				if (bInitializeHealth) SetHUDHealth(HUDHealth, HUDMaxHealth);
				if (bInitializeShield) SetHUDShield(HUDShield, HUDMaxShield);
				if (bInitializeMatchCountdown) SetHUDMatchCountdown(HUDMatchCountdown);
				if (bInitializeCarriedAmmo) SetHUDCarriedAmmo(HUDCarriedAmmo);
				if (bInitializeWeaponAmmo) SetHUDWeaponAmmo(HUDWeaponAmmo);
			}
		}
	}
}

// ========================================================================
// 回合制阵营模式：MatchState 处理器
// ========================================================================
void ABlasterPlayerController::HandleAssignTeams()
{
	// Announcement 文本由 GameState 委托自动填充，此处只管理可见性
	// 不检查 PlayerState：客户端 PS 复制可能晚于 MatchState，可见性不依赖 PS
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud)
	{
		BlasterHud->EnsureAnnouncement();
		if (BlasterHud->Announcement)
		{
			BlasterHud->Announcement->SetVisibility(ESlateVisibility::Visible);
		}
	}
}

void ABlasterPlayerController::HandleRoundPrepare()
{
	// 先确保公告面板可见（不依赖 PlayerState 是否已复制到客户端）
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud)
	{
		BlasterHud->HideCharacterOverlay();
		BlasterHud->HideRoundOverlay();
		BlasterHud->EnsureAnnouncement();
		if (BlasterHud->Announcement)
		{
			BlasterHud->Announcement->SetVisibility(ESlateVisibility::Visible);
		}
	}

	// Announcement 文本由 GameState 委托自动填充，此处只管理可见性

	// 推送回合信息到 RoundOverlay（此时隐藏中，切换到 RoundInProgress 时显示）
	// RoundOverlay 委托已处理回合信息

	// 客户端：标记需要下一帧用最新 GameState 数据刷新公告（GS 复制可能滞后于 MatchState）
}

void ABlasterPlayerController::HandleRoundInProgress()
{
	// 购买阶段结束，强制关闭购买菜单（防止玩家卡在菜单里进入战斗）
	if (bBuyMenuOpen)
	{
		HideBuyMenu();
	}

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud)
	{
		// Widget 已在 InitializeHUD 中预创建，直接 Show/Hide
		BlasterHud->ShowCharacterOverlay();
		BlasterHud->ShowRoundOverlay();

		// 数据已由 RoundOverlay 委托绑定 GameState 自动更新，不再需要 PC 搬运

		BlasterHud->Announcement->SetVisibility(ESlateVisibility::Hidden);
	}
}

void ABlasterPlayerController::HandleRoundEnd()
{
	// 从 GameState 读取（仅做显隐管理，文本由委托处理）
	ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>();
	if (!GS) return;

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;

	if (BlasterHud && BlasterHud->CharacterOverlay)
	{
		BlasterHud->HideCharacterOverlay();
	}
	if (BlasterHud && BlasterHud->RoundOverlay)
	{
		BlasterHud->HideRoundOverlay();
	}

	// Announcement 文本由 GameState 委托自动填充，此处只管理可见性
	if (BlasterHud)
	{
		BlasterHud->EnsureAnnouncement();
		if (BlasterHud->Announcement)
		{
			BlasterHud->Announcement->SetVisibility(ESlateVisibility::Visible);
		}
	}

	// 推送回合结果到 RoundOverlay
	// RoundOverlay 委托已处理回合结果
	// 数据已由 RoundOverlay 委托处理

	// 客户端：标记需要下一帧用最新 GameState 数据刷新公告
}

void ABlasterPlayerController::HandleMatchEnd()
{
	// 从 GameState 读取（仅做显隐管理，文本由委托处理）
	ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>();
	if (!GS) return;

	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;

	if (BlasterHud && BlasterHud->CharacterOverlay)
	{
		BlasterHud->HideCharacterOverlay();
	}
	if (BlasterHud && BlasterHud->RoundOverlay)
	{
		BlasterHud->HideRoundOverlay();
	}

	// Announcement 文本由 GameState 委托自动填充，此处只管理可见性
	if (BlasterHud)
	{
		BlasterHud->EnsureAnnouncement();
		if (BlasterHud->Announcement)
		{
			BlasterHud->Announcement->SetVisibility(ESlateVisibility::Visible);
		}
	}

	// 推送比赛结果到 RoundOverlay
	// RoundOverlay 委托已处理比赛结果

	// 客户端：标记需要下一帧用最新 GameState 数据刷新公告
}

void ABlasterPlayerController::HandleHalftimeSwap()
{
	// 显示半场交换提示：隐藏 RoundOverlay/CharacterOverlay，显示 Announcement
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (BlasterHud)
	{
		BlasterHud->HideRoundOverlay();
		BlasterHud->HideCharacterOverlay();
		BlasterHud->EnsureAnnouncement();
		if (BlasterHud->Announcement)
		{
			BlasterHud->Announcement->SetVisibility(ESlateVisibility::Visible);
		}
	}
	// 蓝图侧通过 GameState::bIsSecondHalf 判断显示"上半场结束"或"下半场开始"文本
}

// ── 购买请求 RPC 验证：仅检查 ItemID 格式 ──
bool ABlasterPlayerController::ServerRequestPurchase_Validate(int32 ItemID)
{
	return ItemID > 0;  // 只校验基本格式，不校验是否存在（服务端 Implementation 做）
}

// ── 购买请求 RPC 实现：查表定价 + 扣款 + 分发 ──
void ABlasterPlayerController::ServerRequestPurchase_Implementation(int32 ItemID)
{
	ABlasterPlayerState* PS = GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// [1] MatchState 校验：仅在 RoundPrepare 阶段允许购买
	if (MatchState != MatchState::RoundPrepare) return;

	// [2] 阵营校验：未分配阵营拒绝
	if (PS->TeamID == ETeamID::ETI_None) return;

	// [3] 存活校验：已死亡玩家不能购买
	APawn* MyPawn = GetPawn();
	if (!MyPawn) return;

	// [4] 查表：从 GameState 获取 DataTable，按 ItemID 查找物品行
	ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>();
	if (!GS) return;

	const FShopItemRow* ItemRow = GS->FindShopItem(ItemID);
	if (!ItemRow)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BuyMenu] Invalid ItemID=%d from %s"),
			ItemID, *GetName());
		return;
	}

	// [5] 金额校验：使用 DataTable 中的 Price（不信任客户端）
	if (PS->Money < ItemRow->Price) return;

	// [6] 扣款（AddMoney 内部 Broadcast OnMoneyChanged -> BuyMenu 刷新）
	PS->AddMoney(-ItemRow->Price);

	// [7] 物品分发：委托 GameMode 按 Category 处理（Step 6 实现 ProcessPurchase）
	ABombDefusalGameMode* GameMode = GetWorld()->GetAuthGameMode<ABombDefusalGameMode>();
	if (GameMode)
	{
		GameMode->ProcessPurchase(this, *ItemRow);
	}

	UE_LOG(LogTemp, Log, TEXT("[BuyMenu] %s purchased ItemID=%d (%s) for $%d, remaining $%d"),
		*GetName(), ItemID, *ItemRow->DisplayName.ToString(), ItemRow->Price, PS->Money);
}

// ========================================================================
// 回合信息 HUD 推送 → RoundOverlay Widget
// ========================================================================




// ========================================================================
// 客户端 GameState 复制延迟补偿：HandleXxx 中 GS 数据可能尚未到达，
// PollInit 下一帧调用此函数用最新 GS 数据刷新公告文本
// ========================================================================

// ========================================================================
// 炸弹 UI 推送（BombMode Phase 4）
// 这些函数由 BombInteractionComponent / GameMode 调用，将数据推送到 HUD Widget
// ========================================================================

void ABlasterPlayerController::UpdateBombStatusUI(float RemainingTime, float TotalTime,
	const FString& StatusText, const FString& SiteName)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (!BlasterHud) return;

	UBombStatusWidget* Widget = BlasterHud->GetBombStatusWidget();
	if (!Widget) return;

	Widget->UpdateTimer(RemainingTime, TotalTime);
	Widget->UpdateStatusText(StatusText);
	Widget->UpdateSiteName(SiteName);
	Widget->SetBombUIVisible(true);
}

void ABlasterPlayerController::UpdateBombInteractUI(float Progress, const FString& PromptText, bool bVisible, bool bShowProgress)
{
	BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
	if (!BlasterHud) return;

	UBombInteractWidget* Widget = BlasterHud->GetBombInteractWidget();
	if (!Widget) return;

	// 进度条仅按住 Q 键时显示（bShowProgress），靠近点位时隐藏
	if (bShowProgress)
	{
		Widget->UpdateProgress(Progress);
		Widget->SetProgressBarVisible(true);
	}
	else
	{
		Widget->SetProgressBarVisible(false);
	}
	Widget->UpdatePromptText(PromptText);
	Widget->SetInteractVisible(bVisible);
}

void ABlasterPlayerController::ShowBombPlantedAnnouncement(const FString& SiteName)
{
	FString Msg = FString::Printf(TEXT("炸弹已在 %s 点安放！"), *SiteName);
	SetHUDMismatchNotification(Msg);
}

// 每帧 Tick 调用：查找世界中已安放的炸弹 → 推送 StatusWidget
void ABlasterPlayerController::UpdateBombStatusFromWorld()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// 查找 Planted 状态的炸弹
	ABombActor* PlantedBomb = nullptr;
	TArray<AActor*> FoundBombs;
	UGameplayStatics::GetAllActorsOfClass(World, ABombActor::StaticClass(), FoundBombs);
	for (AActor* Actor : FoundBombs)
	{
		ABombActor* Bomb = Cast<ABombActor>(Actor);
		if (Bomb && Bomb->GetBombState() == EBombState::EBS_Planted)
		{
			PlantedBomb = Bomb;
			break;
		}
	}

	if (PlantedBomb)
	{
		ABombSite* Site = PlantedBomb->GetPlantedSite();
		FString SiteName = Site ? Site->SiteName : TEXT("?");
		UpdateBombStatusUI(
			PlantedBomb->GetRemainingTime(),
			PlantedBomb->BombCountdown,
			FString::Printf(TEXT("炸弹已在 %s 点安放"), *SiteName),
			SiteName);
	}
	else
	{
		// 没有炸弹 → 隐藏 StatusWidget
		BlasterHud = BlasterHud == nullptr ? Cast<ABlasterHud>(GetHUD()) : BlasterHud;
		if (BlasterHud && BlasterHud->GetBombStatusWidget())
		{
			BlasterHud->GetBombStatusWidget()->SetBombUIVisible(false);
		}
	}
}
