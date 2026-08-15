#include "BombDefusalGameMode.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/BlasterTypes/MatchState.h"    // 独立 MatchState 常量（与 BlasterGameMode 解耦）
#include "Blaster/BlasterTypes/EconomyTypes.h" // ELogicalTeam 枚举 — AssignTeamsOnce/PostLogin 直接使用
#include "GameFramework/PlayerState.h"
#include "GameFramework/GameState.h"
#include "GameFramework/HUD.h"   // InitGame 里打印 HUDClass 诊断需要完整 AHUD 类型
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpectatorPawn.h"   // P1 观战：SpectatorClass 需要完整类型（StaticClass）
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Engine/Engine.h"
#include "Blaster/BlasterTypes/ShopTypes.h"
#include "Blaster/BlasterComponents/CombatComponent.h"
#include "Blaster/BlasterComponents/ThrowableComponent.h"
#include "Blaster/BlasterComponents/BuffComponent.h"
#include "Blaster/PlayerStart/TeamPlayerStart.h"
#include "Blaster/BombMode/BombActor.h"    // 炸弹实体（BombMode Phase 3）
#include "Blaster/BombMode/BombSite.h"     // 埋包点（BombMode Phase 3）
#include "Blaster/Persistence/BlasterPersistenceSubsystem.h"  // P4：比赛结算 → 异步入队写 SQLite
#include "Blaster/Session/SessionManagerSubsystem.h"  // P2：中途加入直连 Bomb 幂等补发会话 token
#include "EngineUtils.h"  // TActorIterator

