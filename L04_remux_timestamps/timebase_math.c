/*
 * L04 时间基与时间戳：时间基换算的几个工具函数。
 *
 * 用法：timebase_math
 *
 * 这个程序不读任何文件，只做计算。数字取自课程素材 small_360p25：
 * 视频 25 帧每秒，MP4 里的时间基是 1/12800；音频 44100 Hz，每个 AAC 包 1024 个采样。
 */

#include <inttypes.h>
#include <stdio.h>

#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>

static void print_q(const char *name, AVRational q)
{
    printf("  %s = %d/%d（%.6f）\n", name, q.num, q.den, av_q2d(q));
}

int main(void)
{
    /* AVRational 就是一个分数：num 是分子，den 是分母。用分数而不用小数，是为了没有舍入误差。 */
    const AVRational frame_rate = { 25, 1 };
    const AVRational tb_mp4_v   = { 1, 12800 };
    const AVRational tb_mp4_a   = { 1, 44100 };
    const AVRational tb_mkv     = { 1, 1000 };
    const AVRational tb_ts      = { 1, 90000 };
    /* AV_TIME_BASE_Q 是 1/1000000，AVFormatContext 里的 duration、start_time 用它。 */
    const AVRational tb_us      = AV_TIME_BASE_Q;

    printf("== 1. AVRational ==\n");
    print_q("帧率 frame_rate", frame_rate);
    /* av_inv_q() 取倒数：帧率的倒数就是一帧的时长。 */
    print_q("一帧的时长 av_inv_q(frame_rate)", av_inv_q(frame_rate));
    print_q("MP4 视频流的时间基", tb_mp4_v);
    /* av_q2d() 把分数转成 double，只用于显示和粗略比较，不要拿它的结果去算时间戳。 */
    printf("  pts 为 64000 时的秒数 = 64000 × %d/%d = %.3f\n",
           tb_mp4_v.num, tb_mp4_v.den, 64000 * av_q2d(tb_mp4_v));

    /*
     * av_rescale_q(a, bq, cq) 计算 a × bq / cq，把以 bq 为单位的数换算成以 cq 为单位。
     * 这里把“一帧的时长”换算到几种时间基下。
     */
    printf("\n== 2. av_rescale_q：同一段时间在不同时间基下的整数 ==\n");
    {
        const AVRational frame_dur = av_inv_q(frame_rate);

        printf("  一帧视频（1/25 秒）  1/12800 下是 %" PRId64 "，1/1000 下是 %" PRId64
               "，1/90000 下是 %" PRId64 "，1/1000000 下是 %" PRId64 "\n",
               av_rescale_q(1, frame_dur, tb_mp4_v), av_rescale_q(1, frame_dur, tb_mkv),
               av_rescale_q(1, frame_dur, tb_ts),    av_rescale_q(1, frame_dur, tb_us));
        printf("  一个 AAC 包（1024 个采样）  1/44100 下是 1024，1/1000 下是 %" PRId64
               "，1/90000 下是 %" PRId64 "\n",
               av_rescale_q(1024, tb_mp4_a, tb_mkv), av_rescale_q(1024, tb_mp4_a, tb_ts));
    }

    /*
     * 换算的结果必须是整数，除不尽时要舍入。av_rescale_q() 四舍五入到最近的整数。
     * 目标时间基越粗，丢掉的精度越多，而且换算回来也恢复不了。
     */
    printf("\n== 3. 舍入：换算到更粗的时间基会丢精度 ==\n");
    {
        const int64_t ms   = av_rescale_q(1024, tb_mp4_a, tb_mkv);
        const int64_t back = av_rescale_q(ms, tb_mkv, tb_mp4_a);

        printf("  1024 个采样的准确时长是 %.4f 毫秒\n", 1024 * av_q2d(tb_mp4_a) * 1000);
        printf("  换算到 1/1000：%" PRId64 "，再换算回 1/44100：%" PRId64 "，少了 %" PRId64 " 个采样\n",
               ms, back, 1024 - back);
        /* 需要指定舍入方向时用 av_rescale_q_rnd()。 */
        printf("  向下舍入 AV_ROUND_DOWN：%" PRId64 "，向上舍入 AV_ROUND_UP：%" PRId64 "\n",
               av_rescale_q_rnd(1024, tb_mp4_a, tb_mkv, AV_ROUND_DOWN),
               av_rescale_q_rnd(1024, tb_mp4_a, tb_mkv, AV_ROUND_UP));
    }

    /*
     * 工程里最常见的错误：先把“一个包的时长”换算好，再一个一个累加。
     * 每次舍入丢掉的零头会越积越多。正确做法是对每个时间戳单独换算。
     */
    printf("\n== 4. 换算时间戳，不要累加换算后的时长 ==\n");
    {
        const int64_t dur_ms = av_rescale_q(1024, tb_mp4_a, tb_mkv);
        const int counts[] = { 1, 10, 100, 217, 43066 };

        printf("  n 是包的序号，三列数字的单位都是毫秒\n");
        printf("  %8s %18s %18s %8s\n", "n", "rescale(n * 1024)", "n * rescale(1024)", "diff");
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            const int64_t n       = counts[i];
            const int64_t correct = av_rescale_q(n * 1024, tb_mp4_a, tb_mkv);
            const int64_t summed  = n * dur_ms;

            printf("  %8" PRId64 " %18" PRId64 " %18" PRId64 " %8" PRId64 "\n",
                   n, correct, summed, correct - summed);
        }
        printf("  217 个包是素材的长度（5 秒），43066 个包大约是 1000 秒。\n");
    }

    /*
     * 为什么不自己写 a * num / den：中间的乘积可能超出 int64_t。
     * 直播流常用墙上时钟（1970 年以来的微秒数）做时间戳，数值在 10 的 15 次方量级。
     */
    printf("\n== 5. 溢出：为什么不自己写乘除法 ==\n");
    {
        const int64_t wallclock_us = INT64_C(1794441600000000); /* 2026-11-12 00:00:00 UTC */
        int64_t product;
        /* __builtin_mul_overflow 是 gcc 和 clang 提供的函数，乘法溢出时返回 1。 */
        const int overflow = __builtin_mul_overflow(wallclock_us, (int64_t)90000, &product);

        printf("  时间戳 %" PRId64 "（单位 1/1000000）换算到 1/90000：\n", wallclock_us);
        printf("  自己算 ts * 90000 / 1000000：乘法%s\n", overflow ? "溢出，结果是错的" : "没有溢出");
        printf("  av_rescale_q() 的结果：%" PRId64 "\n", av_rescale_q(wallclock_us, tb_us, tb_ts));
    }

    /*
     * av_compare_ts() 比较两个时间基不同的时间戳，不需要先换算，也没有舍入误差。
     * 返回 -1、0、1 分别表示第一个更早、相同、更晚。
     */
    printf("\n== 6. av_compare_ts：比较不同时间基下的两个时间戳 ==\n");
    {
        const int64_t v_dts = 512, a_dts = 2048;

        printf("  视频包 dts = %" PRId64 "（%.4f 秒），音频包 dts = %" PRId64 "（%.4f 秒）\n",
               v_dts, v_dts * av_q2d(tb_mp4_v), a_dts, a_dts * av_q2d(tb_mp4_a));
        printf("  av_compare_ts() 返回 %d：视频包更早，应该先写\n",
               av_compare_ts(v_dts, tb_mp4_v, a_dts, tb_mp4_a));
    }

    return 0;
}
