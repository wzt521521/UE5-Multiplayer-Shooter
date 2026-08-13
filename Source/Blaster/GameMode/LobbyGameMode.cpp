// Fill out your copyright notice in the Description page of Project Settings.


#include "LobbyGameMode.h"
#include "OnlineSubsystem.h"
#include "OnlineSessionSettings.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"       // GetPlayerName
#include "GameFramework/HUD.h"               // InitGame 里打印 HUDClass 诊断需要完整 AHUD 类型
#include "Blaster/BlasterTypes/MatchState.h" // LeavingMap 常量
#include "Blaster/PlayerState/BlasterPlayerState.h"         // 无缝切图要求：大厅就用 Blaster 的 PS 类
#include "Blaster/GameState/BlasterGameState.h"             // 同上：GameState 类保持一致
#include "Blaster/PlayerController/BlasterPlayerController.h" // 同上：PC 类保持一致
#include "Blaster/Session/SessionManagerSubsystem.h"         // P6 会话：登录签发 token 并下发

DEFINE_LOG_CATEGORY(LogLobby);
DEFINE_LOG_CATEGORY(LogServerSession);

// ===== LIFECYCLE =====

ALobbyGameMode::ALobbyGameMode()
{
    UE_LOG(LogLobby, Log, TEXT("[LobbyGameMode] Constructor — CDO created, bUseSeamlessTravel=%d, AimPeople=%d, GameMapPath=%s"),
        bUseSeamlessTravel, AimPeople, *GameMapPath);

    // ── 无缝切图关键：核心类必须与目标地图(BombDefusalGameMode)一致 ──
    // 无缝切图会把玩家的 PlayerState/Controller 原样带到下一张地图（不按新 GameMode 重建）。
    // 若大厅用默认 APlayerState，Bomb 地图 GetActivePlayers() 的 Cast<ABlasterPlayerState> 会失败
    // → 状态机卡在 WaitingToStart → 不生成角色。这里统一三个核心类。
    PlayerStateClass = ABlasterPlayerState::StaticClass();
    GameStateClass = ABlasterGameState::StaticClass();
    PlayerControllerClass = ABlasterPlayerController::StaticClass();

    // 绑定 DS 端建会话的异步回调
    ServerCreateSessionCompleteDelegate =
        FOnCreateSessionCompleteDelegate::CreateUObject(this, &ALobbyGameMode::OnServerSessionCreated);
}

void ALobbyGameMode::BeginPlay()
{
    Super::BeginPlay();

    // 只有真正的 Dedicated Server 进程才需要建会话（PIE/客户端不建）
    if (GetNetMode() == NM_DedicatedServer)
    {
        CreateServerSession();
    }
    else
    {
        UE_LOG(LogServerSession, Log,
            TEXT("[ServerSession] NetMode=%d（非 DedicatedServer），跳过创建会话"),
            (int32)GetNetMode());
    }
}