// ────────────────────────────────────────────────────────────
// 服务器下行延迟模拟 CVar（测试钩子，Phase 2 验证用）
// 服务器黑窗敲不了命令（FWindowsConsoleOutputDevice 无输入读取），只能靠代码钩子自动注入。
// PktLag 抬高下行延迟水平，PktLagVariance 制造到达间隔抖动——固定 PktLag 只是"平移"、
// 不产生抖动，只有 Variance 才让下行快照"忽长忽短"。
// 注入点在本类 BeginPlay（时序原因见 BeginPlay 内注释）。
// ────────────────────────────────────────────────────────────
TAutoConsoleVariable<int32> CVarBlasterNetLagSimEnabled(
	TEXT("blaster.NetLagSim.Enabled"),
	0,
	TEXT("服务器下行延迟模拟总开关\n0=关 1=开"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetLagSimPktLag(
	TEXT("blaster.NetLagSim.PktLag"),
	150.f,
	TEXT("下行延迟水平（ms）"),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarBlasterNetLagSimVariance(
	TEXT("blaster.NetLagSim.Variance"),
	30.f,
	TEXT("下行延迟抖动方差（ms）—— 这才是 τ 该响应的抖动"),
	ECVF_Default
);

ABombDefusalGameMode::ABombDefusalGameMode()
{
	// 延迟开局：手动控制角色生成和状态机启动时机
	bDelayedStart = true;
	// bUseSeamlessTravel 由 InitGame 按 WorldType 条件设置
	// 必须设 PlayerStateClass，否则 GetActivePlayers() 里 Cast<ABlasterPlayerState> 失败
	PlayerStateClass = ABlasterPlayerState::StaticClass();
	// 必须显式设 GameStateClass：确保客户端创建的 GameState 代理是 ABlasterGameState 类型，
	// 否则 GetGameState<ABlasterGameState>() 返回 nullptr，所有委托绑定和 OnRep 回调静默失效
	GameStateClass = ABlasterGameState::StaticClass();
	// 无缝切图：新 PC 按本类生成（旧 PC 若为其子类则保留原类）。必须有，否则玩家没有 Blaster 输入/HUD
	PlayerControllerClass = ABlasterPlayerController::StaticClass();

	// 观战（P1）：死亡玩家用引擎默认观战 Pawn（ASpectatorPawn:ADefaultPawn 自带自由飞行相机）。
	// 引擎 InitGameState 会把它同步到 GameState->SpectatorClass（复制属性），
	// 客户端 SpawnSpectatorPawn 据此生成本地 SpectatorPawn（仅本机，SetReplicates(false)）。
	SpectatorClass = ASpectatorPawn::StaticClass();

	UE_LOG(LogTemp, Log, TEXT("[BombDefusalGameMode] Constructor — CDO created, bUseSeamlessTravel=%d, AimPeople=%d"),
		bUseSeamlessTravel, AimPeople);
}

void ABombDefusalGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	const EWorldType::Type WT = GetWorld() ? GetWorld()->WorldType : EWorldType::None;
	const ENetMode NM = GetNetMode();

	// PIE 单进程下引擎禁止无缝切换（见 AGameModeBase::CanServerTravel），
	// 仅在非 PIE（打包/独立服务器）下启用；PIE 时显式关闭以防蓝图子类覆盖
	if (GetWorld() && GetWorld()->WorldType != EWorldType::PIE)
	{
		bUseSeamlessTravel = true;
	}
	else
	{
		bUseSeamlessTravel = false;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[BombDefusalGameMode] InitGame | Map=%s | WorldType=%d | NetMode=%d | bUseSeamlessTravel=%d | Options=%s"),
		*MapName, (int32)WT, (int32)NM, bUseSeamlessTravel, *Options);

	// 诊断：确认 GameMode 的 HUDClass（服务器通过 ClientSetHUD 传给客户端的 HUD 类）
	UE_LOG(LogTemp, Warning,
		TEXT("[BombDefusalGameMode] InitGame → HUDClass=%s"),
		HUDClass ? *HUDClass->GetName() : TEXT("NULL"));
}

void ABombDefusalGameMode::BeginPlay()
{
	Super::BeginPlay();

	// ── 服务器下行延迟模拟注入（测试钩子，Phase 2 验证用）──
	// WHY 放在 Bomb GameMode 而不是 Lobby：-ExecCmds 在引擎首帧 Tick 的 DeferredCommands 里执行，
	// 实测晚于 LobbyGameMode::BeginPlay（02.54.30:706 > :621），Lobby 注入时 CVar 还是默认 0 不生效。
	// Bomb 是无缝切图后（玩家齐）才加载的地图，BeginPlay 必然晚于 ExecCmds，CVar 已被设好。
	// HOW：GEngine->Exec 把 "Net PktLag=..." 路由到 World 的 NetDriver（UNetDriver::Exec 处理，
	// 成功时服务器日志会打 "PktLag set to %d"），PktLag 抬高下行延迟、PktLagVariance 制造到达间隔抖动。
	if (GetNetMode() == NM_DedicatedServer && CVarBlasterNetLagSimEnabled.GetValueOnGameThread())
	{
		const int32 LagMs = (int32)CVarBlasterNetLagSimPktLag.GetValueOnGameThread();
		const int32 VarMs = (int32)CVarBlasterNetLagSimVariance.GetValueOnGameThread();
		GEngine->Exec(GetWorld(), *FString::Printf(TEXT("Net PktLag=%d"), LagMs));
		GEngine->Exec(GetWorld(), *FString::Printf(TEXT("Net PktLagVariance=%d"), VarMs));
		UE_LOG(LogTemp, Log, TEXT("[NetLagSim] 服务器下行延迟模拟已注入：PktLag=%dms Variance=%dms"), LagMs, VarMs);
	}

	// 缓存 GameState 引用并同步阶段时长配置（这些值后续不再变化，但客户端需要知道）
	BlasterGameState = GetGameState<ABlasterGameState>();
	if (BlasterGameState)
	{
		BlasterGameState->RoundPrepareDuration = RoundPrepareTime;
		BlasterGameState->RoundEndDuration = RoundEndTime;
		BlasterGameState->MatchEndDuration = MatchEndTime;
	}

	// ===== ECONOMY CONFIG =====
	// 加载经济配置 DataAsset → 写入 GameState（Phase 5 迁移）
	if (HasAuthority() && !EconomyConfigRef.IsNull())
	{
		UEconomyConfig* Config = EconomyConfigRef.LoadSynchronous();
		if (BlasterGameState)
		{
			BlasterGameState->EconomyConfig = Config;
			BlasterGameState->HalftimeRound = HalftimeRound;
		}
	}

	// ===== SHOP DATA TABLE =====
	// 加载商店物品 DataTable → 写入 GameState（服务端查表定价用）
	if (HasAuthority() && !ShopItemTableRef.IsNull())
	{
		UDataTable* LoadedTable = ShopItemTableRef.LoadSynchronous();
		if (BlasterGameState)
		{
			BlasterGameState->ShopItemTable = LoadedTable;
			// DS 价格同步：提取 ID+Price 填充复制数组
			BlasterGameState->SyncShopPrices();
			UE_LOG(LogTemp, Log, TEXT("[Shop] DT_ShopItems loaded: %d rows"),
				LoadedTable ? LoadedTable->GetRowMap().Num() : 0);
		}
	}
}

// ========================================================================
// Tick 驱动状态机
// WaitingToStart → AssignTeams(瞬间) → RoundPrepare → RoundInProgress → RoundEnd → ...
// ========================================================================
void ABombDefusalGameMode::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// ── 服务器性能采样（简历数据钩子，临时）：每 5s 汇总窗口内平均帧耗时 ──
	// WHY：DS 无渲染 HUD 且黑窗敲不了命令，帧耗时只能靠代码钩子落日志；
	// 为"DS 服务器性能"条目提供可量化数据（平均帧耗时 / 实际 tick 率），配合客户端 [FPS] 日志对照。
	if (GetNetMode() == NM_DedicatedServer)
	{
		PerfSampleAccum += DeltaTime;
		++PerfSampleCount;
		if (PerfSampleAccum >= 5.0f)
		{
			UE_LOG(LogTemp, Log, TEXT("[Perf] 服务器 tick 窗口: 平均帧耗时=%.2fms (实际 %.1fHz) | 样本=%d"),
				PerfSampleAccum / PerfSampleCount * 1000.f,
				PerfSampleCount / PerfSampleAccum,
				PerfSampleCount);
			PerfSampleAccum = 0.f;
			PerfSampleCount = 0;
		}
	}

	if (MatchState == MatchState::WaitingToStart)
	{
		// 人数达标 → 一次性分配阵营 → 短暂 AssignTeams 状态让客户端显示阵营提示
		if (GetActivePlayers().Num() >= AimPeople)
		{
			AssignTeamsOnce();
			SetMatchState(MatchState::AssignTeams);
		}
	}
	else if (MatchState == MatchState::AssignTeams)
	{
		// 阵营提示展示一帧后立即进入回合准备（无倒计时，瞬间过渡）
		StartRoundPrepare();
	}
	else if (MatchState == MatchState::RoundPrepare)
	{
		CountdownTime -= DeltaTime;
		if (BlasterGameState) BlasterGameState->RemainingCountdown = CountdownTime;
		if (CountdownTime <= 0.f)
		{
			StartRoundInProgress();
		}
	}
	else if (MatchState == MatchState::RoundInProgress)
	{
		// 回合计时器倒计时 + 存活计数由 OnPlayerKilled 事件驱动
		CountdownTime -= DeltaTime;
		if (BlasterGameState) BlasterGameState->RemainingCountdown = CountdownTime;
		// 超时：攻击方未能全灭/下包 → 保卫者获胜
		if (CountdownTime <= 0.f)
		{
			EndRound(ETeamID::ETI_Defender);
		}
		CheckRoundEnd();
	}
	else if (MatchState == MatchState::RoundEnd)
	{
		CountdownTime -= DeltaTime;
		if (BlasterGameState) BlasterGameState->RemainingCountdown = CountdownTime;
		if (CountdownTime <= 0.f)
		{
			CheckMatchEnd(); // 内部判断是继续下一回合还是结束比赛
		}
	}
	else if (MatchState == MatchState::HalftimeSwap)
	{
		// 半场交换展示倒计时
		CountdownTime -= DeltaTime;
		if (BlasterGameState) BlasterGameState->RemainingCountdown = CountdownTime;
		if (CountdownTime <= 0.f)
		{
			ExecuteHalftimeSwap();
			StartRoundPrepare();  // 下半场第一回合
		}
	}
	else if (MatchState == MatchState::MatchEnd)
	{
		CountdownTime -= DeltaTime;
		if (BlasterGameState) BlasterGameState->RemainingCountdown = CountdownTime;
		if (CountdownTime <= 0.f)
		{
			ReturnToLobby();
		}
	}
}

// ========================================================================
// 阵营分配：比赛开始一次性随机分配，整场不变
// ========================================================================
void ABombDefusalGameMode::AssignTeamsOnce()
{
	if (bTeamsAssigned) return;
	bTeamsAssigned = true;

	TArray<ABlasterPlayerState*> ActivePlayers = GetActivePlayers();

	// Fisher-Yates 打乱：保证随机公平
	for (int32 i = ActivePlayers.Num() - 1; i > 0; i--)
	{
		int32 j = FMath::RandRange(0, i);
		ActivePlayers.Swap(i, j);
	}

	// 奇数 N → 攻击者多一个（ceil(N/2)）
	int32 AttackerCount = FMath::CeilToInt(ActivePlayers.Num() / 2.0f);
	for (int32 i = 0; i < ActivePlayers.Num(); i++)
	{
		ETeamID Team = (i < AttackerCount) ? ETeamID::ETI_Attacker : ETeamID::ETI_Defender;
		ActivePlayers[i]->SetTeamID(Team);

		// ── 新增：分配 LogicalTeam ──
		// 前一半 → TeamA，后一半 → TeamB（与 Attacker/Defender 分配一致，上半场 TeamA=Attacker）
		ELogicalTeam LT = (i < AttackerCount) ? ELogicalTeam::ELT_TeamA : ELogicalTeam::ELT_TeamB;
		ActivePlayers[i]->SetLogicalTeam(LT);

		// ── 新增：发放起始金 ──
		if (BlasterGameState && BlasterGameState->EconomyConfig)
		{
			ActivePlayers[i]->Money = BlasterGameState->EconomyConfig->StartingMoney;  // $200
		}

		// ── 新增：归零 RoundKills ──
		ActivePlayers[i]->ResetRoundKills();
	}
	// P3 主流方案：大厅断开玩家不纳入对局（未参赛，重连按新玩家/中途加入处理）→ 删除原"重建 Bomb PS"逻辑。
}

// ========================================================================
// 回合生命周期函数
// ========================================================================
void ABombDefusalGameMode::StartRoundPrepare()
{
	RoundNumber++;

	// 新回合开始：清除上一回合胜者，避免中途加入/重连的客户端在公告上
	// 看到上一回合的结果（LastRoundWinner 只在 EndRound 写入，准备期应为 None）
	LastRoundWinner = ETeamID::ETI_None;

	// [NEW] Step 7: 回合开始前清理上回合残留
	ClearAllBuffsOnAllPlayers();    // 清除所有存活玩家的 Buff
	CleanupDroppedWeapons();        // 销毁地面遗留武器
	CleanupBomb();                  // 销毁上局炸弹（BombMode Phase 3）

	CountdownTime = RoundPrepareTime;

	// 必须先推送到 GameState，再切 MatchState：
	// SetMatchState → HandleRoundPrepare → 读 GameState，Sync 在后会导致读到旧值
	SyncToGameState();
	if (BlasterGameState) BlasterGameState->BroadcastRoundInfo();  // 委托驱动 Widget 刷新
	SetMatchState(MatchState::RoundPrepare);

	// 清尸体、重生所有玩家、重置存活计数
	CleanupBodiesAndRespawn();
}

void ABombDefusalGameMode::StartRoundInProgress()
{
	// 重新校准存活计数：RoundPrepare 期间可能有玩家退出导致计数失准
	// 直接写入 GameState（唯一权威源），引擎自动复制到所有客户端
	if (!BlasterGameState) BlasterGameState = GetGameState<ABlasterGameState>();
	if (BlasterGameState)
	{
		BlasterGameState->AttackerAliveCount = GetPlayersInTeam(ETeamID::ETI_Attacker).Num();
		BlasterGameState->DefenderAliveCount = GetPlayersInTeam(ETeamID::ETI_Defender).Num();
		// 重校准后广播：客户端通过 OnRep_AliveCount 自动广播，但服务端无 OnRep 机制，
		// 必须手动广播确保服务端 RoundOverlay 能看到最新存活人数
		BlasterGameState->BroadcastAliveCount();
	}

	// 归零所有玩家的回合击杀计数（新回合战斗开始）
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ABlasterPlayerState* PS = (*It)->GetPlayerState<ABlasterPlayerState>())
		{
			PS->ResetRoundKills();
		}
	}

	// 回合计时器启动：超时 → 保卫者获胜（经典爆破规则）
	CountdownTime = RoundTime;

	// ── 炸弹模式：分配炸弹给随机攻方（BombMode Phase 3）──
	AssignBombToRandomAttacker();

	SetMatchState(MatchState::RoundInProgress);

	// 恢复所有玩家的战斗输入（RoundPrepare 期间被 CleanupBodiesAndRespawn 禁用）
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (ABlasterCharacter* Char = Cast<ABlasterCharacter>((*It)->GetPawn()))
		{
			Char->bDisableGameplayInput = false;
		}
	}
}

