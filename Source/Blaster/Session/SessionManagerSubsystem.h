#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "Blaster/BlasterTypes/TeamTypes.h"     // ETeamID（待重连表快照字段）
#include "Blaster/BlasterTypes/EconomyTypes.h"  // ELogicalTeam（待重连表快照字段）
#include "SessionManagerSubsystem.generated.h"

class APlayerController;
class ABlasterPlayerState;
class FSubsystemCollectionBase;

/**
 * 断线重连 —— 会话管理引擎级门面（UEngineSubsystem）。
 *
 * 设计意图（WHY）：
 * - 选 UEngineSubsystem 而非 WorldSubsystem：会话状态必须跨 ServerTravel 存活。
 *   Lobby→Bomb→Lobby 的无缝切图会销毁世界/GameMode，只有引擎子系统在整进程存活，
 *   PendingSessions（待重连表）放这里才不会随地图切换丢失（与 P4 持久化同理）。
 * - 待重连表用 UPROPERTY 强引用：Logout 后断线玩家的 PlayerState/Pawn 失去 Controller
 *   这个引用链，若不在此处持有，会被 GC 回收，重连将无从恢复（P6 风险 3 的地基性预防）。
 * - IssueToken 必须幂等（见 P0 计划 2.5）：重连时 PS 若已带旧 token，绝不覆盖，
 *   否则待重连表的 key 与客户端认证出示的 token 对不上，下次断线状态错乱。
 *
 * 模块配合（HOW）：
 * - 签发：LobbyGameMode::PostLogin 调用 IssueToken()（P0）。
 * - 待重连表：Logout 注册 / ServerAuthenticateSession 查询（P3 填充与恢复）。
 * - 客户端本地 token：ClientReceiveSessionToken RPC → SaveLocalToken() 落盘；
 *   重连时 LoadLocalToken() 读取并随 ServerAuthenticateSession 出示（P0）。
 */

// 断线留场状态快照
USTRUCT()
struct FPendingSession
{
	GENERATED_BODY()

	// 断线玩家的 PlayerState —— UPROPERTY 强引用防 GC（Logout 后 Controller 消失，
	// PS 必须由本表持有才能继续吃经济/统计）。P3 恢复时换绑到新 Controller。
	UPROPERTY()
	TObjectPtr<ABlasterPlayerState> PlayerState;

	// 断线瞬间阵营快照（重连恢复时据此 Possess 角色 / 决定出生点）
	ETeamID TeamID = ETeamID::ETI_None;
	ELogicalTeam LogicalTeam = ELogicalTeam::ELT_None;
	int32 Money = 0;
	bool bInMatch = false;   // true=对局中断开
};

UCLASS()
class BLASTER_API UBlasterSessionManager : public UEngineSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	static UBlasterSessionManager* Get();

	// ── 签发 ──
	// 登录签发 token（Lobby PostLogin 调用）。幂等：PS 已有 token 则原样返回不覆盖（见 2.5）。
	FString IssueToken(APlayerController* PC);

	// ── 待重连表查询 ──
	// ServerAuthenticateSession 用：按 token 查断线留场条目，命中返回指针、未命中返回 nullptr。
	FPendingSession* FindPendingSession(const FString& Token);

	// P3：Logout 注册断线留场（强引用防 GC）。Token 为 PS->SessionToken（key）。
	void RegisterPendingSession(const FString& Token, FPendingSession&& Session);
	// P3：重连消费 / P4：ReturnToLobby 清空
	void RemovePendingSession(const FString& Token);
	void ClearPendingSessions();
	// P3：遍历待重连表并修改（游戏线程内部访问，非 const）
	TMap<FString, FPendingSession>& GetPendingSessions() { return PendingSessions; }

	// ── 客户端本地 token 存取（重连出示用）──
	// 客户端收到 ClientReceiveSessionToken 后落盘；重连时读取并随 ServerAuthenticateSession 出示。
	// 路径 Saved/Session/SessionToken.txt；同机多客户端测试用 -saveddir 分离 Saved 目录。
	static bool SaveLocalToken(const FString& Token);
	static FString LoadLocalToken();

private:
	// 待重连表：token → 断线留场状态。P0 只建表不填充（P3 Logout 时注册）。
	UPROPERTY()
	TMap<FString, FPendingSession> PendingSessions;
};