// ===== DS 端会话创建 =====
void ALobbyGameMode::CreateServerSession()
{
    IOnlineSubsystem* OSS = IOnlineSubsystem::Get();
    if (!OSS)
    {
        UE_LOG(LogServerSession, Error,
            TEXT("[ServerSession] CreateSession ABORT | OnlineSubsystem 为 null（Steam 未运行？已回退 NULL）"));
        return;
    }

    IOnlineSessionPtr SessionInterface = OSS->GetSessionInterface();
    if (!SessionInterface.IsValid())
    {
        UE_LOG(LogServerSession, Error,
            TEXT("[ServerSession] CreateSession ABORT | SessionInterface 无效"));
        return;
    }

    const FString OSSName = OSS->GetSubsystemName().ToString();
    UE_LOG(LogServerSession, Warning,
        TEXT("[ServerSession] CreateSession ENTER | OSS=%s | 当前为 Dedicated Server"),
        *OSSName);

    // 已有同名会话先销毁，避免重进 Lobby 时重复创建
    if (SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
    {
        SessionInterface->DestroySession(NAME_GameSession);
    }

    // 注册异步回调：创建完成后进入 OnServerSessionCreated
    ServerCreateSessionCompleteDelegateHandle =
        SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(ServerCreateSessionCompleteDelegate);

    // 会话设置：LAN 会话（bIsLANMatch=true），同一台机器上 Steam/NULL 客户端都能用 LAN 发现
    TSharedRef<FOnlineSessionSettings> Settings = MakeShared<FOnlineSessionSettings>();
    const bool bIsNULL = (OSSName == TEXT("NULL"));
    Settings->bIsLANMatch = true;              // LAN 会话 → 启动 LAN beacon（Steam 走 CreateLANSession）
    Settings->bIsDedicated = true;             // 标记为 Dedicated Server
    Settings->NumPublicConnections = 4;
    Settings->bAllowJoinInProgress = true;
    Settings->bAllowJoinViaPresence = true;
    Settings->bShouldAdvertise = true;         // 必须 true，否则不启动 LAN beacon
    Settings->bUsesPresence = false;           // 服务器不参与 Presence 搜索
    Settings->bUseLobbiesIfAvailable = false;  // DS → 游戏服务器/LAN，不是 Steam Lobby
    Settings->BuildUniqueId = 1;
    Settings->Set(FName("MatchType"), FString(TEXT("FreeForAll")), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

    UE_LOG(LogServerSession, Warning,
        TEXT("[ServerSession] Settings: OSS=%s | bIsLAN=1 | bIsDedicated=1 | NumPublic=%d | MatchType=FreeForAll | bAdvertise=1 | bNULL=%d"),
        *OSSName, Settings->NumPublicConnections, bIsNULL);

    // 用 HostingPlayerNum 重载（0），不需要 LocalPlayer —— DS 上本就没有本地玩家
    // 注意：*Settings 显式解引用 TSharedRef，避免重载决议时无法转换
    const bool bResult = SessionInterface->CreateSession(0, NAME_GameSession, *Settings);
    if (!bResult)
    {
        UE_LOG(LogServerSession, Error,
            TEXT("[ServerSession] CreateSession FAILED | 同步调用返回 false"));
        SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(ServerCreateSessionCompleteDelegateHandle);
    }
    else
    {
        UE_LOG(LogServerSession, Warning,
            TEXT("[ServerSession] CreateSession | 请求已发出，等待回调..."));
    }
}

void ALobbyGameMode::OnServerSessionCreated(FName SessionName, bool bWasSuccessful)
{
    if (IOnlineSubsystem* OSS = IOnlineSubsystem::Get())
    {
        OSS->GetSessionInterface()->ClearOnCreateSessionCompleteDelegate_Handle(ServerCreateSessionCompleteDelegateHandle);
    }
    UE_LOG(LogServerSession, Warning,
        TEXT("[ServerSession] OnCreateSessionComplete CALLBACK | SessionName=%s | Success=%d"),
        *SessionName.ToString(), bWasSuccessful);

    // 诊断：确认建成会话的 MatchType / NumOpen，便于和客户端收到的一致比对
    if (bWasSuccessful)
    {
        if (IOnlineSubsystem* OSS = IOnlineSubsystem::Get())
        {
            FNamedOnlineSession* Sess = OSS->GetSessionInterface()->GetNamedSession(SessionName);
            if (Sess)
            {
                FString MT;
                Sess->SessionSettings.Get(FName("MatchType"), MT);
                UE_LOG(LogServerSession, Warning,
                    TEXT("[ServerSession] 确认会话 | Owner=%s | MatchType=%s | NumOpen=%d/%d | bIsLAN=%d | bAdvertise=%d"),
                    *Sess->OwningUserName,
                    *MT,
                    Sess->NumOpenPublicConnections,
                    Sess->SessionSettings.NumPublicConnections,
                    Sess->SessionSettings.bIsLANMatch,
                    Sess->SessionSettings.bShouldAdvertise);
            }
        }
    }
}

void ALobbyGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);

    // 打印 InitGame 上下文：谁在什么网络中用什么地图启动了
    const EWorldType::Type WT = GetWorld() ? GetWorld()->WorldType : EWorldType::None;
    const ENetMode NM = GetNetMode();
    UE_LOG(LogLobby, Warning,
        TEXT("[LobbyGameMode] InitGame | Map=%s | WorldType=%d | NetMode=%d | Options=%s | Error=%s"),
        *MapName, (int32)WT, (int32)NM, *Options, *ErrorMessage);

    // 诊断：确认 GameMode 的 HUDClass（服务器通过 ClientSetHUD 传给客户端的 HUD 类）
    UE_LOG(LogLobby, Warning,
        TEXT("[LobbyGameMode] InitGame → HUDClass=%s"),
        HUDClass ? *HUDClass->GetName() : TEXT("NULL"));

    // PIE 单进程下引擎禁止无缝切换（见 AGameModeBase::CanServerTravel），
    // 仅在非 PIE（打包/独立服务器）下启用
    if (GetWorld() && GetWorld()->WorldType != EWorldType::PIE)
    {
        bUseSeamlessTravel = true;
    }
    else
    {
        // 显式关闭：蓝图子类可能勾选了 bUseSeamlessTravel，PIE 下必须强制关掉
        bUseSeamlessTravel = false;
    }

    UE_LOG(LogLobby, Warning,
        TEXT("[LobbyGameMode] InitGame → bUseSeamlessTravel = %d (WorldType=%d)"),
        bUseSeamlessTravel, (int32)WT);
}