void ABombDefusalGameMode::OnPlayerKilled(ABlasterCharacter* DeadCharacter,
	ABlasterPlayerController* VictimController,
	ABlasterPlayerController* AttackerController)
{
	// P3 主流方案：断线玩家角色被引擎销毁、不留在场上，不会有"无控角色被打死" → 删除原 FindPendingSessionByPawn 分支。
	// 若 VictimController 为空则跳过（防御，正常路径都有控制器）。
	ABlasterPlayerState* VictimPS = VictimController
		? Cast<ABlasterPlayerState>(VictimController->PlayerState) : nullptr;
	if (!VictimPS) return;

	// 加分统计（保留计分逻辑，可用于后续经济系统）
	ABlasterPlayerState* AttackerPS = AttackerController
		? Cast<ABlasterPlayerState>(AttackerController->PlayerState) : nullptr;

	if (AttackerPS && AttackerPS != VictimPS)
	{
		AttackerPS->AddToScore(1.f);
		// ── 经济系统：回合击杀计数 +1（不发钱，回合结束时统一结算）──
		AttackerPS->IncrementRoundKills();
	}
	if (VictimPS)
	{
		VictimPS->AddToDefeats(1);
	}

	// 死亡角色表现处理（播放动画 + 禁用输入/碰撞）
	if (DeadCharacter)
	{
		DeadCharacter->Elim();
	}

	// P1 观战：死亡后进入观战（服务器 + 客户端两侧一致，见 EnterDeathSpectator 注释）。
	// 传入 DeadCharacter：客户端死亡镜头视角锁它（3s 后由 BlasterCharacter::DestroyCorpse 销毁）。
	// 下轮重生时 RestartPlayer → ClientRestart 自动退出观战恢复第三人称。
	if (VictimController)
	{
		VictimController->EnterDeathSpectator(DeadCharacter);
		UE_LOG(LogTemp, Log, TEXT("[Spectate] 玩家 %s 死亡 → 进入观战"),
			*GetNameSafe(VictimController));
	}

	// 事件驱动递减存活计数器（O(1) 判定，直接写入 GameState 唯一权威源）
	if (!BlasterGameState) BlasterGameState = GetGameState<ABlasterGameState>();
	if (VictimPS && BlasterGameState)
	{
		if (VictimPS->TeamID == ETeamID::ETI_Attacker)
			BlasterGameState->AttackerAliveCount--;
		else if (VictimPS->TeamID == ETeamID::ETI_Defender)
			BlasterGameState->DefenderAliveCount--;

		BlasterGameState->BroadcastAliveCount();  // 委托驱动 Widget 刷新
	}

	// ── 炸弹模式：携带者死亡 → 掉落炸弹（BombMode Phase 3）──
	if (DeadCharacter && CurrentBomb && CurrentBomb->GetBombState() == EBombState::EBS_Carried
		&& CurrentBomb->GetOwner() == DeadCharacter)
	{
		DropBombFromDeadPlayer(DeadCharacter);
	}

	CheckRoundEnd();
}

void ABombDefusalGameMode::CheckRoundEnd()
{
	// 仅在战斗中检查：回合结束/准备阶段不重复判定
	if (MatchState != MatchState::RoundInProgress) return;
	if (!BlasterGameState) return;

	// ── 炸弹模式扩展（BombMode Phase 3）──
	// 炸弹已安放时：攻方全灭不结束（守方仍需拆包或等爆炸）
	if (CurrentBomb && CurrentBomb->GetBombState() == EBombState::EBS_Planted)
	{
		// 守方全灭 → 攻方胜（即使炸弹还在倒计时）
		if (BlasterGameState->DefenderAliveCount <= 0)
			EndRound(ETeamID::ETI_Attacker);
		// 攻方全灭 → 不结束，等炸弹爆炸或守方拆包
		return;
	}

	if (BlasterGameState->AttackerAliveCount <= 0)
		EndRound(ETeamID::ETI_Defender);
	else if (BlasterGameState->DefenderAliveCount <= 0)
		EndRound(ETeamID::ETI_Attacker);
}

void ABombDefusalGameMode::EndRound(ETeamID Winner)
{
	// 存储上一回合胜者，供 HandleRoundEnd 显示
	LastRoundWinner = Winner;

	// ── 映射 Winner(ETeamID) → LogicalTeam，递增逻辑队胜场（直接写入 GameState）──
	ELogicalTeam WinningLT = GetLogicalTeamFromRole(Winner);
	if (BlasterGameState) BlasterGameState->AddRoundWin(WinningLT);

	// ── 回合经济发放（所有金钱变动的唯一入口）──
	DistributeRoundEconomy(WinningLT);

	CountdownTime = RoundEndTime;

	// 必须先推送到 GameState，再切 MatchState，确保 HandleRoundEnd 读到最新值
	SyncToGameState();
	if (BlasterGameState) BlasterGameState->BroadcastRoundResult();  // 委托驱动 Widget 刷新
	SetMatchState(MatchState::RoundEnd);
}

void ABombDefusalGameMode::CheckMatchEnd()
{
	if (!BlasterGameState) return;

	if (BlasterGameState->GetRoundWinsForTeam(ELogicalTeam::ELT_TeamA) >= RoundsToWin)
		ConcludeMatch(ELogicalTeam::ELT_TeamA);
	else if (BlasterGameState->GetRoundWinsForTeam(ELogicalTeam::ELT_TeamB) >= RoundsToWin)
		ConcludeMatch(ELogicalTeam::ELT_TeamB);
	else if (RoundNumber == BlasterGameState->HalftimeRound && !BlasterGameState->bIsSecondHalf)
	{
		CountdownTime = HalftimeSwapTime;
		SetMatchState(MatchState::HalftimeSwap);
	}
	else
		StartRoundPrepare();
}

