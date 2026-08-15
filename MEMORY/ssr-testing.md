# SSR 量化测试方法（按需加载）

> 本文件从 `AGENTS.md` 拆出，仅在做 SSR（Server-Side Rewind）延迟补偿测试时按需读取。

**类型**：Project

SSR 延迟补偿的量化测试方法与判据，以及一个已删除的废弃方案。

## 验证三要素（缺一不可，来自代码注释 SSR_RewindManager.cpp:366）

1. **移动目标**：静止目标时回退帧=当前帧，SSR 开/关结果相同测不出差别 → 日志 `disp>0cm`。
2. **射手有延迟**：SSR 补偿射手→服务器的单向延迟；LAN 纯延迟只有几 ms 无意义 → 射手客户端控制台 `Net PktLag=120` 注入（≈140ms RTT），日志 `Delay≈70~140ms`。
3. **射手直瞄移动目标**（不打提前量）→ 回退帧命中、当前帧落空。

**判据**：服务器日志每枪一行 `[SSR] 分析 | ... rewindHit=true | currentHit=false | ... disp>0cm` = **"SSR 挽回命中"**（没有 SSR 就是"打中了却没伤害"）。对照实验：服务器 `ssr.Enabled 0` 后同操作，命中率应骤降。

## 废弃方案（已删除）

自动横移驱动 `SSRStrafeDriver`（`blaster.Test.StrafeTarget` 控制台命令，UWorldSubsystem 服务器驱动目标横移）是废弃方案，2026-08-13 已从 `Source/Blaster/SSR/` 删除（未提交，git 历史无记录）。SSR 测试一律用**两真人**：目标玩家手动左右横移 + 射手注入延迟。

## 相关 CVar（服务器运行时）

`ssr.Enabled`(1)、`ssr.AnalysisLog`(1 每枪输出对比日志)、`ssr.MaxPingCompensation`(0.25s)、`ssr.DrawDebug`(0)。

**为什么**：SSR 的价值只有在"高延迟射手 + 移动目标"场景下才可量化；自动化驱动已废弃，避免再被当成现行工具。

**如何应用**：双机测试时射手=注入 `Net PktLag=120` 的客户端，目标=另一台真人手动横移；量化数据收集服务器日志 `[SSR] 分析` 行（曾写 CSV 于 `Saved/Logs/SSR_Test_Results.csv`）。

## 延迟注入方向

- **上行（射手→服务器）**：射手**客户端**控制台 `Net PktLag=120`，SSR 补偿的就是这段。
- **下行（服务器→客户端）**：服务器启动 `-ExecCmds` 注入 `blaster.NetLagSim.*`，与上行方向相反。
- 见「3 窗口 DS 测试启动指令」。
