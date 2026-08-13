// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "LobbyGameMode.generated.h"

// 大厅日志分类，控制台用 LogLobby 标签筛选
DECLARE_LOG_CATEGORY_EXTERN(LogLobby, Log, All);
// DS 端会话创建日志，控制台用 LogServerSession 标签筛选
DECLARE_LOG_CATEGORY_EXTERN(LogServerSession, Log, All);

/**
 *
 */
UCLASS()
class BLASTER_API ALobbyGameMode : public AGameMode
{
	GENERATED_BODY()
public:
	ALobbyGameMode();
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void BeginPlay() override;

protected:
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

	// ── DS 端会话创建：仅 Dedicated Server 在 Lobby 启动时调用 ──
	// DS 进程无渲染、无 LocalPlayer，不能走插件 UMultiplayerSessionsSubsystem::CreateSession（它依赖 LocalPlayer）。
	// 这里直接用 IOnlineSession::CreateSession(HostingPlayerNum) 重载建一个 LAN 会话（bIsLANMatch=true），
	// 客户端用同 OSS 的 FindSessions(LAN 模式) 即可发现并加入。
	void CreateServerSession();
	void OnServerSessionCreated(FName SessionName, bool bWasSuccessful);

	// 自动开局人数阈值（双机测试临时改为 2：两台电脑各 1 名玩家即可进入对局）
	UPROPERTY(EditDefaultsOnly, Category = "Lobby", meta = (ClampMin = "2"))
	int32 AimPeople = 2;

	// 目标游戏地图路径（ServerTravel 自动保持 Listen 模式，无需 ?listen）
	UPROPERTY(EditDefaultsOnly, Category = "Lobby")
	FString GameMapPath = TEXT("/Game/Maps/BombDefusalGameMode");

private:
	FOnCreateSessionCompleteDelegate ServerCreateSessionCompleteDelegate;
	FDelegateHandle ServerCreateSessionCompleteDelegateHandle;
};