void ABombDefusalGameMode::ConcludeMatch(ELogicalTeam Winner)
{
	// ── 映射 LogicalTeam → ETeamID（按当前半场反推）──
	// 上半场: TeamA=Attacker, TeamB=Defender
	// 下半场: TeamA=Defender, TeamB=Attacker
	// ── 写入新字段：ELogicalTeam 维度（Phase 5 新增）──
	if (BlasterGameState) BlasterGameState->LastMatchWinnerLT = Winner;

	// 旧字段：按半场反推 ETeamID（Widget 仍通过旧字段读比赛胜者）
	const bool bSecondHalf = BlasterGameState ? BlasterGameState->bIsSecondHalf : false;
	if (bSecondHalf)
	{
		LastMatchWinner = (Winner == ELogicalTeam::ELT_TeamA)
			? ETeamID::ETI_Defender : ETeamID::ETI_Attacker;
	}
	else
	{
		LastMatchWinner = (Winner == ELogicalTeam::ELT_TeamA)
			? ETeamID::ETI_Attacker : ETeamID::ETI_Defender;
	}

	CountdownTime = MatchEndTime;

	// 必须先推送到 GameState，再切 MatchState，确保 HandleMatchEnd 读到最新值
	SyncToGameState();
	if (BlasterGameState) BlasterGameState->BroadcastMatchResult();  // 委托驱动 Widget 刷新
	SetMatchState(MatchState::MatchEnd);

	// ── 持久化（P4）：比赛结束 → 快照统计并入队写入 SQLite ──
	// 本函数只"入队"不"等待"：真正的 DB 写由后台线程（UBlasterPersistenceSubsystem 的 worker）完成。
	// 为什么必须异步：ReturnToLobby 的 ServerTravel 会销毁世界，若同步等写盘将阻塞游戏线程
	// 甚至在 travel 后丢数据；入队后即使服务器立刻 travel，worker 线程仍会把数据落盘。
	if (UBlasterPersistenceSubsystem* Persistence = UBlasterPersistenceSubsystem::Get())
	{
		Persistence->EnqueueMatchResult(BuildMatchResultRecord(Winner));
	}
}

// P4 持久化：把本场最终战绩快照为纯数据结构（不引用任何 UObject → worker 线程安全）。
// 遍历 GameState->PlayerArray（权威全列表，含中途加入/掉线者）读取各 PlayerState 终值。
FMatchResultRecord ABombDefusalGameMode::BuildMatchResultRecord(ELogicalTeam Winner) const
{
	FMatchResultRecord Record;
	Record.TimestampUtc  = FDateTime::UtcNow().ToIso8601();  // 入队时（游戏线程）取服务器时间
	Record.MapName       = GetWorld() ? GetWorld()->GetMapName() : TEXT("Unknown");
	Record.WinnerTeam    = (Winner == ELogicalTeam::ELT_TeamA) ? TEXT("TeamA")
	                     : (Winner == ELogicalTeam::ELT_TeamB) ? TEXT("TeamB") : TEXT("None");
	Record.TeamARoundWins = BlasterGameState ? BlasterGameState->TeamARoundWins : 0;
	Record.TeamBRoundWins = BlasterGameState ? BlasterGameState->TeamBRoundWins : 0;
	Record.RoundsPlayed   = RoundNumber;

	if (GameState)
	{
		for (APlayerState* PS : GameState->PlayerArray)
		{
			ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
			if (!BPS) continue;

			FPlayerMatchRecord P;
			P.PlayerId     = BPS->GetPlayerId();   // 客户端上报的持久身份（按人归集的键）
			P.PlayerName   = BPS->GetPlayerName(); // 展示名（不唯一）
			P.TeamID       = (int32)BPS->TeamID;
			P.LogicalTeam  = (int32)BPS->LogicalTeam;
			P.Kills        = (int32)BPS->GetScore();  // 每击杀 +1（OnPlayerKilled）
			P.Deaths       = BPS->GetDefeats();
			P.RoundKills   = BPS->GetRoundKills();    // 最后一回合快照（冗余，便于演示经济）
			P.Money        = BPS->Money;
			Record.Players.Add(P);
		}
	}
	return Record;
}

void ABombDefusalGameMode::ReturnToLobby()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("[BombDefusalGameMode] ReturnToLobby → ABORT: GetWorld() is null!"));
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[BombDefusalGameMode] ReturnToLobby | WorldType=%d | NetMode=%d | bUseSeamlessTravel=%d | MapPath=%s | MatchState=%s"),
		(int32)World->WorldType, (int32)GetNetMode(), bUseSeamlessTravel, *LobbyMapPath, *MatchState.ToString());

	// 先切到 LeavingMap 防重入：ServerTravel 是帧末延迟执行的，
	// 若不切状态，Tick 会在后续帧重复调用 ReturnToLobby
	SetMatchState(MatchState::LeavingMap);

	// P3 主流方案：对局结束回大厅 → 清空待重连表。
	// 否则断线玩家的 PS 被强引用跨场残留：下一场重连可能命中上一场条目 → 串状态/串经济。
	if (UBlasterSessionManager* Mgr = UBlasterSessionManager::Get())
	{
		Mgr->ClearPendingSessions();
	}

	const bool bTravelSuccess = World->ServerTravel(LobbyMapPath);
	if (bTravelSuccess)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[BombDefusalGameMode] ReturnToLobby → ServerTravel SUCCESS, returning to lobby..."));
	}
	else
	{
		UE_LOG(LogTemp, Error,
			TEXT("[BombDefusalGameMode] ReturnToLobby → ServerTravel FAILED! MapPath=%s | bUseSeamlessTravel=%d | WorldType=%d"),
			*LobbyMapPath, bUseSeamlessTravel, (int32)World->WorldType);
	}
}

// ========================================================================
// 阵营出生点选择：覆盖 UE 原生 ChoosePlayerStart 钩子
// ========================================================================
// 策略：按 PlayerState->TeamID 筛选同阵营 ATeamPlayerStart，随机返回一个。
//       子类覆盖此方法即可替换选点策略，无需修改 CleanupBodiesAndRespawn。
// 回退：
//   [1] 同阵营有专属点 → 随机取
//   [2] 同阵营无专属点 → 从所有 ATeamPlayerStart 随机取（兼容全部 None 的旧配置）
//   [3] 关卡无 ATeamPlayerStart → 走引擎默认 ChoosePlayerStart（普通 APlayerStart）
AActor* ABombDefusalGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	ABlasterPlayerState* PS = Player->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return Super::ChoosePlayerStart_Implementation(Player);

	// [1] 收集所有 ATeamPlayerStart（已包括普通 APlayerStart 的超集）
	TArray<AActor*> AllTeamStarts;
	UGameplayStatics::GetAllActorsOfClass(this, ATeamPlayerStart::StaticClass(), AllTeamStarts);

	// [2] 按 PS->TeamID 筛选同阵营出生点
	TArray<AActor*> MyTeamStarts;
	for (AActor* Start : AllTeamStarts)
	{
		const ATeamPlayerStart* TPS = Cast<ATeamPlayerStart>(Start);
		if (TPS && TPS->Team == PS->TeamID)
		{
			MyTeamStarts.Add(Start);
		}
	}

	// [3] 同阵营有专属点 → 随机选取
	if (MyTeamStarts.Num() > 0)
	{
		return MyTeamStarts[FMath::RandRange(0, MyTeamStarts.Num() - 1)];
	}

	// [4] 回退：关卡有 ATeamPlayerStart 但该阵营无专属点 → 全量随机
	if (AllTeamStarts.Num() > 0)
	{
		return AllTeamStarts[FMath::RandRange(0, AllTeamStarts.Num() - 1)];
	}

	// [5] 最终回退：关卡未放置 ATeamPlayerStart → 引擎默认逻辑（普通 APlayerStart）
	return Super::ChoosePlayerStart_Implementation(Player);
}

