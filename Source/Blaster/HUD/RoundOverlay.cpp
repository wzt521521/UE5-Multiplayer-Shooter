#include "RoundOverlay.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Components/TextBlock.h"

void URoundOverlay::NativeConstruct()
{
	Super::NativeConstruct();

	// 绑定 GameState 委托：数据变化时自动刷新，不再需要 PC 轮询
	if (ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>())
	{
		GS->OnAliveCountChanged.AddDynamic(this, &URoundOverlay::RefreshAliveCount);
		GS->OnRoundInfoChanged.AddDynamic(this, &URoundOverlay::RefreshRoundInfo);
		GS->OnRoundResultChanged.AddDynamic(this, &URoundOverlay::RefreshRoundResult);
		GS->OnMatchResultChanged.AddDynamic(this, &URoundOverlay::RefreshMatchResult);

		// 初始刷新：委托只会在数据变化时触发，不会推送历史数据。
		// 如果 GameState 数据在 NativeConstruct 之前就已到达（客户端复制早于 Widget 创建），
		// 需要主动读取一次当前值来初始化所有控件。
		RefreshAliveCount(GS->AttackerAliveCount, GS->DefenderAliveCount);
		RefreshRoundInfo(GS->CurrentRoundNumber, GS->TeamARoundWins, GS->TeamBRoundWins);
		RefreshRoundResult(GS->LastRoundWinner, GS->TeamARoundWins, GS->TeamBRoundWins);
		RefreshMatchResult(GS->LastMatchWinner, GS->TeamARoundWins, GS->TeamBRoundWins);
		// TeamLabel 从本地 PlayerState 读取，不在 GameState 委托参数中，需独立刷新
		RefreshTeamLabel();
	}
}

void URoundOverlay::NativeDestruct()
{
	// 解绑委托：防止 Widget 销毁后 GameState 持有野指针
	if (ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>())
	{
		GS->OnAliveCountChanged.RemoveDynamic(this, &URoundOverlay::RefreshAliveCount);
		GS->OnRoundInfoChanged.RemoveDynamic(this, &URoundOverlay::RefreshRoundInfo);
		GS->OnRoundResultChanged.RemoveDynamic(this, &URoundOverlay::RefreshRoundResult);
		GS->OnMatchResultChanged.RemoveDynamic(this, &URoundOverlay::RefreshMatchResult);
	}

	Super::NativeDestruct();
}

void URoundOverlay::RefreshAliveCount(int32 AttackerAlive, int32 DefenderAlive)
{
	if (AttackerAliveText)  AttackerAliveText->SetText(FText::AsNumber(AttackerAlive));
	if (DefenderAliveText)  DefenderAliveText->SetText(FText::AsNumber(DefenderAlive));
}

void URoundOverlay::RefreshRoundInfo(int32 RoundNumber, int32 TeamAWins, int32 TeamBWins)
{
	// RoundNumber ≤ 0 说明 GameState 数据尚未同步（初始默认值），
	// 不更新 RoundNumberText，避免显示"第0回合"——等委托推送真实值。
	// 同时防止 RefreshRoundResult / RefreshMatchResult 中调用 RefreshRoundInfo(0, ...)
	// 时覆盖已设置的胜者文本。
	if (RoundNumberText && RoundNumber > 0)
	{
		RoundNumberText->SetText(
			FText::FromString(FString::Printf(TEXT("第%d回合"), RoundNumber)));  // 第X回合
	}
	// ── 半场交换修正（bug 修复）──
	// 比分按 LogicalTeam 记录（TeamA/TeamB 跟随玩家跨半场不互换，EndRound→AddRoundWin 按逻辑队递增），
	// 但 ScoreText 的标签是角色维度"攻击者/保卫者"，而角色映射随半场翻转：
	// 上半场 攻击者=TeamA；下半场（bIsSecondHalf）攻击者=TeamB（与 GameMode::GetLogicalTeamFromRole 同款映射）。
	// 修复前 ScoreText 无条件用 (TeamAWins, TeamBWins) 对位 (攻击者, 保卫者)，
	// 下半场会把守方比分显示在"攻击者"名下——实测 bug。
	if (ScoreText)
	{
		const ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr;
		const bool bSecondHalf = GS ? GS->bIsSecondHalf : false;
		const int32 AttackerWins = bSecondHalf ? TeamBWins : TeamAWins;
		const int32 DefenderWins = bSecondHalf ? TeamAWins : TeamBWins;
		ScoreText->SetText(FText::FromString(
			FString::Printf(TEXT("攻击者 %d - %d 保卫者"), AttackerWins, DefenderWins)));
	}
	// 同时刷新阵营标签：回合准备阶段 TeamID 已分配，确保 TeamLabel 与最新阵营同步
	RefreshTeamLabel();
}

void URoundOverlay::RefreshRoundResult(ETeamID Winner, int32 TeamAWins, int32 TeamBWins)
{
	// ETI_None 守卫：尚未有任何回合结束时跳过，避免 else 分支误显示"保卫者胜"
	if (Winner == ETeamID::ETI_None) return;

	// 回合结束时：胜者显示在 RoundNumberText，比分刷新
	if (RoundNumberText)
	{
		const FString WinnerStr = (Winner == ETeamID::ETI_Attacker)
			? TEXT("攻击者胜") : TEXT("保卫者胜");  // 攻击者胜 / 保卫者胜
		RoundNumberText->SetText(FText::FromString(WinnerStr));
	}
	// 同时刷新比分
	RefreshRoundInfo(0, TeamAWins, TeamBWins);
}

void URoundOverlay::RefreshMatchResult(ETeamID Winner, int32 TeamAWins, int32 TeamBWins)
{
	// ETI_None 守卫：尚未有任何比赛结束时跳过，避免 else 分支误显示"保卫者赢得比赛!"
	if (Winner == ETeamID::ETI_None) return;

	if (RoundNumberText)
	{
		const FString WinnerStr = (Winner == ETeamID::ETI_Attacker)
			? TEXT("攻击者赢得比赛!")   // 攻击者赢得比赛!
			: TEXT("保卫者赢得比赛!");  // 保卫者赢得比赛!
		RoundNumberText->SetText(FText::FromString(WinnerStr));
	}
	// 最终比分
	RefreshRoundInfo(0, TeamAWins, TeamBWins);
}

// ------------------------------------------------------------
// 从本地 PlayerState 读取阵营标识更新 TeamLabel
// 阵营归属是每玩家数据（PlayerState::TeamID），不在 GameState 全局委托参数中
// ------------------------------------------------------------
void URoundOverlay::RefreshTeamLabel()
{
	if (!TeamLabel) return;
	if (ABlasterPlayerState* PS = GetOwningPlayer()->GetPlayerState<ABlasterPlayerState>())
	{
		const FString TeamStr = (PS->TeamID == ETeamID::ETI_Attacker)
			? TEXT("攻击者") : TEXT("保卫者");  // 攻击者 / 保卫者
		TeamLabel->SetText(FText::FromString(TeamStr));
	}
}