// ===== PLAYER JOIN / TRAVEL =====

void ALobbyGameMode::PostLogin(APlayerController *NewPlayer)
{
    Super::PostLogin(NewPlayer);

    // 打印当前玩家：谁加入了
    const FString PlayerName = NewPlayer->PlayerState
        ? NewPlayer->PlayerState->GetPlayerName() : TEXT("Unknown");
    UE_LOG(LogLobby, Warning,
        TEXT("[LobbyGameMode] PostLogin | Player=%s | MatchState=%s"),
        *PlayerName, *MatchState.ToString());

    // ── ：签发 token 并下发客户端（重连凭证）──
    // 每个经过大厅登录的玩家都拿到一个全局唯一 token；客户端落盘保存，
    // 断线重连时出示同一 token 让服务器在待重连表中定位留场状态（P3）。
    if (UBlasterSessionManager* SessionMgr = UBlasterSessionManager::Get())
    {
        const FString Token = SessionMgr->IssueToken(NewPlayer);// 服务器生成 token，写进 PS->SessionToken
        if (ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(NewPlayer))
        {
            BPC->ClientReceiveSessionToken(Token);
        }
    }

    if (!GameState)
    {
        UE_LOG(LogLobby, Error, TEXT("[LobbyGameMode] PostLogin → ABORT: GameState is null!"));
        return;
    }

    // 防重入：ServerTravel 是帧末异步执行，若不切状态，后续 PostLogin/Tick 会重复调用
    if (MatchState == MatchState::LeavingMap)
    {
        UE_LOG(LogLobby, Warning,
            TEXT("[LobbyGameMode] PostLogin → IGNORE: Already in LeavingMap, travel in progress"));
        return;
    }

    // 当前大厅人数
    const int32 NumOfPlayers = GameState->PlayerArray.Num();

    // 大厅游戏状态
    const bool bEnoughPlayers = NumOfPlayers >= AimPeople;
    const FString LobbyState = bEnoughPlayers
        ? TEXT("ReadyToTravel")
        : TEXT("WaitingForPlayers");

    // 控制台打印：大厅人数 + 大厅状态（用 LogLobby 标签筛选：LogLobby）
    UE_LOG(LogLobby, Warning,
        TEXT("[LobbyGameMode] PostLogin | People=%d / Aim=%d | LobbyState=%s"),
        NumOfPlayers, AimPeople, *LobbyState);

    if (bEnoughPlayers)
    {
        UWorld* World = GetWorld();
        if (!World)
        {
            UE_LOG(LogLobby, Error, TEXT("[LobbyGameMode] PostLogin → ABORT: GetWorld() is null!"));
            return;
        }

        // 打印 ServerTravel 前的完整快照
        UE_LOG(LogLobby, Warning,
            TEXT("[LobbyGameMode] Pre-ServerTravel SNAPSHOT | WorldType=%d | NetMode=%d | bUseSeamlessTravel=%d | MapPath=%s | MatchState=%s"),
            (int32)World->WorldType, (int32)GetNetMode(),
            bUseSeamlessTravel, *GameMapPath, *MatchState.ToString());

        // 先切到 LeavingMap 防重入（与 BombDefusalGameMode::ReturnToLobby 一致）
        SetMatchState(MatchState::LeavingMap);
        UE_LOG(LogLobby, Warning,
            TEXT("[LobbyGameMode] PostLogin → SetMatchState(LeavingMap), calling ServerTravel..."));

        // ServerTravel 返回 false 表示失败（地图不存在/路径错误等）
        const bool bTravelSuccess = World->ServerTravel(GameMapPath);
        if (bTravelSuccess)
        {
            UE_LOG(LogLobby, Warning,
                TEXT("[LobbyGameMode] PostLogin → ServerTravel SUCCESS, travelling..."));
        }
        else
        {
            // 严重错误：打印全部诊断信息
            UE_LOG(LogLobby, Error,
                TEXT("[LobbyGameMode] PostLogin → ServerTravel FAILED!"));
            UE_LOG(LogLobby, Error,
                TEXT("  ↳ MapPath: %s"), *GameMapPath);
            UE_LOG(LogLobby, Error,
                TEXT("  ↳ WorldType: %d | NetMode: %d | bUseSeamlessTravel: %d"),
                (int32)World->WorldType, (int32)GetNetMode(), bUseSeamlessTravel);
            UE_LOG(LogLobby, Error,
                TEXT("  ↳ MatchState: %s | PlayerArray.Num: %d"),
                *MatchState.ToString(), GameState->PlayerArray.Num());
        }
    }
}

// ===== LOGOUT / LEAVE =====
// P3 主流方案：大厅断开玩家不注册待重连表（未参赛，重连按新玩家/中途加入处理），引擎默认 Logout 即可。