// ========================================================================
// 复活逻辑：销毁死尸 → 重生所有玩家 → 重置存活计数
// ========================================================================
void ABombDefusalGameMode::CleanupBodiesAndRespawn()
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC) continue;

		// 选点策略委托给 ChoosePlayerStart：同阵营筛选、回退、策略替换全部解耦
		AActor* BestStart = ChoosePlayerStart(PC);

		ABlasterCharacter* OldCharacter = Cast<ABlasterCharacter>(PC->GetPawn());

		// 存活玩家：保留 Pawn，传送回重生点（武器/投掷物/血条/护盾自然继承）
		if (OldCharacter && !OldCharacter->IsElimmed())
		{
			if (BestStart)
			{
				OldCharacter->SetActorLocation(BestStart->GetActorLocation());
			}
			OldCharacter->bDisableGameplayInput = true;
			continue;
		}

		// 死亡玩家：销毁尸体 → 重生 → 发放默认武器
		if (OldCharacter)
		{
			PC->UnPossess();
			OldCharacter->Destroy();
		}

		// P2：等待参战的中途加入者（IsSpectator=true 但有队伍）重生 → 恢复活跃。
		// 不清除会一直被 GetPlayersInLogicalTeam/ExecuteHalftimeSwap 的 IsSpectator() 过滤排除 → 吃不到经济/半场不翻转。
		if (ABlasterPlayerState* PS = PC->GetPlayerState<ABlasterPlayerState>())
		{
			if (PS->IsSpectator())
			{
				PS->SetIsSpectator(false);
			}
		}

		if (BestStart)
		{
			RestartPlayerAtPlayerStart(PC, BestStart);
		}
		else
		{
			RestartPlayer(PC);
		}

		// 准备阶段禁止移动/战斗输入，只允许转视角和购买
		if (ABlasterCharacter* NewChar = Cast<ABlasterCharacter>(PC->GetPawn()))
		{
			NewChar->bDisableGameplayInput = true;
		}
	}

	// P3 主流方案：断线玩家角色被引擎销毁、不重生（重连后由 PC 遍历正常重生）→ 删除原"无控重生"逻辑。

	// 重置存活计数：GetPlayersInTeam 已排除待重连表玩家（断线者没角色、不算存活），
	// 无需再单独减 PendingAtk/Def（避免双重扣除，方案 A）。
	if (!BlasterGameState) BlasterGameState = GetGameState<ABlasterGameState>();
	if (BlasterGameState)
	{
		BlasterGameState->AttackerAliveCount = GetPlayersInTeam(ETeamID::ETI_Attacker).Num();
		BlasterGameState->DefenderAliveCount = GetPlayersInTeam(ETeamID::ETI_Defender).Num();
		BlasterGameState->BroadcastAliveCount();  // 委托驱动 Widget 刷新
	}
}

// ========================================================================
// 状态推送：服务器 MatchState 变化 → 通知所有 PlayerController
// ========================================================================
void ABombDefusalGameMode::OnMatchStateSet()
{
	Super::OnMatchStateSet();

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* BlasterPlayer = Cast<ABlasterPlayerController>(*It);
		if (BlasterPlayer)
		{
			BlasterPlayer->OnMatchStateSet(MatchState, true);
		}
	}
}

// ========================================================================
// 玩家加入/退出
// ========================================================================
void ABombDefusalGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	if (bTeamsAssigned)
	{
		// ── P3 中途加入：只标记真实 Bomb 登录 ──
		// token 签发/下发 + HandleMidRoundJoin 移到 ServerAuthenticateSession：
		// ① 重连者必须先过 authenticate 检测（命中待重连表 → 恢复），不能 PostLogin 直接当新玩家 setup；
		// ② PostLogin 若下发新 token 会覆盖客户端文件 → 重连者 BeginPlay 可能读到新 token → 出示失败（竞态）。
		if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(NewPlayer))
		{
			BPC->bIsMidJoinCandidate = true;
		}
	}
	// else：比赛未开始，AssignTeamsOnce 时统一分配
}

void ABombDefusalGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);

	//是不是"对局进行中"
	const bool bMatchInProgress = bTeamsAssigned && MatchState != MatchState::LeavingMap;
	if (!bMatchInProgress)
	{
		//HandleMidRoundLeave 是"不配/不值得重连留场的玩家离开时"的收尾处理器
		HandleMidRoundLeave(Exiting);
		return;
	}

	//有没有身份凭证
	ABlasterPlayerState* PS = Exiting->GetPlayerState<ABlasterPlayerState>();
	if (!PS || PS->GetSessionToken().IsEmpty())
	{
		HandleMidRoundLeave(Exiting);
		return;
	}

	// 主流方案：注册待重连表（PS 保留 → 续吃经济/统计）。
	// ⚠ 角色已被引擎销毁（断线清理），不保留 Pawn（原"留场"方案废弃）。
	// PS 的存活由 CleanupPlayerState 重写保留（见 ABlasterPlayerController::CleanupPlayerState）。
	FPendingSession Pending; //构造 FPendingSession 快照
	Pending.PlayerState  = PS;
	Pending.TeamID       = PS->TeamID;
	Pending.LogicalTeam  = PS->LogicalTeam;
	Pending.Money        = PS->Money;
	Pending.bInMatch     = true;
	if (UBlasterSessionManager* Mgr = UBlasterSessionManager::Get())
	{
		Mgr->RegisterPendingSession(PS->GetSessionToken(), MoveTemp(Pending));// 注册进表，key = token
	}

	// 角色被销毁 = 队伍少一人 → 存活断开则递减 AliveCount + 检查回合结束。
	// 已死断开（bWasAliveAtDisconnect=false）→ 死亡时已递减，勿重复。
	ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(Exiting);
	if (BPC && BPC->bWasAliveAtDisconnect && BlasterGameState)
	{
		if (PS->TeamID == ETeamID::ETI_Attacker)
			BlasterGameState->AttackerAliveCount--;
		else if (PS->TeamID == ETeamID::ETI_Defender)
			BlasterGameState->DefenderAliveCount--;
		BlasterGameState->BroadcastAliveCount();
		CheckRoundEnd();
	}
	UE_LOG(LogTemp, Log, TEXT("[Reconnect] 玩家 %s 断线（角色移除）| token=%s | 断开时存活=%d"),
		*GetNameSafe(PS), *PS->GetSessionToken(), BPC ? BPC->bWasAliveAtDisconnect : 0);
}

void ABombDefusalGameMode::HandleMidRoundJoin(APlayerController* NewPlayer)
{
	ABlasterPlayerState* PS = NewPlayer->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// ① 分到人少阵营（保持双方平衡）
	int32 AtkCount = GetPlayersInTeam(ETeamID::ETI_Attacker).Num();
	int32 DefCount = GetPlayersInTeam(ETeamID::ETI_Defender).Num();
	ETeamID AssignedTeam = (AtkCount <= DefCount)
		? ETeamID::ETI_Attacker : ETeamID::ETI_Defender;
	PS->SetTeamID(AssignedTeam);

	// ② 逻辑队伍（按半场映射角色→逻辑队，与经济归集一致 —— 半场后新加入按当前半场映射）
	PS->SetLogicalTeam(GetLogicalTeamFromRole(AssignedTeam));

	// ③ 初始经济 $200（与开局玩家一致，不带累计 —— 新加入 vs 重连的核心区别）
	if (BlasterGameState && BlasterGameState->EconomyConfig)
	{
		PS->Money = BlasterGameState->EconomyConfig->StartingMoney;
	}

	// ④ 等待期标记观战（不算活跃、不吃经济；下轮重生时 CleanupBodiesAndRespawn 清除）
	PS->SetIsSpectator(true);

	if (MatchState == MatchState::RoundInProgress)
	{
		// ⑤ 回合进行中 → 进观战（自由飞行），下轮 CleanupBodiesAndRespawn 重生参战
		if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(NewPlayer))
		{
			BPC->EnterJoinSpectator();
		}
	}
	else
	{
		// 非战斗期 → 立即生成参战（清除观战标记，恢复活跃/经济）
		PS->SetIsSpectator(false);
		RestartPlayer(NewPlayer);
	}
}

void ABombDefusalGameMode::HandleMidRoundLeave(AController* Exiting)
{
	ABlasterPlayerState* PS = Exiting->GetPlayerState<ABlasterPlayerState>();
	if (!PS) return;

	// 从存活计数中移除（如果是战斗中且该玩家还活着）
	if (MatchState == MatchState::RoundInProgress)
	{
		APawn* ExitingPawn = Exiting->GetPawn();
		ABlasterCharacter* ExitingChar = Cast<ABlasterCharacter>(ExitingPawn);
		// 仅在未死亡时递减：已死亡的玩家已经在 OnPlayerKilled 中递减过了
		if (ExitingChar && !ExitingChar->IsElimmed() && BlasterGameState)
		{
			if (PS->TeamID == ETeamID::ETI_Attacker)
				BlasterGameState->AttackerAliveCount--;
			else if (PS->TeamID == ETeamID::ETI_Defender)
				BlasterGameState->DefenderAliveCount--;

			BlasterGameState->BroadcastAliveCount();  // 委托驱动 Widget 刷新
		}
		CheckRoundEnd();
	}
}

