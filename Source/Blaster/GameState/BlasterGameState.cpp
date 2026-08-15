// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterGameState.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BlasterTypes/ShopTypes.h"
#include "Blaster/SSR/SSR_FrameHistory.h"
#include "Blaster/SSR/SSR_RewindManager.h"
#include "Net/UnrealNetwork.h"

// ════════════════════════════════════════════════════════════════
// 构造函数：必须在此设置 bCanEverTick（BeginPlay 设置太晚，Tick 系统不会注册）
// ════════════════════════════════════════════════════════════════

ABlasterGameState::ABlasterGameState()
{
	PrimaryActorTick.bCanEverTick = true;
}

// ════════════════════════════════════════════════════════════════
// BeginPlay：服务器端初始化 SSR 子系统（帧历史录制 + 回退管理器）
// ════════════════════════════════════════════════════════════════

void ABlasterGameState::BeginPlay()
{
	Super::BeginPlay();

	// 启用 Tick（SSR 帧录制需要每帧执行）
	PrimaryActorTick.bCanEverTick = true;

	if (HasAuthority())
	{
		SSR_FrameHistory = NewObject<USSR_FrameHistory>(this);
		SSR_FrameHistory->Initialize(this);

		SSR_RewindManager = NewObject<USSR_RewindManager>(this);
		SSR_RewindManager->Initialize(this);
	}
}

// ════════════════════════════════════════════════════════════════
// Tick：服务器每帧录制世界快照（必须在所有角色移动完成后）
// GameState::Tick 在 TG_DuringPhysics 阶段，角色移动在 TG_PrePhysics
// ════════════════════════════════════════════════════════════════

void ABlasterGameState::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (HasAuthority() && SSR_FrameHistory)
	{
		SSR_FrameHistory->RecordFrame();
	}
}

void ABlasterGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty> &OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ABlasterGameState, TopScoringPlayers);
    DOREPLIFETIME(ABlasterGameState, RemainingCountdown);
    DOREPLIFETIME(ABlasterGameState, CurrentRoundNumber);
    DOREPLIFETIME(ABlasterGameState, TeamARoundWins);
    DOREPLIFETIME(ABlasterGameState, TeamBRoundWins);
    DOREPLIFETIME(ABlasterGameState, TeamALossStreak);
    DOREPLIFETIME(ABlasterGameState, TeamBLossStreak);
    DOREPLIFETIME(ABlasterGameState, TeamAWinStreak);
    DOREPLIFETIME(ABlasterGameState, TeamBWinStreak);
    DOREPLIFETIME(ABlasterGameState, bIsSecondHalf);
    DOREPLIFETIME(ABlasterGameState, HalftimeRound);
    DOREPLIFETIME(ABlasterGameState, EconomyConfig);
    DOREPLIFETIME(ABlasterGameState, LastMatchWinnerLT);
    DOREPLIFETIME(ABlasterGameState, LastRoundWinner);
    DOREPLIFETIME(ABlasterGameState, LastMatchWinner);
    DOREPLIFETIME(ABlasterGameState, RoundPrepareDuration);
    DOREPLIFETIME(ABlasterGameState, RoundEndDuration);
    DOREPLIFETIME(ABlasterGameState, MatchEndDuration);
    DOREPLIFETIME(ABlasterGameState, AttackerAliveCount);
    DOREPLIFETIME(ABlasterGameState, DefenderAliveCount);
    DOREPLIFETIME(ABlasterGameState, ShopItemPrices);
}

// ------------------------------------------------------------
// 最高分排行榜维护（服务器执行）：每次玩家分数变动时调用，
// 更新 TopScoringPlayers 数组 —— 始终保持当前最高分玩家（支持并列）
// 数组通过 DOREPLIFETIME 复制到所有客户端，HandleCooldown 读取显示胜者
// ------------------------------------------------------------
void ABlasterGameState::OnRep_AliveCount()
{
	OnAliveCountChanged.Broadcast(AttackerAliveCount, DefenderAliveCount);
}

// ------------------------------------------------------------
// OnRep 回调：客户端收到 GameState 属性复制时，广播对应的本地委托
// 服务端由 BombDefusalGameMode 直接调用 BroadcastXxx()；
// 客户端通过 OnRep → BroadcastXxx() 链获得相同的通知，
// 确保 RoundOverlay / Announcement 委托绑定在两端行为一致
// ------------------------------------------------------------
void ABlasterGameState::OnRep_CurrentRoundNumber()
{
	BroadcastRoundInfo();  // 回合号变化 → 广播回合信息
}
void ABlasterGameState::OnRep_LastRoundWinner()
{
	BroadcastRoundResult(); // 回合胜者变化 → 广播回合结果
}
void ABlasterGameState::OnRep_LastMatchWinner()
{
	BroadcastMatchResult(); // 比赛胜者变化 → 广播比赛结果
}

// ── Phase 5 新增 OnRep ──
void ABlasterGameState::OnRep_TeamAWins()
{
	BroadcastRoundInfo();
	BroadcastRoundResult(); // 兜底：同队连胜时 OnRep_LastRoundWinner 不触发
}
void ABlasterGameState::OnRep_TeamBWins()
{
	BroadcastRoundInfo();
	BroadcastRoundResult();
}
void ABlasterGameState::OnRep_LastMatchWinnerLT()
{
	BroadcastMatchResult();
}

