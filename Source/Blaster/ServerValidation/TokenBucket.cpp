// FBlasterTokenBucket 实现：令牌桶（Token Bucket）限频算法
//
//
// ── 时间基准 ─────────────────────────────────────────
//   TryConsume 的时间由调用方传入服务器世界时间（GetWorld()->GetTimeSeconds()），
//   结构体不持有 UWorld，保持纯数据可复用、可单测。
//   LastRefillTime 记录"上次掉硬币的时刻"，-1 是哨兵值表示还没初始化。

#include "TokenBucket.h"

void FBlasterTokenBucket::Reset()
{
	// 重置为"初始满桶"状态：换武器 / 重生时调用，
	// 让新装备的武器获得完整的一次突发额度（而不是继承上一把枪的剩余令牌）。
	Tokens = Capacity;
	// 基准时间复位为 -1 → 下次 TryConsume 会走"首次使用"分支重新建基准。
	LastRefillTime = -1.f;
}

bool FBlasterTokenBucket::TryConsume(float ServerTimeNow)
{
	// ── 阶段一 · 补发：把"上次掉硬币到现在"落下的硬币加进桶里 ──
	if (LastRefillTime < 0.f)
	{
		// 首次使用：只把当前时刻记为基准、桶回满，然后进入下方消费。
		// 为什么必须单独处理？若按 else 分支算，Elapsed = now - (-1) 会是一个
		// 巨大的数值，把"开局前发呆的几十秒"错误折算成海量令牌——
		// 玩家在热身区挂机 60 秒，不等于他攒了 60×15=900 发的额度。
		LastRefillTime = ServerTimeNow;
		Tokens = Capacity;
	}
	else
	{
		// 正常情况：经过的秒数 × 掉落速率 = 新增令牌数
		// Elapsed为服务器的时间流逝量，可能是小数（0.0167s/帧），所以 Tokens 也可能是小数。
		const float Elapsed = FMath::Max(0.f, ServerTimeNow - LastRefillTime);
		if (Elapsed > 0.f)
		{
			// 令牌桶补发：按速率掉落硬币，最多补满桶容量
			Tokens = FMath::Min(Capacity, Tokens + Elapsed * RefillRatePerSecond);
			LastRefillTime = ServerTimeNow;
		}
	}

	// ── 阶段二 · 消费：桶里有令牌就取 1 个放行，没有就拒绝 ──
	// 用 1.f - KINDA_SMALL_NUMBER 而非 1.f：
	// 补发经过浮点运算后 Tokens 可能是 0.9999999（本应=1），
	// 直接 >= 1.f 判断会误拒绝合法请求；微小容差消除这个浮点边界误差。
	if (Tokens >= 1.f - KINDA_SMALL_NUMBER)
	{
		Tokens -= 1.f;
		return true;
	}
	return false;
}