// ========================================================================
// P3 断线重连：重连恢复 + 按队伍选点（无控制器）
// ========================================================================

// 重连恢复（服务器执行，ServerAuthenticateSession 命中待重连表时调用）：
// 换绑 PS（续吃经济/战绩）→ 重发原 token（覆盖 PostLogin 竞态）→ 消费待重连条目
// → Pawn 存活则 Possess 拿回控制；已死则进观战（团队锁定）等下轮重生。
void ABombDefusalGameMode::RestoreReconnectedPlayer(
	ABlasterPlayerController* NewPC, const FPendingSession& Pending, const FString& PresentedToken)
{
	ABlasterPlayerState* PendingPS = Pending.PlayerState.Get();
	if (!PendingPS || !NewPC) return;

	// ① 换绑 PS：销毁新 PS（含其 PlayerArray 注册），新 PC 绑定待重连 PS（续吃经济/统计）。
	//    保留的 PS 的 Owner 是已销毁的旧 PC → 重设为新 PC。
	if (APlayerState* FreshPS = NewPC->PlayerState)
	{
		if (AGameStateBase* GS = GetWorld()->GetGameState())
		{
			GS->RemovePlayerState(FreshPS);
		}
		FreshPS->Destroy();
	}
	NewPC->PlayerState = PendingPS;      // 复制属性，同步客户端
	PendingPS->SetOwner(NewPC);
	PendingPS->SetIsSpectator(false);    // 恢复非观战

	// ② 重发原 token（覆盖 PostLogin 阶段可能的覆盖，保证客户端文件与恢复后 PS 一致）
	NewPC->ClientReceiveSessionToken(PendingPS->GetSessionToken());

	// ③ 消费待重连条目（避免下次再命中）
	if (UBlasterSessionManager* Mgr = UBlasterSessionManager::Get())
	{
		Mgr->RemovePendingSession(PresentedToken);
	}

	// ④ 主流方案：场上无角色可拿回（角色被引擎销毁）→ 非战斗期立即重生 / 战斗期进观战等下轮
	if (MatchState != MatchState::RoundInProgress)
	{
		RestartPlayer(NewPC);
	}
	else
	{
		NewPC->EnterDeathSpectator(nullptr);
	}
	UE_LOG(LogTemp, Log, TEXT("[Reconnect] 玩家 %s 重连恢复（角色移除→重生/观战）"), *GetNameSafe(PendingPS));
}

// ========================================================================
// 将 CountdownTime / 回合信息推送到 GameState（服务器执行）
// 客户端通过 GetGameState<ABlasterGameState>() 读取，无需依赖 GameMode
// ========================================================================
void ABombDefusalGameMode::SyncToGameState()
{
	if (!BlasterGameState)
		BlasterGameState = GetGameState<ABlasterGameState>();
	if (!BlasterGameState) return;

	BlasterGameState->CurrentRoundNumber = RoundNumber;
	BlasterGameState->LastRoundWinner = LastRoundWinner;
	BlasterGameState->LastMatchWinner = LastMatchWinner;
}

// ========================================================================
// 辅助函数
// ========================================================================
TArray<ABlasterPlayerState*> ABombDefusalGameMode::GetPlayersInTeam(ETeamID Team) const
{
	TArray<ABlasterPlayerState*> Result;
	if (!GameState) return Result;

	for (APlayerState* PS : GameState->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		// P3 主流方案：断线玩家（待重连表）无角色、不算队内活跃人数。
		// 收敛到这一处 → AliveCount 重校准 / 炸弹分配 / 阵营平衡 自动正确（方案 A）。
		if (BPS && BPS->TeamID == Team && !IsInPendingSessions(BPS))
		{
			Result.Add(BPS);
		}
	}
	return Result;
}

// ── P3 主流方案：断线玩家判定 ──
// 该 PS 是否在待重连表（bInMatch=true 的条目）。断线玩家角色被引擎销毁、重连前无角色：
// 经济发放（GetPlayersInLogicalTeam）/ 存活计数（GetPlayersInTeam）/ 半场金钱重置 统一排除。
bool ABombDefusalGameMode::IsInPendingSessions(const ABlasterPlayerState* PS) const
{
	if (!PS) return false;
	if (UBlasterSessionManager* Mgr = UBlasterSessionManager::Get())
	{
		for (const auto& Pair : Mgr->GetPendingSessions())
		{
			if (Pair.Value.bInMatch && Pair.Value.PlayerState.Get() == PS)
			{
				return true;
			}
		}
	}
	return false;
}

TArray<ABlasterPlayerState*> ABombDefusalGameMode::GetActivePlayers() const
{
	TArray<ABlasterPlayerState*> Result;
	if (!GameState) return Result;

	for (APlayerState* PS : GameState->PlayerArray)
	{
		// 排除等待中的玩家（中途加入等待下回合者不算活跃）
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (BPS && !PS->IsSpectator() && !PS->IsABot())
		{
			Result.Add(BPS);
		}
	}
	return Result;
}

// ========================================================================
// 经济系统辅助函数
// ========================================================================

// ── 角色 → 逻辑队伍映射 ──
// 上半场: Attacker=TeamA, Defender=TeamB
// 下半场: Attacker=TeamB, Defender=TeamA（角色翻转后）
ELogicalTeam ABombDefusalGameMode::GetLogicalTeamFromRole(ETeamID TeamRole) const
{
	const bool bSecondHalf = BlasterGameState ? BlasterGameState->bIsSecondHalf : false;
	if (!bSecondHalf)
	{
		if (TeamRole == ETeamID::ETI_Attacker) return ELogicalTeam::ELT_TeamA;
		if (TeamRole == ETeamID::ETI_Defender) return ELogicalTeam::ELT_TeamB;
	}
	else
	{
		if (TeamRole == ETeamID::ETI_Attacker) return ELogicalTeam::ELT_TeamB;
		if (TeamRole == ETeamID::ETI_Defender) return ELogicalTeam::ELT_TeamA;
	}
	return ELogicalTeam::ELT_None;
}

// ── 按 LogicalTeam 筛选玩家 ──
TArray<ABlasterPlayerState*> ABombDefusalGameMode::GetPlayersInLogicalTeam(ELogicalTeam LT) const
{
	TArray<ABlasterPlayerState*> Result;
	if (!GameState) return Result;

	for (APlayerState* PS : GameState->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (BPS && BPS->LogicalTeam == LT && !BPS->IsSpectator() && !IsInPendingSessions(BPS))
		{
			// P3 主流方案（经济冻结）：断线玩家（待重连表）不参与回合经济发放。
			// 重连后从待重连表移除 → 恢复领取。
			Result.Add(BPS);
		}
	}
	return Result;
}

