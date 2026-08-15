# Blaster 项目长期记忆

> 由 Claude Code 项目级记忆迁移而来（DSH 自动加载）。Claude 专属工具已适配为 Windows/DSH 等价做法。
> **精简约定**：本文件只放"常驻高频"内容（原则 + 高频命令 + 关键坑）；低频细节拆到 `MEMORY/` 目录，按需读取（见各条指针）。

## 索引

### 工作偏好（Feedback）

- [代码必须带核心注释](#代码必须带核心注释) —— 写实现代码必须附核心注释（WHY + HOW）
- [聚焦当下需要](#聚焦当下需要) —— 汇报只讲当下要做什么，不列举删除项
- [实时监听日志汇报](#实时监听日志汇报) —— 测试时实时监听日志、事件到达即简报
- [学习笔记分析方法](#学习笔记分析方法) —— 按需读 `MEMORY/study-notes-style.md`

### 项目技术事实（Project）

- [SSR 量化测试方法](#ssr-量化测试方法) —— 按需读 `MEMORY/ssr-testing.md`
- [Dedicated Server 架构](#dedicated-server-架构) —— 游戏以 DS 模式运行，服务器与客户端完全分离
- [3 窗口 DS 测试启动指令](#3-窗口-ds-测试启动指令) —— 服务器/客户端启动命令（含下行延迟注入）
- [无缝切图与联机排障的坑](#无缝切图与联机排障的坑) —— 核心类统一 / Development 编辑器 / HUD 兜底 / 断线重连
- [launcher 不能构建 Server](#launcher-不能构建-server) —— 预编译版 UE 5.0 不编 Server 目标，必须用源码版
- [简历项目核心价值](#简历项目核心价值) —— 任何需求先过四问，输出 ✅/⚠️/❌

### 参考信息（Reference）

- [日志位置与监听方法](#日志位置与监听方法) —— 服务器/客户端日志位置 + `Get-Content -Wait` 监控
- [UE 引擎路径与编译命令](#ue-引擎路径与编译命令) —— UE 5.0 路径 + Build.bat
- [简历素材目录](#简历素材目录) —— 按需读 `MEMORY/resume-materials.md`

---

## 代码必须带核心注释

**类型**：Feedback

写任何实现代码时必须加上对应的核心注释，内容包括两方面：

1. **逻辑意图（WHY）**：每段关键逻辑的目的和设计考量
2. **模块配合（HOW）**：该代码与哪些其他模块/函数/类如何交互，例如：
   - 该函数由谁调用（如某个 RPC、输入绑定、蓝图事件）
   - 该变量通过什么复制条件同步到哪些客户端
   - 该操作在服务器/客户端的执行路径有什么不同
   - 该修改会影响哪些 HUD/UI 更新链

不能只写代码不加注释。

**为什么**：用户明确要求代码可维护性和可读性，注释需要说明设计意图和模块间协作关系。

**如何应用**：每次写或修改 `.cpp`/`.h` 文件时，在关键逻辑块前添加简洁注释。不是逐行解释 WHAT，而是说明 WHY + 跟谁配合。

---

## 聚焦当下需要

**类型**：Feedback

呈现项目方向/计划/改造清单时，直接给"当下需要做什么 + 执行方向"，不要回顾性列举删除了哪些项或未选的备选。

**为什么**：用户关注的是下一步行动；"砍掉了什么"是过程叙事，对决策无帮助。

**如何应用**：交付物聚焦"要做的事 + 执行方向 + 优先级"；被排除的项只在用户明确询问时才说明。配合「简历项目核心价值」的优先级判断使用。

---

## 实时监听日志汇报

**类型**：Feedback

用户喜欢「测试时跟随运行 + 实时监听日志」的协作方式（P0/P1/P2 已验证有效，用户明确喜欢）。

**为什么**：用户在 3 窗口 DS 测试时希望我实时盯着服务器/客户端日志，事件一发生就汇报，而不是他截图/贴日志或我事后翻。

**如何应用**：
- 用户说"准备监听 / 即将测试 / 启动好了"时，立刻挂**后台 PowerShell 进程**实时监听（覆盖整场测试会话）：
  ```powershell
  Get-Content "<日志路径>" -Wait -Tail 0 | Select-String -Pattern "[关键词]"
  ```
  （在 DSH 中用后台任务启动，`job_output` 收流、`job_kill` 停止；服务器与各客户端日志各自挂一个后台任务。）
- **日志路径与关键词**：见「日志位置与监听方法」。
- 每个事件到达即**简报**（谁/发生了什么/是否符合预期），不批量囤积；异常立即指出根因方向。
- **同一日志文件只挂一个监听** —— 对同一文件重复挂会双推送；服务器与各客户端日志各自一个后台任务，互不冲突。重挂前先 `job_kill` 停掉旧任务。
- **⚠ 日志滚动后需重挂**：`Get-Content -Wait` 只在目标文件被持续追加时跟随，**不会**像 `tail -F` 那样自动跟随日志滚动。游戏重启/重新开局（日志换名或重建）后，需 `job_kill` 旧任务并重新挂监听。
- 分工：用户自己编译/打包/启动，我只负责**编码 + 监听 + 汇报**。

> 适配说明：原 Claude 版用 `Monitor` 工具 + `tail -F | grep`；DSH 在 Windows 上的等价物是后台 PowerShell `Get-Content -Wait`。

---

## 学习笔记分析方法

**类型**：Feedback（按需加载）

写学习笔记 / 架构分析 / 系统梳理时，先读 `MEMORY/study-notes-style.md`（架构优先、逐层展开、ASCII 图 + 角色总表 + 设计亮点的完整模板）。

---

## SSR 量化测试方法

**类型**：Project（按需加载）

做 SSR（Server-Side Rewind）延迟补偿测试前，先读 `MEMORY/ssr-testing.md`（验证三要素 + 判据 + CVar）。延迟注入的两个方向见「3 窗口 DS 测试启动指令」。

---

## Dedicated Server 架构

**类型**：Project

项目已从 Listen Server 改为 **Dedicated Server（专用服务器）** 模式。

**为什么**：用户确认当前游戏使用专用服务器运行——服务器进程不渲染画面、不控制角色，所有玩家（包括主机）都是纯客户端。

**如何应用**：
- 服务器上 `HasAuthority()` 为 true，但没有任何 `IsLocallyControlled()` 为 true 的玩家
- 所有玩家（包括开房者）都通过 RPC 与服务器通信，不存在"服务器玩家"的双重视角
- `SetHUDWeaponAmmo` 等在服务器上对任何玩家都无效——服务器端没有 HUD
- SSR 延迟补偿对所有玩家都生效（不再需要跳过 Listen Server 主机）
- 设计时只需考虑两种角色：服务器（权威端、无渲染）和客户端（有 HUD、有渲染）

**网络形态**：纯 UDP + 局域网联机，**Steam 已摘除**。

3 窗口联机测试的启动命令见「3 窗口 DS 测试启动指令」，无缝切图与断线重连排障见「无缝切图与联机排障的坑」。

---

## 3 窗口 DS 测试启动指令

**类型**：Project

3 窗口 DS 联机测试（1 服务器 + 2~3 客户端）的固定启动命令，**由用户本人执行**；用户询问启动命令时，直接照此给出。

### 启动方式（3 窗口）

- **窗口 1 服务器**（Development 编辑器起 DS）：
  - **基础命令**：
    ```
    "D:\UE_engineer\UE_5.0\Engine\Binaries\Win64\UnrealEditor.exe" "D:\UE_Projects\Blaster\Blaster.uproject" -server /Game/Maps/Lobby -log -port=7777
    ```
  - **带下行延迟注入（下行延迟模拟：服务器→客户端）**：
    ```
    "D:\UE_engineer\UE_5.0\Engine\Binaries\Win64\UnrealEditor.exe" "D:\UE_Projects\Blaster\Blaster.uproject" -server /Game/Maps/Lobby -log -port=7777 -ExecCmds="blaster.NetLagSim.Enabled 1,blaster.NetLagSim.PktLag 150,blaster.NetLagSim.Variance 30"
    ```
  - 参数说明：
    - `-server` = 以 Dedicated Server 模式启动
    - `/Game/Maps/Lobby` = 起始地图（Lobby，随后无缝切图到 Bomb）
    - `-port=7777` = 端口
    - `-ExecCmds=...` = 启动后注入**下行延迟**（`blaster.NetLagSim.Enabled 1` 开启、`PktLag 150` 延迟 150ms、`Variance 30` 抖动 30ms）
  - **必须用 Development 编辑器 `UnrealEditor.exe`**，不能用 `UnrealEditor-Win64-DebugGame.exe`——DebugGame 模块可能是旧代码，改了 C++ 没重编就白改。
- **窗口 2、3、4 客户端**（小窗便于查看；打包产物根是 `Build\Windows\`，启动器是根下 `Blaster.exe`，**不是** `Blaster\Blaster.exe`）：
  ```
  "D:\UE_Projects\Blaster\Build\Windows\Blaster.exe" -log -Windowed -ResX=800 -ResY=450 -userdir="D:/UE_Projects/Blaster/Build/Windows/ClientA"
  ```
  - **⚠ `-saveddir=` 在 UE 5.0 不存在**（实测被忽略，客户端全用共享默认 Saved，token 互相覆盖）。必须用 **`-userdir=X`**（全量重定向，ProjectSavedDir→`X/Saved/`）或 `-saveddirsuffix=X`（→`Saved_X/`）
  - **P6 断线重连必须给每个客户端独立 `-userdir`**（token 按 userdir 分离；重连时用同一 userdir 才能出示同一 token）
  - 菜单点 Join，或直连 `127.0.0.1:7777`；AimPeople=2，2 个客户端（2 名玩家）齐即可开局（2026-08 实测修正：此前误记为 3）

### 延迟注入的两个方向（易混淆，注意区分）

- **上行延迟（射手→服务器）**：射手**客户端**控制台 `Net PktLag=120`（≈140ms RTT），是 SSR 延迟补偿要补偿的那段，见「SSR 量化测试方法」。
- **下行延迟（服务器→客户端）**：服务器启动 `-ExecCmds` 注入 `blaster.NetLagSim.*`（如上），与上行方向相反。
- 两者方向不同、机制不同，按测试目的选择或叠加。

> 相关：UE 引擎路径、launcher 不能构建 Server、日志位置与监听方法。

---

## 无缝切图与联机排障的坑

**类型**：Project

3 窗口 DS 测试中积累的排障经验：无缝切图三大坑 + 断线重连的坑。

### 教训 1：无缝切图必须统一三个核心类

Lobby → Bomb 用 `bUseSeamlessTravel`，玩家的 PlayerState/Controller 是**原样带过去、不按新 GameMode 重建**。若 Lobby 的 GameMode 没设 `PlayerStateClass`（默认 APlayerState），Bomb 的 `GetActivePlayers()` 里 `Cast<ABlasterPlayerState>` 失败 → 返回 0 人 → 状态机卡死 → 不生成角色。

**修复**：Lobby 和 Bomb 的 GameMode 构造函数统一设 `PlayerStateClass = ABlasterPlayerState`、`GameStateClass = ABlasterGameState`、`PlayerControllerClass = ABlasterPlayerController`。

### 教训 2：客户端 HUD 消失 = ClientSetHUD RPC 在无缝切图时丢失

服务器 GameMode 的 HUDClass 是对的（BP_BlasterHUD），但 `ClientSetHUD` Client RPC 在切图时可能没送达客户端 → 客户端 `GetHUD()` 一直是默认 AHUD → `Cast<ABlasterHud>` 失败 → 无 HUD（客户端日志里 BP_BlasterHUD 出现 0 次）。

**修复**：客户端 `ABlasterPlayerController::EnsureBlasterHud()` 每帧兜底——若 `GetHUD()` 不是 ABlasterHud，`StaticLoadClass` 加载 `/Game/Blueprints/HUD/BP_BlasterHUD.BP_BlasterHUD_C` 并本地调 `ClientSetHUD` 生成（不依赖服务器 RPC）。成功日志：`[HUD] EnsureBlasterHud → 客户端兜底生成 BlasterHud 成功`。

### 教训 3：服务器是编辑器进程，改 C++ 必须重编对应的编辑器配置

服务器进程 = 编辑器进程，加载对应配置的模块 DLL。只重编了 Development 模块，再用 DebugGame 编辑器跑服务器 = 跑旧代码。

### P3 断线重连验证流程（2026-08-06 跑通）

- **完整闭环**：3 客户端独立 `-userdir` 开局 → 存活断开（关窗口）→ `[Reconnect] 玩家 X 断线（角色移除）| 断开时存活=1`（必须=1，验证 PawnPendingDestroy 时序）→ 下轮不重生、回合正常结算（AliveCount 修复验证）→ 重连（**同一 `-userdir` 命令重启**）→ `[Reconnect] 玩家 X 重连恢复` → 下轮自动重生。

### 坑 4：重连被当新玩家 = token 文件共享/被覆盖

症状特征：断开日志全对（`断开时存活=1`），但重连永远走 `ServerAuthenticateSession → 新玩家` 分支、签发新 token。根因：客户端没独立 userdir → 全读写同一个 `../../../Blaster/Saved/Session/SessionToken.txt` → 后加入者覆盖前者 token → 重连出示**别人的 token** → 待重连表查不到。**判定**：看客户端日志 `[Session] 客户端读取本地 token=XXX` 读的是不是自己的。

### 坑 5：重连时机 + 经济冻结语义

回合进行中重连 → 观战等下轮；回合间隙重连 → 立即重生。经济冻结只作用于「断开期间结算的回合」；重连后（即使观战）下一回合结束即恢复领取。

### 快速定位

- "不生成角色" → 查 PlayerState/GameState/PlayerController 三个类是否统一（教训 1）
- "无 HUD" → 查客户端 `EnsureBlasterHud` 兜底是否触发（教训 2）
- "改了 C++ 却无效果" → 确认用的是 Development 编辑器 + 已重编对应配置（教训 3）

> 相关：3 窗口 DS 测试启动指令、Dedicated Server 架构。

---

## launcher 不能构建 Server

**类型**：Project

本项目的 UE 引擎有两套安装：
- **launcher 预编译版**：`D:\UE_engineer\UE_5.0`
- **源码版**：`D:\UE_engineer\UE_5.0_Source`

**launcher 预编译版引擎（`D:\UE_engineer\UE_5.0`）不能构建 Dedicated Server 目标。** 用 UAT 打包 server（`BuildCookRun -server`）跑到 Build 步骤时，UBT 报错：

```
ERROR: Server targets are not currently supported from this engine distribution.
Took 0.9s to run UnrealBuildTool.exe, ExitCode=6
```

launcher 版能烘焙(Cook)、能编编辑器模块（Development Editor）、能编游戏目标（Blaster.exe），唯独**不编 Server 目标**（BlasterServer）。这是预编译版的硬性限制，不是配置问题——反复遇到过多次。

**为什么**：launcher 预编译版不包含构建 Server 目标所需的引擎二进制/平台支持；源码版引擎没有这个限制。

**如何应用**：
- 构建/打包 `BlasterServer`（Dedicated Server）必须走源码版引擎 `D:\UE_engineer\UE_5.0_Source`。
- 但源码版引擎的 AutomationTool（.NET 工具链）可能没装全（曾缺 fastJSON.dll、NuGet 还原不完整），打包前可能要先修；`-nocompileuat` 可尝试跳过 .NET 重编译直接用预编译版。
- 快速联机测试可绕过打包：用编辑器 `-server` 起 DS（编辑器认 `-server`），客户端用已打包的 Blaster.exe 连。

> 相关：UE 引擎路径、Dedicated Server 架构。

---

## 简历项目核心价值

**类型**：Project

Blaster 是一个**简历项目**（DS 权威架构多人射击），唯一目标是**为实习面试准备**，方向是**服务器开发/网络方向** 或 **游戏开发方向**。不是产品，不追求上架与游戏性打磨。

**为什么**：用户 2026-08 明确：找实习，项目只作简历填充和学习经历。项目核心（DS 权威架构 + SSR 延迟补偿 + 局域网联机）已是简历主叙事，后续改动应服务"简历谈资 + 面试深度"。

**如何应用**：对用户任何需求，先做四问再动手——

1. **加分吗**：能否成为简历亮点/面试谈资（权威服务器、网络同步、反作弊、性能/带宽优化、数据层）；纯运维/打包/美术收益低。
2. **冲突吗**：项目已定型 DS + 局域网 + 纯 UDP（Steam 已摘除，见「Dedicated Server 架构」），需求若推翻已验证链路须先指出冲突与代价。
3. **依赖吗**：需公网/云/Docker 才可落地演示的，标注依赖，并优先给"本机可跑可演示"替代。
4. **性价比**：对比成本与简历收益，低者给缩水版或延后建议。

输出格式：✅建议 / ⚠️有条件 / ❌不建议 + 理由 + 最小落地路径。默认不做：上架发布、真线上部署、游戏性/手感打磨、美术表现。

---

## 日志位置与监听方法

**类型**：Reference

服务器（Development 编辑器 `-server`）和打包客户端（`Blaster.exe`）的日志位置与监控方法。

### 服务器（UnrealEditor.exe -server）

- **实时**：`-log` 黑底控制台**只输出、不能敲命令**（实测主窗口类名 `ConsoleWindowClass`=普通 Win32 控制台；UE 5.0 的 `FWindowsConsoleOutputDevice` 无输入读取，引擎二进制也无 UnrealConsole 远程工具，Windows 端无 TCP 控制台监听）。之前"交互式可直接输命令"是错误记录。要执行服务器命令只能改代码钩子（如自动触发）或重启参数注入，黑窗敲不了。
- **文件**：`D:\UE_Projects\Blaster\Saved\Logs\Blaster.log`（滚动备份 `Blaster-backup-*.log`）

### 客户端（打包 Blaster.exe）

- **文件（默认）**：`D:\UE_Projects\Blaster\Build\Windows\Blaster\Saved\Logs\Blaster.log`
  - **多开自动编号**：`Blaster.log`、`Blaster_2.log`、`Blaster_3.log`…
- **文件（`-userdir=X`）**：重定向到 `X\Saved\Logs\`；建议绝对路径（如 `-userdir="D:/UE_Projects/Blaster/Build/Windows/ClientA"`）。`-userdir` 启动规则见「3 窗口 DS 测试启动指令」。
- **实时**：加 `-log` 有独立控制台

### 监控命令（PowerShell 另一窗口）

```powershell
Get-Content "路径" -Wait                                    # 实时跟随
Get-Content "路径" -Wait | Select-String -Pattern "Error|Fatal|Session|token"  # 过滤
```

### P0/P6 会话调试关键词

- 服务器日志：`[Session] 签发 token=...`（每个客户端一条，值应不同）；控制台命令 `BlasterDumpPendingSessions` 查待重连表
- 客户端日志：`[Session] 客户端已保存 token=...`

### P1 观战调试关键词

- 服务器日志：`[Spectate] 玩家 X 死亡 → 进入观战`
- 客户端日志（按序）：`[Spectate] 客户端进入观战状态（死亡镜头）` → ~3s 后尸体销毁 → `[Spectate] 观战锁定队友 X | Team=N`（有存活队友）或 `[Spectate] 无存活队友 → 自由飞行` → 箭头键 `[Spectate] 切换到队友 X` → 下轮 `[Spectate] 客户端退出观战（重生恢复第三人称）`

> 相关：3 窗口 DS 测试启动指令、UE 引擎路径。

---

## UE 引擎路径与编译命令

**类型**：Reference

UE 5.0 引擎安装路径和编译命令。

**引擎路径**：`D:\UE_engineer\UE_5.0`

**源码版引擎路径**（构建/打包 Dedicated Server 必须用它，launcher 版不编 Server 目标）：`D:\UE_engineer\UE_5.0_Source`，见「launcher 不能构建 Server」

**编译命令**（Development Editor）：

```bash
"D:/UE_engineer/UE_5.0/Engine/Build/BatchFiles/Build.bat" BlasterEditor Win64 Development "D:/UE_Projects/Blaster/Blaster.uproject" -WaitMutex
```

**项目 .uproject**：`D:\UE_Projects\Blaster\Blaster.uproject`
**项目模块名**：`Blaster`
**Build.cs 位置**：`Source/Blaster/Blaster.Build.cs`
**Target.cs**：`Source/BlasterEditor.Target.cs`（Editor），`Source/Blaster.Target.cs`（Game）

**Visual Studio**：2022，toolchain `D:\visualstudio\VC\Tools\MSVC\14.38.33130`
**Windows SDK**：10.0.22621.0

---

## 简历素材目录

**类型**：Reference（按需加载）

做简历时读 `MEMORY/resume-materials.md`（素材目录 + 「服务器/网络方向」主文档清单）。