// 半场翻转到达客户端 → 立即广播回合信息：
// RoundOverlay::RefreshRoundInfo 依赖 bIsSecondHalf 做"攻击者/保卫者"标签与 TeamA/TeamB 比分的翻转映射，
// 广播保证翻转瞬间 ScoreText 立即按新半场刷新，而不是等下一次比分/回合号复制才更新。
void ABlasterGameState::OnRep_bIsSecondHalf()
{
	BroadcastRoundInfo();
}

// ── 经济辅助方法 ──
int32 ABlasterGameState::GetLossStreakForTeam(ELogicalTeam T) const
{
	return (T == ELogicalTeam::ELT_TeamA) ? TeamALossStreak : TeamBLossStreak;
}
int32 ABlasterGameState::GetWinStreakForTeam(ELogicalTeam T) const
{
	return (T == ELogicalTeam::ELT_TeamA) ? TeamAWinStreak : TeamBWinStreak;
}
void ABlasterGameState::IncrementLossStreak(ELogicalTeam T)
{
	if (T == ELogicalTeam::ELT_TeamA) TeamALossStreak++; else TeamBLossStreak++;
}
void ABlasterGameState::ResetLossStreak(ELogicalTeam T)
{
	if (T == ELogicalTeam::ELT_TeamA) TeamALossStreak = 0; else TeamBLossStreak = 0;
}
void ABlasterGameState::IncrementWinStreak(ELogicalTeam T)
{
	if (T == ELogicalTeam::ELT_TeamA) TeamAWinStreak++; else TeamBWinStreak++;
}
void ABlasterGameState::ResetWinStreak(ELogicalTeam T)
{
	if (T == ELogicalTeam::ELT_TeamA) TeamAWinStreak = 0; else TeamBWinStreak = 0;
}
void ABlasterGameState::ResetAllStreaks()
{
	TeamALossStreak = TeamBLossStreak = 0;
	TeamAWinStreak = TeamBWinStreak = 0;
}
void ABlasterGameState::AddRoundWin(ELogicalTeam T)
{
	if (T == ELogicalTeam::ELT_TeamA) TeamARoundWins++; else TeamBRoundWins++;
}
int32 ABlasterGameState::GetRoundWinsForTeam(ELogicalTeam T) const
{
	return (T == ELogicalTeam::ELT_TeamA) ? TeamARoundWins : TeamBRoundWins;
}

void ABlasterGameState::UpdateTopScore(ABlasterPlayerState *ScoringPlayer)
{
	if (ScoringPlayer == nullptr) return;

	// 关键：先把得分者从榜上移除，再用剩余玩家的分数作为"旧最高分"比较
	// 否则如果得分者已在榜上，TopScoringPlayers[0] 就是他自己的新分数，永远和自己比
	// 导致比他分低的旧并列者赖在榜上不被清除
	TopScoringPlayers.Remove(ScoringPlayer);

	if (TopScoringPlayers.Num() == 0)
	{
		// 榜上只剩他自己（或本来是空的），直接放回去
		TopScoringPlayers.Add(ScoringPlayer);
		return;
	}

	const float NewScore = ScoringPlayer->GetScore();
	// 取剩余玩家中任意一人的分数作为旧最高分（榜上所有人分数相同）
	const float CurrentTopScore = TopScoringPlayers[0]->GetScore();

	if (NewScore > CurrentTopScore)
	{
		// 打破旧纪录：清空旧榜，独占第一
		TopScoringPlayers.Empty();
		TopScoringPlayers.Add(ScoringPlayer);
	}
	else if (NewScore == CurrentTopScore)
	{
		// 追平纪录：加入并列
		TopScoringPlayers.AddUnique(ScoringPlayer);
	}
	// NewScore < CurrentTopScore：已被超越，不加入（Remove 已将其移除）
}

// ── 商店物品查询：遍历 DataTable 按 ItemID 匹配 ──
// 当前 20 行 O(N) 遍历无性能问题；后续 ≥100 行时可改为 TMap 缓存
const FShopItemRow* ABlasterGameState::FindShopItem(int32 ItemID) const
{
    if (!ShopItemTable) return nullptr;

    TArray<FShopItemRow*> AllRows;
    ShopItemTable->GetAllRows<FShopItemRow>(TEXT("ShopLookup"), AllRows);

    for (const FShopItemRow* Row : AllRows)
    {
        if (Row && Row->ItemID == ItemID)
        {
            return Row;
        }
    }
    return nullptr;
}

// ── 蓝图查询包装：返回 Price，-1 = 无效 ID ──
int32 ABlasterGameState::GetShopItemPrice(int32 ItemID) const
{
	// 从复制数组读（客户端 DS 安全，无需访问 DataTable 内存）
	for (const FShopItemPriceEntry& Entry : ShopItemPrices)
	{
		if (Entry.ItemID == ItemID) return Entry.Price;
	}
	return -1;
}

// 服务端：从 DataTable 提取 ID+Price 填充复制数组，触发客户端同步
void ABlasterGameState::SyncShopPrices()
{
	if (!HasAuthority() || !ShopItemTable) return;
	TArray<FShopItemRow*> AllRows;
	ShopItemTable->GetAllRows<FShopItemRow>(TEXT("ShopSync"), AllRows);
	ShopItemPrices.Reset();
	for (const FShopItemRow* Row : AllRows)
	{
		if (Row) ShopItemPrices.Add({Row->ItemID, Row->Price});
	}
}
