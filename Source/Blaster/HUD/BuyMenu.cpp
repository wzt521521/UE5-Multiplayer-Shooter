#include "BuyMenu.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Components/TextBlock.h"

void UBuyMenu::NativeConstruct()
{
    Super::NativeConstruct();

    // ── 绑定 Money 变化委托 ──
    if (ABlasterPlayerState* PS = GetOwningPlayerState<ABlasterPlayerState>())
    {
        PS->OnMoneyChanged.AddDynamic(this, &UBuyMenu::OnMoneyChangedHandler);

        // ── 初始刷新（bug 修复，同 CharacterOverlay）──
        // 打开商店时初始金钱的复制早已到达、OnRep_Money 广播在绑定前已错过，
        // 主动读一次当前值，否则 MoneyText 停在默认值直到下一笔经济变动。
        OnMoneyChangedHandler(PS->Money, 0);
    }

    // ── 武器按钮绑定 ──
    if (Button_AR)              Button_AR->OnClicked.AddDynamic(this, &UBuyMenu::OnARClicked);
    if (Button_SMG)             Button_SMG->OnClicked.AddDynamic(this, &UBuyMenu::OnSMGClicked);
    if (Button_Rocket)          Button_Rocket->OnClicked.AddDynamic(this, &UBuyMenu::OnRocketClicked);
    if (Button_Pistol)          Button_Pistol->OnClicked.AddDynamic(this, &UBuyMenu::OnPistolClicked);
    if (Button_Sniper)          Button_Sniper->OnClicked.AddDynamic(this, &UBuyMenu::OnSniperClicked);
    if (Button_Shotgun)         Button_Shotgun->OnClicked.AddDynamic(this, &UBuyMenu::OnShotgunClicked);
    if (Button_GrenadeLauncher) Button_GrenadeLauncher->OnClicked.AddDynamic(this, &UBuyMenu::OnGrenadeLauncherClicked);

    // ── 弹药按钮绑定 ──
    if (Button_AR_Ammo)              Button_AR_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnARAmmoClicked);
    if (Button_SMG_Ammo)             Button_SMG_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnSMGAmmoClicked);
    if (Button_Rocket_Ammo)          Button_Rocket_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnRocketAmmoClicked);
    if (Button_Pistol_Ammo)          Button_Pistol_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnPistolAmmoClicked);
    if (Button_Sniper_Ammo)          Button_Sniper_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnSniperAmmoClicked);
    if (Button_Shotgun_Ammo)         Button_Shotgun_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnShotgunAmmoClicked);
    if (Button_GrenadeLauncher_Ammo) Button_GrenadeLauncher_Ammo->OnClicked.AddDynamic(this, &UBuyMenu::OnGrenadeLauncherAmmoClicked);

    // ── 投掷物按钮绑定 ──
    if (Button_Frag)  Button_Frag->OnClicked.AddDynamic(this, &UBuyMenu::OnFragClicked);
    if (Button_Flash) Button_Flash->OnClicked.AddDynamic(this, &UBuyMenu::OnFlashClicked);
    if (Button_Smoke) Button_Smoke->OnClicked.AddDynamic(this, &UBuyMenu::OnSmokeClicked);

    // ── Buff 按钮绑定 ──
    if (Button_Speed)  Button_Speed->OnClicked.AddDynamic(this, &UBuyMenu::OnSpeedClicked);
    if (Button_Jump)   Button_Jump->OnClicked.AddDynamic(this, &UBuyMenu::OnJumpClicked);
    if (Button_Shield) Button_Shield->OnClicked.AddDynamic(this, &UBuyMenu::OnShieldClicked);
    if (Button_Heal)   Button_Heal->OnClicked.AddDynamic(this, &UBuyMenu::OnHealClicked);
}

void UBuyMenu::NativeDestruct()
{
    // 解绑委托：防止 Widget 销毁后 PlayerState 持有野指针
    if (ABlasterPlayerState* PS = GetOwningPlayerState<ABlasterPlayerState>())
    {
        PS->OnMoneyChanged.RemoveDynamic(this, &UBuyMenu::OnMoneyChangedHandler);
    }
    Super::NativeDestruct();
}

int32 UBuyMenu::GetPlayerMoney() const
{
    if (ABlasterPlayerState* PS = GetOwningPlayerState<ABlasterPlayerState>())
    {
        return PS->Money;
    }
    return 0;
}

UDataTable* UBuyMenu::GetShopItemTable() const
{
    if (ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>())
    {
        return GS->ShopItemTable;
    }
    return nullptr;
}

void UBuyMenu::RequestPurchase(int32 ItemID)
{
    if (ABlasterPlayerController* PC = GetOwningPlayer<ABlasterPlayerController>())
    {
        PC->ServerRequestPurchase(ItemID);
    }
}

void UBuyMenu::OnMoneyChangedHandler(int32 NewMoney, int32 Delta)
{
    if (MoneyText) MoneyText->SetText(FText::AsNumber(NewMoney));
}

void UBuyMenu::TryPurchase(int32 ItemID)
{
    // 客户端查表取价格（DataTable 客户端可读）
    int32 Price = 0;
    if (ABlasterGameState* GS = GetWorld()->GetGameState<ABlasterGameState>())
    {
        Price = GS->GetShopItemPrice(ItemID);
    }

    if (Price <= 0) return;  // 无效 ItemID

    if (GetPlayerMoney() < Price)
    {
        ShowInsufficientFundsWarning();
        return;
    }

    RequestPurchase(ItemID);
}

void UBuyMenu::ShowInsufficientFundsWarning()
{
    if (!InsufficientFundsText) return;

    InsufficientFundsText->SetVisibility(ESlateVisibility::Visible);

    // 2 秒后自动隐藏
    FTimerHandle HideTimer;
    GetWorld()->GetTimerManager().SetTimer(
        HideTimer,
        FTimerDelegate::CreateWeakLambda(this, [this]()
        {
            if (InsufficientFundsText)
            {
                InsufficientFundsText->SetVisibility(ESlateVisibility::Hidden);
            }
        }),
        2.f, false
    );
}