// ── 回合经济发放（EndRound 末尾调用，所有金钱变动的唯一入口）──
void ABombDefusalGameMode::DistributeRoundEconomy(ELogicalTeam WinningLT)
{
	if (!BlasterGameState || !BlasterGameState->EconomyConfig) return;

	UEconomyConfig* Config = BlasterGameState->EconomyConfig;

	// ① 确定败方
	ELogicalTeam LosingLT = (WinningLT == ELogicalTeam::ELT_TeamA)
		? ELogicalTeam::ELT_TeamB : ELogicalTeam::ELT_TeamA;

	// ② ⚠ 快照胜方连败次数 —— 必须在归零前！
	int32 SnapshotLoss = BlasterGameState->GetLossStreakForTeam(WinningLT);

	// ③ 更新计数器（直接写入 GameState）
	BlasterGameState->ResetLossStreak(WinningLT);
	BlasterGameState->IncrementWinStreak(WinningLT);
	BlasterGameState->ResetWinStreak(LosingLT);
	BlasterGameState->IncrementLossStreak(LosingLT);

	// ④ 计算胜方奖励
	int32 LossBonus = Config->GetLossBonus(SnapshotLoss);
	int32 WinStreak = BlasterGameState->GetWinStreakForTeam(WinningLT);
	int32 BaseReward = (WinStreak >= Config->WinStreakThreshold)
		? Config->WinStreakPenalty : Config->WinBaseReward;

	// ⑤ 发放：胜方每人 BaseReward + LossBonus + KillReward×RoundKills
	for (ABlasterPlayerState* PS : GetPlayersInLogicalTeam(WinningLT))
	{
		PS->AddMoney(BaseReward + LossBonus);
		PS->AddMoney(PS->GetRoundKills() * Config->KillReward);
		PS->ResetRoundKills();
	}

	// ⑥ 发放：败方每人 LossParticipation + KillReward×RoundKills
	for (ABlasterPlayerState* PS : GetPlayersInLogicalTeam(LosingLT))
	{
		PS->AddMoney(Config->LossParticipation);
		PS->AddMoney(PS->GetRoundKills() * Config->KillReward);
		PS->ResetRoundKills();
	}

	UE_LOG(LogTemp, Log, TEXT("[Economy] Round distributed: Winner=%s | Base=%d + LossBonus=%d"),
		WinningLT == ELogicalTeam::ELT_TeamA ? TEXT("TeamA") : TEXT("TeamB"),
		BaseReward, LossBonus);
}

// ── 半场交换（MR12：第 12 局结束后执行）──
void ABombDefusalGameMode::ExecuteHalftimeSwap()
{
	if (!BlasterGameState || !BlasterGameState->EconomyConfig) return;

	UEconomyConfig* Config = BlasterGameState->EconomyConfig;

	// ① 标记下半场（写入 GameState，必须在角色翻转之前）
	BlasterGameState->bIsSecondHalf = true;

	// ②+④ 翻转 ETeamID + 经济重置
	// P3 主流方案（经济冻结）：断线玩家（待重连表）跳过金钱重置（冻结不被半场冲掉），但队伍照常翻转。
	for (APlayerState* PS : GameState->PlayerArray)
	{
		ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(PS);
		if (!BPS || BPS->IsSpectator()) continue;

		if (BPS->TeamID == ETeamID::ETI_Attacker)
			BPS->SetTeamID(ETeamID::ETI_Defender);
		else if (BPS->TeamID == ETeamID::ETI_Defender)
			BPS->SetTeamID(ETeamID::ETI_Attacker);

		// 断线玩家（待重连表）跳过金钱重置（冻结不被半场冲掉），但队伍照常翻转。
		if (!IsInPendingSessions(BPS))
		{
			BPS->Money = Config->StartingMoney;
			BPS->OnMoneyChanged.Broadcast(BPS->Money, 0);
		}
	}

	// ③ 旧字段交换 —— Phase 5 删除！新字段是 LogicalTeam 维度不需要交换

	// ⑤ 连胜/连败归零（比分不重置）
	BlasterGameState->ResetAllStreaks();

	// [NEW] Step 7: 半场交换物品重置
	// A — 清除所有 Buff
	ClearAllBuffsOnAllPlayers();

	// B+C — 清除投掷物 + 发放默认武器（EquipWeapon 内部自动 Drop 旧武器到地面）
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(It->Get());
		if (!PC) continue;

		ABlasterCharacter* Character = Cast<ABlasterCharacter>(PC->GetPawn());
		if (!Character) continue;

		// B — 清除投掷物
		if (Character->GetThrowable())
		{
			Character->GetThrowable()->ClearAllThrowables();
		}

		// C — 发放默认武器（EquipWeapon → DropEquippedWeapon 自动将旧武器变为 Dropped）
		//     不主动 Destroy：否则 SpawDefaultWeapon → EquipWeapon → DropEquippedWeapon 访问野指针
		Character->SpawDefaultWeapon();
	}

	// D — 清理地面武器（销毁 B/C 步骤中 Drop 到地面的旧武器，兜底）
	CleanupDroppedWeapons();

	SyncToGameState();
	if (BlasterGameState) BlasterGameState->BroadcastRoundInfo();

	UE_LOG(LogTemp, Log, TEXT("[Economy] HalftimeSwap executed | Score: TeamA=%d TeamB=%d | All Money=$%d"),
		BlasterGameState->TeamARoundWins, BlasterGameState->TeamBRoundWins, Config->StartingMoney);
}

// ================================================================
// 炸弹模式：分配/事件/掉落/清理（BombMode Phase 3）
// ================================================================

// 回合开始：Spawn 炸弹 → 随机分配给一名攻方
void ABombDefusalGameMode::AssignBombToRandomAttacker()
{
	// 先清理上一局残留炸弹
	CleanupBomb();

	if (!BombActorClass) return; // 蓝图未配置 BombActor 子类，跳过

	TArray<ABlasterPlayerState*> Attackers = GetPlayersInTeam(ETeamID::ETI_Attacker);
	if (Attackers.Num() == 0) return;

	// 随机选一名攻方
	int32 RandomIndex = FMath::RandRange(0, Attackers.Num() - 1);
	ABlasterPlayerState* CarrierPS = Attackers[RandomIndex];

	// 找到该 PlayerState 对应的 Character
	ABlasterCharacter* Carrier = nullptr;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(It->Get());
		if (PC && PC->GetPlayerState<ABlasterPlayerState>() == CarrierPS)
		{
			Carrier = Cast<ABlasterCharacter>(PC->GetPawn());
			break;
		}
	}

	if (!Carrier) return;

	// Spawn 炸弹
	UWorld* World = GetWorld();
	if (!World) return;

	CurrentBomb = World->SpawnActor<ABombActor>(BombActorClass);
	if (!CurrentBomb) return;

	// 绑定炸弹事件到 GameMode 回调（GameMode 是订阅者，BombActor 是发布者）
	// 注意：BombActor 的委托使用 MULTICAST，绑定后不持有引用，解绑在 CleanupBomb 中
	CurrentBomb->OnBombPlanted.AddUObject(this, &ABombDefusalGameMode::OnBombPlanted);
	CurrentBomb->OnBombExploded.AddUObject(this, &ABombDefusalGameMode::OnBombExploded);
	CurrentBomb->OnBombDefused.AddUObject(this, &ABombDefusalGameMode::OnBombDefused);

	// Attach 到携带者身上
	CurrentBomb->AssignToCarrier(Carrier);

	UE_LOG(LogTemp, Log, TEXT("[Bomb] Assigned to random attacker: %s"), *Carrier->GetName());
}

// 炸弹安放事件回调 → 全服文字公告
void ABombDefusalGameMode::OnBombPlanted(ABombSite* Site)
{
	FString SiteName = Site ? Site->SiteName : TEXT("Unknown");
	FString Msg = FString::Printf(TEXT("炸弹已在 %s 点安放！"), *SiteName);

	// 全服公告：遍历所有 PlayerController → HUD Announcement
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(It->Get());
		if (PC)
		{
			PC->SetHUDAnnouncementCountdown(-1.f); // -1 表示显示公告文字而非倒计时
			// 通过已有的 MismatchNotification 通道显示炸弹公告（绿色提示 5 秒）
			PC->SetHUDMismatchNotification(Msg);
		}
	}
}

// 炸弹爆炸事件回调 → 攻方胜利
void ABombDefusalGameMode::OnBombExploded()
{
	EndRound(ETeamID::ETI_Attacker);
}

// 炸弹拆除事件回调 → 守方胜利
void ABombDefusalGameMode::OnBombDefused()
{
	EndRound(ETeamID::ETI_Defender);
}

