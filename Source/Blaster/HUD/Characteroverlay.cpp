#include "Characteroverlay.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Components/TextBlock.h"

void UCharacteroverlay::NativeConstruct()
{
	Super::NativeConstruct();

	// 绑定 Money 变化委托：回合结束发钱 / 购买扣款后自动刷新 HUD 金额显示
	if (ABlasterPlayerState* PS = GetOwningPlayerState<ABlasterPlayerState>())
	{
		PS->OnMoneyChanged.AddDynamic(this, &UCharacteroverlay::OnMoneyChangedHandler);

		// ── 初始刷新（bug 修复）──
		// 委托只在 Money 变化时触发、不推送历史值：初始金钱（AssignTeamsOnce 发 $200）
		// 的复制早于 HUD Widget 创建，绑定前 OnRep_Money 的广播已被错过，
		// 不主动读一次当前值，MoneyText 会停在默认值直到下一笔经济变动
		// （实测：要打完一把拿到回合经济后才显示正确金额）。
		OnMoneyChangedHandler(PS->Money, 0);
	}
}

void UCharacteroverlay::NativeDestruct()
{
	if (ABlasterPlayerState* PS = GetOwningPlayerState<ABlasterPlayerState>())
	{
		PS->OnMoneyChanged.RemoveDynamic(this, &UCharacteroverlay::OnMoneyChangedHandler);
	}

	Super::NativeDestruct();
}

void UCharacteroverlay::OnMoneyChangedHandler(int32 NewMoney, int32 Delta)
{
	if (MoneyText) MoneyText->SetText(FText::AsNumber(NewMoney));
}