// 携带者死亡：炸弹掉落在尸体位置
void ABombDefusalGameMode::DropBombFromDeadPlayer(ABlasterCharacter* DeadCharacter)
{
	if (!CurrentBomb || !DeadCharacter) return;

	FVector DropLocation = DeadCharacter->GetActorLocation();
	CurrentBomb->DropAtLocation(DropLocation);
}

// 回合结束：销毁炸弹 + 解绑委托
void ABombDefusalGameMode::CleanupBomb()
{
	if (!CurrentBomb) return;

	// 解绑委托（虽然 Actor 马上销毁，显式解绑防悬空）
	CurrentBomb->OnBombPlanted.RemoveAll(this);
	CurrentBomb->OnBombExploded.RemoveAll(this);
	CurrentBomb->OnBombDefused.RemoveAll(this);

	// 保险重置：非爆炸结束的回合（拆包/团灭）不会经过 Explode()，
	// 此处确保点位释放，防止下回合 bIsBombPlantedHere 粘滞
	if (CurrentBomb->GetPlantedSite())
	{
		CurrentBomb->GetPlantedSite()->bIsBombPlantedHere = false;
	}

	CurrentBomb->Destroy();
	CurrentBomb = nullptr;
}

// ================================================================
// 购买系统：物品分发
// ================================================================

void ABombDefusalGameMode::ProcessPurchase(ABlasterPlayerController* PC,
                                            const FShopItemRow& ItemRow)
{
    ABlasterCharacter* Character = Cast<ABlasterCharacter>(PC->GetPawn());
    if (!Character) return;

    switch (ItemRow.Category)
    {
    case EShopItemCategory::ESIC_Weapon:
        SpawnAndEquipPurchasedWeapon(Character, ItemRow.WeaponClass);
        break;

    case EShopItemCategory::ESIC_Ammo:
        GrantAmmoToEquippedWeapon(Character, ItemRow.AmmoWeaponType, ItemRow.AmmoAmount);
        break;

    case EShopItemCategory::ESIC_Throwable:
        AddThrowableToInventory(Character, ItemRow.ThrowableType);
        break;

    case EShopItemCategory::ESIC_Buff:
        ApplyBuffToCharacter(Character, ItemRow.BuffType);
        break;

    default:
        break;
    }
}

// ── 武器：Spawn + 设置初始弹药 + 装备 ──
void ABombDefusalGameMode::SpawnAndEquipPurchasedWeapon(
    ABlasterCharacter* Character, TSubclassOf<AWeapon> WeaponClass)
{
    if (!WeaponClass) return;

    UWorld* World = GetWorld();
    if (!World) return;

    // [1] 服务端生成武器 Actor
    AWeapon* Weapon = World->SpawnActor<AWeapon>(WeaponClass);
    if (!Weapon) return;

    // [2] 设置初始弹药：满弹匣 + 1 倍弹匣容量备弹
    Weapon->SetInitialAmmo(Weapon->GetMagCapacity(), Weapon->GetMagCapacity());

    // [3] 标记为非默认武器：死亡时掉落而非销毁
    Weapon->bDestroyWeapon = false;

    // [4] 装备武器（内部自动 DropEquippedWeapon 扔掉旧武器）
    Character->GetCombat()->EquipWeapon(Weapon);

    UE_LOG(LogTemp, Log, TEXT("[Shop] %s equipped purchased weapon: %s (Ammo=%d, Spare=%d)"),
        *Character->GetName(), *GetNameSafe(Weapon), Weapon->GetAmmo(), Weapon->GetSpareAmmo());
}

// ── 弹药：给手持武器补充备弹 ──
void ABombDefusalGameMode::GrantAmmoToEquippedWeapon(
    ABlasterCharacter* Character, EWeaponType AmmoWeaponType, int32 Amount)
{
    // [1] 获取手持武器
    AWeapon* EquippedWeapon = Character->GetEquippedWeapon();
    if (!EquippedWeapon) return;  // 无武器，拒绝

    // [2] 类型匹配校验
    if (EquippedWeapon->GetWeaponType() != AmmoWeaponType) return;

    // [3] 备弹已满校验
    if (EquippedWeapon->GetSpareAmmo() >= EquippedWeapon->GetMaxSpareAmmo()) return;

    // [4] 补充备弹（AddToSpare 内部已做 Clamp）
    EquippedWeapon->AddToSpare(Amount);

    UE_LOG(LogTemp, Log, TEXT("[Shop] %s granted %d ammo to %s (Spare=%d)"),
        *Character->GetName(), Amount, *GetNameSafe(EquippedWeapon),
        EquippedWeapon->GetSpareAmmo());
}

// ── 投掷物：增加库存 ──
void ABombDefusalGameMode::AddThrowableToInventory(
    ABlasterCharacter* Character, EThrowableType ThrowableType)
{
    UThrowableComponent* ThrowComp = Character->GetThrowable();
    if (!ThrowComp) return;

    // [1] 获取上限（从 EconomyConfig）
    ABlasterGameState* GS = GetGameState<ABlasterGameState>();
    if (!GS || !GS->EconomyConfig) return;

    int32 MaxCount = 0;
    switch (ThrowableType)
    {
    case EThrowableType::ETT_FragGrenade:
        MaxCount = GS->EconomyConfig->MaxFragGrenadeCount;
        break;
    case EThrowableType::ETT_Flashbang:
        MaxCount = GS->EconomyConfig->MaxFlashbangCount;
        break;
    case EThrowableType::ETT_SmokeGrenade:
        MaxCount = GS->EconomyConfig->MaxSmokeGrenadeCount;
        break;
    default:
        return;
    }

    // [2] 校验上限
    if (!ThrowComp->CanAddThrowable(ThrowableType, MaxCount)) return;

    // [3] 增加计数
    ThrowComp->AddThrowable(ThrowableType, 1);

    UE_LOG(LogTemp, Log, TEXT("[Shop] %s purchased throwable %d, now has %d"),
        *Character->GetName(), (int32)ThrowableType,
        ThrowComp->GetCount(ThrowableType));
}

// ── Buff：立即应用效果 ──
void ABombDefusalGameMode::ApplyBuffToCharacter(
    ABlasterCharacter* Character, EBuffType BuffType)
{
    UBuffComponent* BuffComp = Character->GetBuff();
    if (!BuffComp) return;

    switch (BuffType)
    {
    case EBuffType::EBT_Speed:
        BuffComp->BuffSpeed(1600.f, 850.f, 30.f);
        break;

    case EBuffType::EBT_Jump:
        BuffComp->BuffJump(4000.f, 30.f);
        break;

    case EBuffType::EBT_Shield:
        BuffComp->ReplenishShield(100.f, 5.f);
        break;

    case EBuffType::EBT_Heal:
        BuffComp->BuffHeal(100.f);  // 与 HealthPickup::HealAmount 调平
        break;

    default:
        break;
    }

    UE_LOG(LogTemp, Log, TEXT("[Shop] %s applied buff %d"),
        *Character->GetName(), (int32)BuffType);
}

// ================================================================
// 回合清理
// ================================================================

void ABombDefusalGameMode::CleanupDroppedWeapons()
{
    // 遍历世界中所有 AWeapon Actor，销毁 Dropped 状态的（地面遗留武器）
    for (TActorIterator<AWeapon> It(GetWorld()); It; ++It)
    {
        AWeapon* Weapon = *It;
        if (Weapon && Weapon->GetWeaponState() == EWeaponState::EWS_Dropped)
        {
            Weapon->Destroy();
        }
    }
}

void ABombDefusalGameMode::ClearAllBuffsOnAllPlayers()
{
    // 遍历所有 PlayerController，对存活角色清除 Buff
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        ABlasterPlayerController* PC = Cast<ABlasterPlayerController>(It->Get());
        if (!PC) continue;

        ABlasterCharacter* Character = Cast<ABlasterCharacter>(PC->GetPawn());
        if (Character && Character->GetBuff())
        {
            Character->GetBuff()->ClearAllBuffs();
        }
    }
}
