# L18 自定义 I/O

前面所有的程序，读数据都是 `avformat_open_input()` 给一个文件名，写数据都是 `avio_open()` 给一个文件名。
这一讲换一个方向：数据不在文件里，在内存里、网络上、数据库里，或者要先解密才能用，
这时要自己提供读数据、写数据的函数，让 libavformat 通过它们读写，这就是**自定义 I/O**（`AVIOContext`）。

官方示例 `avio_read_callback.c` 展示了读的一边。我们在它的基础上加了两个自己写的程序：
`avio_mem_in` 能把每一次回调打印出来，能不提供 seek 回调，能在读回调里“解密”；
`avio_mem_out` 是写的一边，把输出写进内存，对比各种容器对 seek 的要求。

完成标志：说得清楚读回调、写回调、seek 回调各自的约定；
知道有没有 seek 回调会怎样改变读和写的结果（MP4 的 `moov` 在文件尾时没有 seek 回调读不出包，写 MP4 没有 seek 回调根本写不出来）；
知道自己的 `AVIOContext` 怎样接到解复用器和复用器上，收尾时按什么顺序释放；
`avio_mem_in` 数出来的包数和 `ffprobe` 一致，`avio_mem_out` 写出来的和直接写文件逐字节相同（`compare_file.sh`，28 项相同）。

## 本讲要点

**数据从哪里来：`AVIOContext` 夹在中间**

解复用器和复用器不直接读写文件，它们只认 `AVIOContext`。`AVIOContext` 自己有一块缓冲区，
缓冲区空了（读）或者满了（写），才去找真正的数据来源：

```text
解复用器、复用器（mov、matroska、mpegts ……）
        │   avio_read()、avio_r8()、avio_write()、avio_seek() ……
        ▼
AVIOContext：缓冲区 + 三个函数指针
        │   read_packet   缓冲区空了，要数据
        │   write_packet  缓冲区满了，或者 avio_flush()，交出数据
        │   seek          要换一个位置
        ▼
   ┌────┴──────────────────────────────┐
   │                                   │
协议层（file、http、pipe ……）       我们自己的回调
avio_open()、avformat_open_input()    avio_alloc_context()
按 URL 自动选                         把 AVIOContext 挂到 fmt_ctx->pb 上
```

用文件名的时候，libavformat 替我们建好一个 `AVIOContext`，回调是协议层的函数；自定义 I/O 就是自己建，回调换成自己的。
解复用器和复用器看不出区别。这一点后面会用来验证：自定义 I/O 读出来的包和直接读文件的一模一样，写出来的文件和直接写文件的逐字节相同。

**三个回调的约定**

| 回调 | 原型（这个版本） | 返回值 | 要注意 |
| --- | --- | --- | --- |
| 读 | `int read_packet(void *opaque, uint8_t *buf, int buf_size)` | 读到的字节数；没有数据了返回 `AVERROR_EOF`；出错返回别的负数 | 可以比 `buf_size` 少（实测每次只给 100 字节也行）。**不能在末尾返回 0**：MKV、FLV 会卡死（第 5 步） |
| 写 | `int write_packet(void *opaque, const uint8_t *buf, int buf_size)` | 写出的字节数，要等于 `buf_size`；出错返回负数 | 返回比 `buf_size` 小的正数，libavformat 不会重试，当作全写完了（练习 5） |
| seek | `int64_t seek(void *opaque, int64_t offset, int whence)` | 新的位置（从头算的字节数）；不支持、出错返回负数 | `whence` 是 `SEEK_SET`、`SEEK_CUR`、`SEEK_END` 三选一，可能或上 `AVSEEK_FORCE`；还有 `AVSEEK_SIZE`：不移动，只问“一共多大” |

三个回调的第一个参数 `opaque` 都是 `avio_alloc_context()` 传进去的同一个指针，用来找到“我们的数据”。
`avio_alloc_context()` 的第 3 个参数 `write_flag`：0 是读，1 是写。一个 `AVIOContext` 只能用来读或者用来写。

回调实际收到的 `whence`，比约定的少：`avio_seek()` 会先把 `SEEK_CUR` 换算成绝对位置，只用 `SEEK_SET` 调回调，
也不接受 `SEEK_END`（返回 `AVERROR(EINVAL)`）；`SEEK_END` 只有一种情况会出现：`avio_size()` 问 `AVSEEK_SIZE` 没有得到回答，就退而求其次，
调 `seek(-1, SEEK_END)` 得到最后一个字节的位置（第 4 步）。第 3 步的记录里看到的只有 `SEEK_SET`（“从头”）和“问大小”。
但三种 `whence` 还是都要实现：回调是公开的函数，别的代码也可能直接调它。

seek 回调可以传 `NULL`，意思是“不能 seek”。这看起来只是少了个功能，其实差别很大，下面两张表是这一讲的重点。

**读：没有 seek 回调，哪些输入读不全**

`avio_mem_in` 加 `seek=0` 就是不提供 seek 回调。同一个素材读两次（命令见第 4 步）：

| 输入 | 有 seek 回调 | 没有 seek 回调 |
| --- | --- | --- |
| MP4（`moov` 在文件尾） | 读 6 次，时长 5.000 秒，码率 901 kbit/s，流参数完整 | **读 138 次（整个文件），视频流的像素格式未知，一个包也读不出来** |
| MKV | 读 5 次，时长 5.038 秒，码率 891 kbit/s | 读 5 次，时长 5.038 秒，**没有码率** |
| MPEG-TS | 读 87 次，时长 5.039 秒，码率 967 kbit/s | 读 25 次，**时长未知**，没有码率 |
| FLV | 读 43 次，时长 5.095 秒，码率 886 kbit/s | 读 43 次，时长 5.095 秒，**没有码率** |
| MP2 | 读 8 次，时长 5.016 秒 | 读 6 次，**时长未知** |

没有 seek 回调时，libavformat 不知道数据一共多大（算不出码率），也不能跳到末尾去估计时长，更不能回头。
MP4 最严重：`moov` 里记着每一帧在哪里，它在文件尾，只能把整个 `mdat` 读过去才能拿到，而读过去的数据已经回不来了。

**写：没有 seek 回调，哪些输出写不出来**

`avio_mem_out` 同样加 `seek=0`，把 MP4 的 5 秒素材复制到各种容器里（第 7 步）：

| 输出容器 | 没有 seek 回调 |
| --- | --- |
| MP4 | **写文件头就失败**：`muxer does not support non seekable output` |
| MKV | 能写，但文件头里的时长、各部分的大小没有回填，`ffprobe` 看时长是 N/A |
| FLV | 能写，警告 `Failed to update header with correct duration`，文件头里的时长不对（4.960 秒，应该是 5.095 秒） |
| MPEG-TS | 能写，和有 seek 的输出逐字节相同：TS 是一个个独立的小包，不需要回头 |
| 分片 MP4（`movflags=frag_keyframe+empty_moov+default_base_moof`） | 能写，和直接写文件逐字节相同（L15 讲过） |

写和读是同一个道理：**要回头改的字段，就需要 seek**。MP4 的 `mdat` 大小、`moov` 里每一层盒子的大小，要等写完才知道，写完再回头填（第 7 步能看到每一次回头）。

**两种内存输出**

| | 自己的写回调 | `avio_open_dyn_buf()` |
| --- | --- | --- |
| 谁管缓冲区 | 自己（要能长大，要处理往回写） | libavformat |
| 结果怎么拿 | 自己的缓冲区 | `avio_close_dyn_buf()` 返回指针和长度，用完 `av_free()` |
| 能做别的事吗 | 能：加密、限速、边写边上传、统计 | 只攒在内存里 |

`avio_open_dyn_buf()` 省事，但第 9 步会看到它有一个问题：flv、aiff、caf、asf 写出来的结果被弄坏了，补丁是 0084。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L18_avio_callbacks             # 可执行文件在 L18_avio_callbacks/build/ 下
B=L18_avio_callbacks/build
mkdir -p out out/l18
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `avio_read_callback` | 官方示例（加了课程注释，打了 2 个 patch）：整个文件读进内存，通过读回调和 seek 回调交给 libavformat，打印流信息 |
| `avio_mem_in` | 自己写的自定义输入：打印回调，可以没有 seek 回调、可以用小缓冲区、可以从标准输入读、可以在读回调里“解密”、把所有的包数一遍 |
| `avio_mem_out` | 自己写的自定义输出：把包复制到内存里，三种输出方式（写文件、自己的写回调、`avio_open_dyn_buf`），写回调里可以“加密” |
| `compare_file.sh` | 一次性比较自定义 I/O 和直接读写文件的结果（第 10 步之后） |

本讲用到的素材：`samples/small_360p25.mp4`（MP4）、`.mkv`、`.ts`、`.flv`，以及 `samples/audio_44k_stereo.mp2`。
`avio_mem_in` 的参数：`avio_mem_in <文件名|-> [seek=0|1] [bufsize=N] [xor=KEY] [trace=N] [packets=0|1]`，别的 `名字=值` 交给解复用器；
`avio_mem_out` 的参数：`avio_mem_out <输入文件> <输出格式名> <输出文件> [mode=file|cb|dyn] [seek=0|1] [bufsize=N] [trace=N] [bitexact=0|1] [xor=KEY] [dynend=0|1]`，别的 `名字=值` 交给复用器。

## 第 1 步：官方示例 avio_read_callback.c

```bash
$B/avio_read_callback samples/small_360p25.mp4 2>/dev/null | sed 's/^ptr:[^ ]* //'
$B/avio_read_callback samples/small_360p25.mp4 >/dev/null 2>&1; echo "退出码 $?"
$B/avio_read_callback samples/small_360p25.mp4 2>&1 >/dev/null | grep -E "Duration|Stream"
```

```text
size:563196
size:5329
size:1233
size:563148
size:554236
size:549755
退出码 0
  Duration: 00:00:05.00, start: 0.000000, bitrate: 901 kb/s
  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 640x360 [SAR 1:1 DAR 16:9], 770 kb/s, 25 fps, 25 tbr, 12800 tbn (default)
  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 121 kb/s (default)
```

程序做的事：把整个文件读进内存，通过读回调和 seek 回调一块一块交给 libavformat，最后打印流信息。
结果和 L03 的 `ffprobe` 一样，不同的是**数据不是 libavformat 自己读的，是我们的回调交给它的**。
每次读回调打印一行“后面还剩多少字节”（`size:`）。六行是这样来的：

1. `563196`：第一次读，从文件头开始，探测格式（读 4096 字节）。
2. `5329`：libavformat 发现 MP4 的 `moov` 在文件尾，**seek 到 557867**，那里到文件尾还剩 5329 字节。
3. `1233`：再读 4096 字节，还剩 1233。
4. `563148`：`moov` 读完了，**seek 回 48**，也就是 `mdat` 的开头。
5. `554236`、`549755`：从 `mdat` 读一些帧来补全流的参数。

整个过程里，回调被调了 6 次，一共读了不到 30 KB，没有把 563 KB 的文件读完。这是 seek 回调的功劳，第 2 步会看到没有它是什么样子。

读 `avio_read_callback.c`，按调用顺序看这几处（每一处都有课程注释）：

- `read_packet()`：读回调。`FFMIN` 取剩下的和要的的较小者，`memcpy` 复制，修改 `ptr` 和 `size`，返回复制的字节数。
- `seek_buffer()`：seek 回调（patch 0083 加的）。处理 `SEEK_SET`、`SEEK_CUR`、`SEEK_END` 和 `AVSEEK_SIZE`。
- `main()` 里的 `avio_alloc_context()`：七个参数，读写标志 0，`opaque` 是 `&bd`。
- `fmt_ctx->pb = avio_ctx;`：把自己的 `AVIOContext` 挂上去，`avformat_open_input()` 的文件名传 `NULL`。
- `end:` 里的释放顺序：先 `avformat_close_input()`，再释放 `avio_ctx->buffer`，再 `avio_context_free()`，最后才 `av_file_unmap()`。

出错的时候：

```bash
$B/avio_read_callback /nonexistent; echo "退出码 $?"
head -c 5000 /dev/urandom > out/l18/rand.bin
$B/avio_read_callback out/l18/rand.bin; echo "退出码 $?"
```

```text
[FILE @ 0x7ffc88e89a60] Cannot read file '/nonexistent': No such file or directory
Error occurred: No such file or directory
退出码 1
ptr:0x7fc9eb505000 size:5000
ptr:0x7fc9eb506000 size:904
Could not open input
Error occurred: Invalid data found when processing input
退出码 1
```

文件不存在，连 `av_file_map()` 都失败，回调一次也没有被调用。随机数据读了两次（4096 字节加 904 字节，探测用的），没有任何格式认得，`avformat_open_input()` 返回 `AVERROR_INVALIDDATA`。
（`ptr:` 后面的地址每次不同。输出到终端时 `ptr:` 在前；重定向到文件或者管道时，stdout 整块缓冲，顺序会变，代码里的课程注释说过。）

## 第 2 步：原版的两个问题

**问题一（patch 0082）：编译警告。** 提交 `a337e8d` 的原版 `read_packet()` 里有一行：

```c
buf_size = FFMIN(buf_size, bd->size);     /* buf_size 是 int，bd->size 是 size_t */
```

加上 `-Wall -Wextra` 编译：

```text
warning: comparison of integer expressions of different signedness: 'int' and 'size_t' {aka 'long unsigned int'} [-Wsign-compare]
```

警告本身不影响运行（`buf_size` 不会是负数），但官方示例应该编译干净，改成 `FFMIN((size_t)buf_size, bd->size)`。

**问题二（patch 0083）：没有 seek 回调。** 原版的 `avio_alloc_context()` 最后一个参数是 `NULL`。在读 MP4 以外的格式时没有区别，
但 `moov` 在文件尾的 MP4（用 `ffmpeg` 默认设置转出来的就是这样，L15 说过）就读不全：

```bash
# patch 0083 以前的行为：avio_mem_in 加 seek=0，等于没有 seek 回调
$B/avio_mem_in samples/small_360p25.mp4 seek=0 packets=0; echo "退出码 $?"
```

```text
来源：内存，大小已知，缓冲区 4096 字节，seek 回调 没有
数据大小：563196 字节
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x55d821da4380] stream 0, offset 0x30: partial file
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x55d821da4380] Could not find codec parameters for stream 0 (Video: h264 (avc1 / 0x31637661), none, 640x360, 770 kb/s): unspecified pixel format
Consider increasing the value for the 'analyzeduration' (0) and 'probesize' (5000000) options
格式：mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒
  流 #0：video h264，640x360，像素格式 未知    ← 参数不完整
  流 #1：audio aac，44100 Hz，stereo，采样格式 fltp
流参数不完整
回调统计：读 138 次共 563196 字节，seek 0 次，问大小 0 次
退出码 2
```

三处不对：像素格式 `none`；**138 次读回调把整个文件读完了**，读 `moov` 的代价是把 563 KB 的 `mdat` 全部读过一遍；
日志里 `partial file`：`mdat` 已经读过去了，回不来，视频包一个也读不出来。
对比有 seek 回调时（第 1 步）的 6 次读、`yuv420p`。

最危险的是：**libavformat 不报错**。`avformat_open_input()` 返回 0，`avformat_find_stream_info()` 也返回 0，官方示例的退出码也是 0，
只有打印出来的流信息里能看出问题。所以 `avio_mem_in` 自己检查每路流的参数，不完整时退出码是 2（上面的退出码就是这样来的）。

patch 0083 给官方示例加了 `seek_buffer()`，`struct buffer_data` 里多了 `start` 和 `total` 两个字段（seek 要从头算位置，要回答“一共多大”）。
`seek_buffer()` 要处理的事：

- `whence` 先去掉 `AVSEEK_FORCE`；
- `AVSEEK_SIZE`：返回总大小，不移动；
- `SEEK_SET`、`SEEK_CUR`、`SEEK_END`：算出新位置，位置小于 0 或者超过总大小返回 `AVERROR(EINVAL)`；
- 返回新位置，**不是 0**。

## 第 3 步：avio_mem_in，看每一次回调

`avio_mem_in` 的 `trace=N` 把前 N 次回调打印出来：

```bash
$B/avio_mem_in samples/small_360p25.mp4 trace=12 packets=0 | sed -n 3,15p
```

```text
回调记录（前 12 次）：
  回调 #1   读   位置 0，要 4096 字节 → 给 4096 字节
  回调 #2   问大小 → 知道
  回调 #3   seek 位置 4096 → 557867（从头 557867）
  回调 #4   读   位置 557867，要 4096 字节 → 给 4096 字节
  回调 #5   读   位置 561963，要 4096 字节 → 给 1233 字节
  回调 #6   问大小 → 知道
  回调 #7   seek 位置 563196 → 48（从头 48）
  回调 #8   读   位置 48，要 8912 字节 → 给 8912 字节
  回调 #9   读   位置 8960，要 4481 字节 → 给 4481 字节
  回调 #10  读   位置 13441，要 4096 字节 → 给 4096 字节
  回调 #11  问大小 → 知道
  回调 #12  问大小 → 知道
格式：mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒，码率 901 kbit/s
```

和第 1 步官方示例的六行对得上，现在多了几样：

- **问大小**：`AVSEEK_SIZE`。解复用器要知道文件多大，去问 seek 回调。一共问了 5 次。
- **读的字节数不是缓冲区大小**：第 8 行要 8912 字节，第 9 行要 4481 字节，不是 4096。解复用器要读一块指定长度的数据时，
  `AVIOContext` 在缓冲区放不下的情况下直接读进目标里，不经过缓冲区。所以回调里**不能假设 `buf_size` 等于缓冲区大小**。
- **最后一次读回调给的比要的少**：第 5 行要 4096 字节，只给 1233（文件到头了）。这是允许的，下一次再读才返回 `AVERROR_EOF`。

加上 `seek=0`，看没有 seek 回调时 libavformat 怎么读（前几行）：

```bash
$B/avio_mem_in samples/small_360p25.mp4 seek=0 trace=6 packets=0 2>/dev/null | sed -n 3,10p
```

```text
回调记录（前 6 次）：
  回调 #1   读   位置 0，要 4096 字节 → 给 4096 字节
  回调 #2   读   位置 4096，要 4096 字节 → 给 4096 字节
  回调 #3   读   位置 8192，要 4096 字节 → 给 4096 字节
  回调 #4   读   位置 12288，要 4096 字节 → 给 4096 字节
  回调 #5   读   位置 16384，要 4096 字节 → 给 4096 字节
  回调 #6   读   位置 20480，要 4096 字节 → 给 4096 字节
```

位置一路往后，一次也不回头，一直读到文件尾（一共 138 次）。

## 第 4 步：seek 回调决定了什么

把五个素材各读两次，一次有 seek 回调，一次没有：

```bash
for f in small_360p25.mp4 small_360p25.mkv small_360p25.ts small_360p25.flv audio_44k_stereo.mp2; do
  for s in 1 0; do
    out=$($B/avio_mem_in samples/$f seek=$s packets=0 2>/dev/null)
    echo "$f seek=$s | $(echo "$out" | grep '^格式' | sed 's/格式：//') | $(echo "$out" | grep '^回调统计' | sed 's/回调统计：//') | $(echo "$out" | grep '^流参数')"
  done
done
```

```text
small_360p25.mp4 seek=1 | mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒，码率 901 kbit/s | 读 6 次共 26914 字节，seek 2 次，问大小 5 次 | 流参数完整
small_360p25.mp4 seek=0 | mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒 | 读 138 次共 563196 字节，seek 0 次，问大小 0 次 | 流参数不完整
small_360p25.mkv seek=1 | matroska,webm，2 路流，时长 5.038 秒，码率 891 kbit/s | 读 5 次共 22051 字节，seek 0 次，问大小 4 次 | 流参数完整
small_360p25.mkv seek=0 | matroska,webm，2 路流，时长 5.038 秒 | 读 5 次共 22051 字节，seek 0 次，问大小 0 次 | 流参数完整
small_360p25.ts seek=1 | mpegts，2 路流，时长 5.039 秒，码率 967 kbit/s | 读 87 次共 352400 字节，seek 2 次，问大小 4 次 | 流参数完整
small_360p25.ts seek=0 | mpegts，2 路流，时长未知 | 读 25 次共 102400 字节，seek 0 次，问大小 0 次 | 流参数完整
small_360p25.flv seek=1 | flv，2 路流，时长 5.095 秒，码率 886 kbit/s | 读 43 次共 179315 字节，seek 0 次，问大小 3 次 | 流参数完整
small_360p25.flv seek=0 | flv，2 路流，时长 5.095 秒 | 读 43 次共 179315 字节，seek 0 次，问大小 0 次 | 流参数完整
audio_44k_stereo.mp2 seek=1 | mp3，1 路流，时长 5.016 秒，码率 128 kbit/s | 读 8 次共 28800 字节，seek 2 次，问大小 5 次 | 流参数完整
audio_44k_stereo.mp2 seek=0 | mp3，1 路流，时长未知，码率 128 kbit/s | 读 6 次共 24576 字节，seek 0 次，问大小 0 次 | 流参数完整
```

读这张表：

- **MP4 没有 seek 就是不能用**，上面说过。
- **MKV、FLV 没有 seek 也能读**：时长都在文件头里写着。但**码率没了**，因为码率是“文件大小 ÷ 时长”算出来的，没有 seek 回调就没法问大小。
- **TS、MP2 没有 seek，时长就未知**。这两种格式文件头里没有时长，要靠读文件开头和结尾的时间戳相减（TS）或者按码率估算（MP2）。有了 seek 回调就可以“跳到末尾读一点”。
- **有 seek 不等于便宜**：TS 有 seek 时读 87 次、352 KB，没有时 25 次、102 KB。为了算时长，libavformat 多读了不少。

**问大小可以不支持吗？** `AVSEEK_SIZE` 返回负数（比如 `AVERROR(ENOSYS)`），`avio_size()` 就改用 `seek(-1, SEEK_END)` 得到最后一个字节的位置，再 seek 回原来的位置，结果一样，只是每问一次多两次 seek（练习 3）。

## 第 5 步：读回调的约定

**读到文件尾要返回 `AVERROR_EOF`，不是 0。** 做个实验：把 `avio_mem_in.c` 里的 `return AVERROR_EOF;` 改成 `return 0;`，其他不变（`timeout 10` 防止卡死）：

```bash
sed 's/return AVERROR_EOF;/return 0;/' L18_avio_callbacks/avio_mem_in.c > out/l18/zero.c
gcc -std=c11 -g $(pkg-config --cflags libavformat libavcodec libavutil) out/l18/zero.c -o out/l18/zero \
    $(pkg-config --libs libavformat libavcodec libavutil)
for f in mp4 ts mkv flv; do
  timeout 10 out/l18/zero samples/small_360p25.$f >/dev/null 2>&1; echo "$f 退出码 $?"
done
```

```text
mp4 退出码 0
ts 退出码 0
mkv 退出码 124
flv 退出码 124
```

124 是 `timeout` 的超时：**MKV 和 FLV 卡死了**，MP4、TS 碰巧没事。裸的 MPEG-1 视频 `samples/video_360p25.m1v` 也卡死，MP2 没事。
原因是返回 0 既不是数据也不是 EOF，`AVIOContext` 不会把它当成文件尾（`eof_reached` 没有置位），有的解复用器就一直重试。
没有任何错误信息，所以最难查。（用 `--assert-level=2` 配置的 FFmpeg，`read_packet_wrapper()` 里有一句 `av_assert2(ret || s->max_packet_size)`，返回 0 会直接断言失败。）

**少给不要紧。** 反过来，每次只给一点点是允许的。把 `avio_mem_in.c` 里的 `FFMIN((size_t)buf_size, left)` 改成 `FFMIN((size_t)FFMIN(buf_size, 100), left)`，每次最多给 100 字节：

```bash
sed 's/n = (int)FFMIN((size_t)buf_size, left);/n = (int)FFMIN((size_t)FFMIN(buf_size, 100), left);/' \
    L18_avio_callbacks/avio_mem_in.c > out/l18/short.c
gcc -std=c11 -g $(pkg-config --cflags libavformat libavcodec libavutil) out/l18/short.c -o out/l18/short \
    $(pkg-config --libs libavformat libavcodec libavutil)
out/l18/short samples/small_360p25.ts 2>&1 | tail -5
```

```text
回调统计：读 3527 次共 352664 字节，seek 6 次，问大小 4 次
读完所有的包：
  流 #0：125 个包，482550 字节
  流 #1：217 个包，77733 字节
读包又用了 6097 次读回调，现在共读 962348 字节，seek 6 次
```

读的次数多了很多（正常是 87 次），但包的个数一样：125 个视频包、217 个音频包，和 `ffprobe` 数出来的相同。
网络上的数据就是这样一点一点到的，读回调可以只给已经有的那一部分（阻塞等待、或者返回 `AVERROR(EAGAIN)` 是另一种做法，这里不展开）。

**缓冲区大小：次数和字节数的取舍。** `bufsize=` 是 `AVIOContext` 缓冲区的大小。读 MP4（`packets=0`）：

```bash
for n in 16 512 4096 32768 262144; do echo -n "bufsize=$n  "; $B/avio_mem_in samples/small_360p25.mp4 bufsize=$n packets=0 | grep 回调统计; done
```

```text
bufsize=16  回调统计：读 262 次共 21103 字节，seek 5 次，问大小 5 次
bufsize=512  回调统计：读 15 次共 21282 字节，seek 2 次，问大小 5 次
bufsize=4096  回调统计：读 6 次共 26914 字节，seek 2 次，问大小 5 次
bufsize=32768  回调统计：读 3 次共 70865 字节，seek 2 次，问大小 5 次
bufsize=262144  回调统计：读 3 次共 529617 字节，seek 2 次，问大小 5 次
```

缓冲区越大，回调的次数越少，但是每次多读进来的、不一定用得上的字节越多：256 KB 的缓冲区，探测流信息就读了 529 KB，几乎是整个文件。
数据来自网络或者磁盘时，每次回调有固定的开销，缓冲区大一点划算；数据来自内存的话，反过来，小一点没有损失。

## 第 6 步：数据来自标准输入

`avio_mem_in -` 从标准输入读，这是最常见的“自定义”来源：别的程序的输出通过管道进来，或者用 `<` 重定向文件。
程序会检查标准输入能不能 `lseek()`：管道不行，就算要求了 seek 也没有；重定向的文件可以。

```bash
cat samples/small_360p25.ts | $B/avio_mem_in - packets=0
```

```text
来源：标准输入（管道），大小未知，缓冲区 4096 字节，seek 回调 没有
格式：mpegts，2 路流，时长未知
  流 #0：video h264，640x360，像素格式 yuv420p
  流 #1：audio aac，44100 Hz，stereo，采样格式 fltp
流参数完整
回调统计：读 25 次共 102400 字节，seek 0 次，问大小 0 次
```

```bash
cat samples/small_360p25.mp4 | $B/avio_mem_in - packets=0 2>/dev/null; echo "退出码 ${PIPESTATUS[1]}"
```

```text
来源：标准输入（管道），大小未知，缓冲区 4096 字节，seek 回调 没有
格式：mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒
  流 #0：video h264，640x360，像素格式 未知    ← 参数不完整
  流 #1：audio aac，44100 Hz，stereo，采样格式 fltp
流参数不完整
回调统计：读 138 次共 563196 字节，seek 0 次，问大小 0 次
退出码 2
```

```bash
$B/avio_mem_in - packets=0 < samples/small_360p25.mp4
```

```text
来源：标准输入（文件重定向），大小已知，缓冲区 4096 字节，seek 回调 有
数据大小：563196 字节
格式：mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒，码率 901 kbit/s
  流 #0：video h264，640x360，像素格式 yuv420p
  流 #1：audio aac，44100 Hz，stereo，采样格式 fltp
流参数完整
回调统计：读 6 次共 26914 字节，seek 2 次，问大小 5 次
```

和第 4 步完全一致：TS 从管道读没有问题，时长未知；**MP4 从管道读，流参数不完整**，退出码 2；用 `<` 重定向文件，可以 seek，一切正常。
所以“把 MP4 通过管道交给程序”这件事，对 `moov` 在文件尾的文件是行不通的，要先转成 `+faststart`（L15）或者分片 MP4。

命令行的 `ffprobe -i pipe:0` 有同样的限制，而且更严格：`pipe:` 协议永远不能 seek，即使是 `< file` 重定向的文件也一样：

```bash
ffprobe -v warning -i pipe:0 -show_entries stream=codec_name,pix_fmt -of csv=p=0 < samples/small_360p25.mp4 2>&1 | tail -3
```

```text
Consider increasing the value for the 'analyzeduration' (0) and 'probesize' (5000000) options
h264,unknown
aac
```

`pix_fmt` 是 `unknown`，和我们没有 seek 回调时一样。命令行要从标准输入读 MP4，只能是 `+faststart` 的文件。

## 第 7 步：自定义输出 avio_mem_out

写的一边，`avio_alloc_context()` 的 `write_flag` 是 1，传写回调，`read_packet` 传 `NULL`。
`avio_mem_out` 把输入文件的包原样复制（不解码，和 L04 的 remux 一样）到一个输出容器里，输出方式用 `mode=` 选：

| `mode=` | 输出方式 |
| --- | --- |
| `file` | `avio_open()`，libavformat 自己写文件，对照用 |
| `cb` | 自己的写回调，数据攒在内存里，最后存成文件 |
| `dyn` | `avio_open_dyn_buf()`，libavformat 自带的内存输出 |

MP4 到 MP4，用 `mode=cb`，看前几次回调和最后几次回调（`trace=100000` 是“全部”）：

```bash
I=samples/small_360p25.mp4
$B/avio_mem_out $I mp4 out/l18/m.mp4 trace=100000 > out/l18/trace.txt
sed -n 1,8p out/l18/trace.txt; echo ...; tail -14 out/l18/trace.txt
```

```text
输出方式：自己的写回调（内存），缓冲区 4096 字节，seek 回调 有，格式 mp4
回调记录（前 100000 次）：
  回调 #1   写   位置 0，48 字节
  回调 #2   写   位置 48，4096 字节
  回调 #3   写   位置 4144，4096 字节
  回调 #4   写   位置 8240，720 字节
  回调 #5   写   位置 8960，4096 字节
  回调 #6   写   位置 13056，385 字节
...
  回调 #398 写   位置 560582，4 字节（盖在已有的数据上）
  回调 #399 seek 位置 560586 → 563098
  回调 #400 seek 位置 563098 → 560497
  回调 #401 写   位置 560497，4 字节（盖在已有的数据上）
  回调 #402 seek 位置 560501 → 563098
  回调 #403 seek 位置 563098 → 560361
  回调 #404 写   位置 560361，4 字节（盖在已有的数据上）
  回调 #405 seek 位置 560365 → 563098
  回调 #406 写   位置 563098，98 字节
  回调 #407 seek 位置 563196 → 557867
  回调 #408 写   位置 557867，4 字节（盖在已有的数据上）
  回调 #409 seek 位置 557871 → 563196
复制了 342 个包，内存里共 563196 字节
回调统计：写 395 次，seek 14 次（往回 7 次）
```

开头是顺序写：先写 48 字节，是 `ftyp`（32 字节）、`free`（8 字节）和 `mdat` 的头（8 字节，大小这时还不知道，先占位），
然后每次 4096 字节，和缓冲区一样大。结尾能看到**回头改写**，一共往回 seek 7 次，每次都是 seek 回已经写过的位置，写 4 个字节，再 seek 回末尾：

- 第一次回到位置 40，写 `mdat` 的大小（557827），这时所有的帧都写完了，`moov` 还没有写；
- 后面 6 次是 `moov` 里每一层盒子写完以后回头填自己的大小；最后一次 `seek 563196 → 557867`，写的是最外层 `moov` 自己的大小（5329）。
  `moov` 就是从 557867 开始的，第 3 步读的时候，libavformat 也是 seek 到这里去找它。

**写回调要支持“位置在中间”**：本程序用 `pos` 记着当前位置，写在 `pos` 上，盖掉旧数据。如果写回调只会“往后追加”，MP4 就写坏了。

接着看没有 seek 回调：

```bash
for f in mp4 matroska mpegts flv; do for s in 1 0; do
  r=$($B/avio_mem_out $I $f out/l18/z.$f seek=$s bitexact=1 2>&1 | grep -E "回调统计|muxer does not|出错" | tr '\n' ' ')
  echo "$f seek=$s: $r"
done; done
```

```text
mp4 seek=1: 回调统计：写 395 次，seek 14 次（往回 7 次） 
mp4 seek=0: [mp4 @ 0x55fed272ebc0] muxer does not support non seekable output 出错：Invalid argument 
matroska seek=1: 回调统计：写 141 次，seek 2 次（往回 1 次） 
matroska seek=0: 回调统计：写 146 次，seek 0 次（往回 0 次） 
mpegts seek=1: 回调统计：写 391 次，seek 0 次（往回 0 次） 
mpegts seek=0: 回调统计：写 391 次，seek 0 次（往回 0 次） 
flv seek=1: 回调统计：写 388 次，seek 2 次（往回 1 次） 
flv seek=0: 回调统计：写 386 次，seek 0 次（往回 0 次） 
```

`bitexact=1` 让复用器不写随机数和时间（Matroska 每次都会写一个随机的 UID，不加这个选项，两次输出不可能逐字节相同）。

- **MP4**：没有 seek 回调，写文件头就失败：`muxer does not support non seekable output`。这是复用器自己检查的，不是我们的回调出了错。
- **MKV**：往回 seek 1 次，回填文件头里的时长、各部分的大小。没有 seek，这些字段留着占位的值：能写，但文件头里没有时长。
- **FLV**：往回 seek 1 次，回填时长和文件大小，没有 seek 时多了两句警告。
- **TS**：不需要回头，有没有 seek 都一样。

写出来的文件，用 `ffprobe` 看（`-count_packets` 数包，包数都是 125 和 217，包本身一个不少）：

```bash
for f in matroska flv mpegts; do for s in 1 0; do
  $B/avio_mem_out $I $f out/l18/q$s.$f seek=$s bitexact=1 >/dev/null 2>&1
  echo "$f seek=$s: $(stat -c %s out/l18/q$s.$f) 字节, duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 out/l18/q$s.$f), 包 $(ffprobe -v error -count_packets -show_entries stream=nb_read_packets -of csv=p=0 out/l18/q$s.$f | tr '\n' '/')"
done; done
```

```text
matroska seek=1: 561036 字节, duration=5.038000, 包 125/217/
matroska seek=0: 561014 字节, duration=N/A, 包 125/217/
flv seek=1: 564393 字节, duration=5.095000, 包 125/217/
flv seek=0: 564393 字节, duration=4.960000, 包 125/217/
mpegts seek=1: 640516 字节, duration=5.038722, 包 125/217/
mpegts seek=0: 640516 字节, duration=5.038722, 包 125/217/
```

MKV 没有 seek：时长 N/A（`ffprobe` 要读完才知道）；FLV 没有 seek：时长是 4.960 而不是 5.095，`ffprobe` 从文件里的时间戳估计的，和文件头里的值不一样。
**包都是对的，坏的是“文件头里要回填的信息”**。

**MP4 一定要 seek 吗？** 不一定。分片 MP4（L15）不需要回头：每个片段自带 `moof`，文件头的 `moov` 一开始就是空的：

```bash
FRAG=movflags=frag_keyframe+empty_moov+default_base_moof
$B/avio_mem_out $I mp4 out/l18/frag.mp4 seek=0 $FRAG | tail -2
$B/avio_mem_out $I mp4 out/l18/frag_file.mp4 mode=file $FRAG | tail -1
cmp out/l18/frag.mp4 out/l18/frag_file.mp4 && echo "与写文件相同"
ffprobe -v error -count_packets -show_entries stream=codec_name,nb_read_packets -of compact out/l18/frag.mp4
```

```text
复制了 342 个包，内存里共 562332 字节
回调统计：写 143 次，seek 0 次（往回 0 次）
复制了 342 个包，已写入文件 out/l18/frag_file.mp4
与写文件相同
stream|codec_name=h264|nb_read_packets=125
stream|codec_name=aac|nb_read_packets=217
```

没有 seek 回调，没有一次往回写，和直接写文件逐字节相同。这就是网络直播（HTTP 推流、管道）要用分片 MP4 的原因：输出不能回头。

## 第 8 步：在回调里加密和解密

回调是“夹在中间”的函数，可以在里面做别的事。最简单的例子是加密：写回调里把每个字节和密钥异或，读回调里再异或一次就还原。
`avio_mem_out` 的 `xor=KEY` 在写回调里加密，`avio_mem_in` 的 `xor=KEY` 在读回调里解密：

```bash
$B/avio_mem_out $I mp4 out/l18/enc.bin xor=secret | tail -2
ffprobe -v error -show_entries format=format_name out/l18/enc.bin 2>&1 | head -1
$B/avio_mem_in out/l18/enc.bin xor=secret | tail -9
```

```text
复制了 342 个包，内存里共 563196 字节
回调统计：写 395 次，seek 14 次（往回 7 次）
out/l18/enc.bin: Invalid data found when processing input
格式：mov,mp4,m4a,3gp,3g2,mj2，2 路流，时长 5.000 秒，码率 901 kbit/s
  流 #0：video h264，640x360，像素格式 yuv420p
  流 #1：audio aac，44100 Hz，stereo，采样格式 fltp
流参数完整
回调统计：读 6 次共 26914 字节，seek 2 次，问大小 5 次
读完所有的包：
  流 #0：125 个包，481605 字节
  流 #1：217 个包，76214 字节
读包又用了 128 次读回调，现在共读 570658 字节，seek 2 次
```

`enc.bin` 是加密过的 MP4，`ffprobe` 不认识；`avio_mem_in` 在读回调里解密，libavformat 看到的是明文，包的个数（125 和 217）和直接读明文文件的一样。
密钥错了呢：

```bash
$B/avio_mem_in out/l18/enc.bin xor=Secret 2>&1 | tail -3
```

```text
数据大小：563196 字节
打不开输入：Invalid data found when processing input
出错：Invalid data found when processing input
```

两个要点：

1. **密钥按“文件里的位置”循环用，不是按“读了多少字节”**：读回调里是 `key[(pos + i) % keylen]`。MP4 要 seek，seek 以后位置变了，如果用的是只增不减的计数器，seek 之后就解错了。流式的加密（比如 AES-CTR）要从 seek 的位置重新算计数器，不是简单的“往下接着解”。
2. **写的时候也一样**：写回调里 MP4 会回头盖旧数据，加密的位置要用写的位置，盖上去的新数据才和别处对得上（这里用 `s->pos + i`）。

## 第 9 步：avio_open_dyn_buf 和 patch 0084

`avio_open_dyn_buf()` 是 libavformat 自带的“写进内存”，不用自己管缓冲区的增长和往回写。用法是写完 `av_write_trailer()` 以后调 `avio_close_dyn_buf()`，
拿到指针和长度，用完 `av_free()`。省事，所以很多代码用它。把同一个输入分别用 `mode=file`、`mode=cb`、`mode=dyn` 写出来比较（`bitexact=1`）：

```bash
for f in mp4 matroska flv; do
  for m in file cb dyn; do $B/avio_mem_out $I $f out/l18/x_$m.$f mode=$m bitexact=1 >/dev/null 2>&1; done
  echo -n "$f: "
  for m in cb dyn; do
    cmp -s out/l18/x_$m.$f out/l18/x_file.$f && echo -n "$m 与写文件相同  " || echo -n "$m 与写文件不同（$(stat -c %s out/l18/x_$m.$f) 对 $(stat -c %s out/l18/x_file.$f) 字节）  "
  done; echo
done
```

```text
mp4: cb 与写文件相同  dyn 与写文件相同  
matroska: cb 与写文件相同  dyn 与写文件相同  
flv: cb 与写文件相同  dyn 与写文件不同（564329 对 564393 字节）  
```

FLV 用 `avio_open_dyn_buf()` 写出来**少了 64 字节，内容也变了**。看变了什么：

```bash
cmp -l out/l18/x_file.flv out/l18/x_dyn.flv 2>&1 >/dev/null        # 只看 cmp 的提示
cmp -l out/l18/x_file.flv out/l18/x_dyn.flv 2>/dev/null | wc -l     # 有几个字节不同
cmp -l out/l18/x_file.flv out/l18/x_dyn.flv 2>/dev/null | head -3   # 位置、写文件的值、dyn 的值（八进制）
ffprobe -v error -count_packets -show_entries stream=nb_read_packets -of csv=p=0 out/l18/x_file.flv | tr '\n' ' '; echo
ffprobe -v error -count_packets -show_entries stream=nb_read_packets -of csv=p=0 out/l18/x_dyn.flv 2>&1 | tr '\n' ' '; echo
```

```text
cmp: EOF on out/l18/x_dyn.flv after byte 564329
37
   272  11   0
   275   1   0
   276   3   0
125 217 
[flv @ 0x557407f40cc0] Missing AMF_END_OF_OBJECT in AMF_DATA_TYPE_MIXEDARRAY, found 0 [flv @ 0x557407f40cc0] Packet mismatch 0 259 259 [extract_extradata @ 0x557407f444c0] No start code is found. [extract_extradata @ 0x557407f6de40] No start code is found. N/A N/A 
```

从第 272 个字节开始，一段本来有数据的地方变成了 0（第三列是 0），文件头的元数据被破坏，包也数不出来。
`dyn` 的文件比正常的短 64 字节（第一行：`cmp` 比到 564329 字节就没有了）。

原因在 `avio_close_dyn_buf()`：它要在数据后面补 `AV_INPUT_BUFFER_PADDING_SIZE`（64）字节的零，
**补在“当前写位置”上**，然后返回“总长度减 64”。写位置不一定在末尾：FLV 的 `write_trailer()` 回头把时长和文件大小填进文件头，
填完就停在那里，没有 seek 回末尾。于是 64 个零盖在文件头后面的元数据上，返回的长度又少了 64，文件尾被截掉。

受影响的是“写完文件尾，写位置不在末尾”的复用器：本讲实测 flv、aiff、caf、asf 四种；MP4、MKV、TS、WAV、FLAC 写完都回到末尾，没有问题。
这是 libavformat 自己的问题，不是示例的问题，补丁是 **0084**：关闭之前先 flush，再把位置移到数据末尾，然后才补零。
我们在 `upstream/master`（提交 `539da4e3d7`）上验证了：没有补丁时，flv、aiff、caf、asf 都少 64 字节；打上补丁以后，四种都和写文件逐字节相同，MP4、MKV、WAV 的输出不变。

补丁合并之前，用到旧版本的 libavformat 怎么办？`avio_mem_out` 的 `dynend=1` 是绕开的办法：关闭之前先把位置移到末尾。

```bash
$B/avio_mem_out $I flv out/l18/x_dynend.flv mode=dyn dynend=1 bitexact=1 | tail -1
cmp out/l18/x_file.flv out/l18/x_dynend.flv && echo "dynend=1 与写文件相同"
```

```text
复制了 342 个包，内存里共 564393 字节
dynend=1 与写文件相同
```

代码是：

```c
uint8_t *tmp;
int size = avio_get_dyn_buf(pb, &tmp);   /* 只为拿到当前数据的长度，不关闭 */
avio_seek(pb, size, SEEK_SET);           /* 把写位置移到末尾 */
size = avio_close_dyn_buf(pb, &buf);     /* 再关闭，补的零就在末尾了 */
```

（不要用 `avio_seek(pb, 0, SEEK_END)`：`avio_seek()` 不接受 `SEEK_END`，返回 `AVERROR(EINVAL)`，什么也没做，实测 flv 仍然被弄坏。）
也可以直接用自己的写回调，不受这个问题影响：第 7 步里 `cb` 和写文件一直是逐字节相同的。

## 第 10 步：出错会怎样，收尾的顺序

**写回调返回错误。** 把 `avio_mem_out.c` 的 `write_cb()` 改一下：累计超过 100000 字节时返回 `AVERROR(ENOSPC)`，模拟磁盘满：

```bash
sed '/^static int write_cb/,/^}/ s/    size_t end = s->pos + buf_size;/    size_t end = s->pos + buf_size;\n    if (end > 100000) return AVERROR(ENOSPC);/' \
    L18_avio_callbacks/avio_mem_out.c > out/l18/full.c
gcc -std=c11 -g $(pkg-config --cflags libavformat libavcodec libavutil) out/l18/full.c -o out/l18/full \
    $(pkg-config --libs libavformat libavcodec libavutil)
for f in matroska mp4; do out/l18/full $I $f out/l18/full.$f 2>&1 | tail -2; done
```

```text
写包失败：No space left on device
出错：No space left on device
写包失败：No space left on device
出错：No space left on device
```

错误一次就“锁住”了：`AVIOContext` 记下第一个错误，后面所有的写都直接返回这个错误。我们的程序在 `av_interleaved_write_frame()` 的返回值里看到它，退出码 1。
**写回调的返回值要当真**：回调里发生的错，只能通过返回值告诉 libavformat。

**收尾顺序。** 读和写的收尾，都要记住一个规则：**自己建的 `AVIOContext` 自己释放**。

| | 读（`avio_mem_in`、官方示例） | 写（`avio_mem_out` 的 `mode=cb`） |
| --- | --- | --- |
| 1 | `avformat_close_input(&fmt)`：不会释放 `pb`（`avformat_open_input()` 自动设了 `AVFMT_FLAG_CUSTOM_IO`） | `av_write_trailer()`，然后 `avio_flush()`：剩下的数据交给写回调 |
| 2 | `av_freep(&avio->buffer)`：释放的是 `avio->buffer`，不是自己 `av_malloc()` 的那个指针，libavformat 可能已经换掉了 | `avformat_free_context(ofmt)`：同样不会碰 `pb` |
| 3 | `avio_context_free(&avio)`：只释放 `AVIOContext` 本身 | 同左：先 `av_freep(&avio->buffer)`，再 `avio_context_free()` |
| 4 | 最后才释放数据（`av_file_unmap()`） | 最后释放攒下的数据 |

写的一边多一步：**`avio_flush()` 要在 `av_write_trailer()` 之后做，数据才完整**（`avio_mem_out` 里 `cb` 模式在存文件之前调用）。
`avio_close_dyn_buf()`、`avio_closep()` 关闭的时候会自己 flush；我们自己的 `AVIOContext` 没有关闭这一步，所以要自己 flush。

十几种错误的路径（文件不存在、不是媒体文件、不认识的输出格式、写满、没有 seek 的 MP4、输出目录不存在……）用 `valgrind` 全部检查过，
没有内存错误，没有泄漏：

```bash
valgrind -q --leak-check=full --error-exitcode=9 $B/avio_mem_in samples/small_360p25.mp4 seek=0 >/dev/null 2>&1; echo "退出码 $?"
valgrind -q --leak-check=full --error-exitcode=9 $B/avio_mem_out $I mp4 out/l18/v.mp4 seek=0 >/dev/null 2>&1; echo "退出码 $?"
```

```text
退出码 1
退出码 1
```

退出码 1 是程序自己的“出错”（读包失败、写文件头失败），`valgrind` 的“有问题”是 9。

## 和直接读写文件比一比

`compare_file.sh` 把以上所有的结论一次验证：

```bash
L18_avio_callbacks/compare_file.sh
```

```text
== 输入：包的个数（avio_mem_in 和 ffprobe）
相同  small_360p25.mp4 有 seek 回调  （125/217）
已知  small_360p25.mp4 没有 seek 回调  （读不出包，预期如此）
相同  small_360p25.mkv 有 seek 回调  （125/217）
……
== 输出：写回调、dyn_buf 和写文件，逐字节比较（bitexact=1）
相同  mp4 写回调
相同  mp4 dyn_buf
……
已知  flv dyn_buf  （比写文件短 64 字节；加 dynend=1 后相同，见 patch 0084）
……
相同 28 项；已知的、符合预期的差别 11 项；不同 0 项
```

“已知”的 11 项：MP4 没有 seek 读不出包（1）、MP4 从管道读（1）、没有 seek 回调写出来的和写文件不同（matroska、flv、mp4，3）、
dyn_buf 写 flv、aiff、caf、asf（4）、密钥不对、不解密（2）。逐项都有解释，没有“不明原因的不同”。

## 常见错误

1. **读回调在文件尾返回 0。** 不是错误也不是 EOF，MKV、FLV 会卡死，没有任何提示。要返回 `AVERROR_EOF`（第 5 步）。
2. **没有 seek 回调，却读 MP4。** `moov` 在文件尾的 MP4 读不出包，而且**函数都返回 0、不报错**，只能自己检查流的参数完整不完整（第 2 步）。
3. **假设读回调的 `buf_size` 等于缓冲区大小。** 不是：解复用器读一块指定长度的数据时，`buf_size` 可以是任意值（第 3 步：8912、4481）。
4. **seek 回调返回 0 当“成功”。** 返回的是**新的位置**，`seek_buffer()` 里 `return pos;`。`AVSEEK_SIZE` 返回的是总大小。
5. **seek 回调忘了去掉 `AVSEEK_FORCE`。** `whence` 可能是 `SEEK_SET | AVSEEK_FORCE`，不去掉的话 `switch` 匹配不上。
6. **写回调只会往后追加。** MP4、MKV、FLV 要回头改写，写回调要用“当前位置”，盖在旧数据上（第 7 步）。
7. **写回调写不满就返回。** 要么全部写完返回 `buf_size`，要么返回负数。返回小一点的正数，libavformat 当作全写完了，数据悄悄丢了（练习 5）。
8. **`AVIOContext` 的缓冲区不是 `av_malloc()` 的。** 必须是 `av_malloc()` 分配，libavformat 可能 `av_free()` 它或者换掉它。
9. **释放了自己的 `iobuf` 指针，而不是 `avio->buffer`。** libavformat 换过缓冲区的话，前者是已经释放过的指针（第 10 步）。
10. **写完文件尾，不 `avio_flush()`。** 缓冲区里剩下的数据还没交给写回调，输出少一截。
11. **用 `avio_open_dyn_buf()` 写 flv、aiff、caf、asf。** 在 patch 0084 合并之前，结果少 64 字节并且文件头被破坏（第 9 步）。
12. **加密、解密的位置不对。** 密钥要按文件里的位置算，不是按读了多少字节算，否则 seek 一下就解错了（第 8 步）。

## 练习

1. 对 `samples/small_360p25.mkv` 用 `seek=0` 和 `seek=1` 各跑一次 `avio_mem_in packets=0`，什么变了、什么没变？为什么？
2. 把读回调里的 `AVERROR_EOF` 改成 `return 0`（第 5 步），对 `samples/audio_44k_stereo.mp2` 和 `samples/video_360p25.m1v` 各跑一次（`timeout 10`）。哪个卡死？
3. 把 `avio_mem_in.c` 里 seek 回调对 `AVSEEK_SIZE` 的回答改成 `return AVERROR(ENOSYS);`（不管知不知道大小），对 MP4 和 MKV 各跑一次，结果有没有变？`seek` 的次数呢？
4. `avio_mem_out` 用 `mode=cb seek=0` 写 MKV 和 FLV，和 `seek=1` 比，`ffprobe` 看到的时长、文件大小、包数各是什么？
5. 把 `avio_mem_out.c` 的 `write_cb()` 最后的 `return buf_size;` 改成 `return buf_size > 1 ? buf_size - 1 : buf_size;`（只报告写了一部分），写 MKV，会怎样？
   再试试累计写到 100000 字节以后返回 `AVERROR(ENOSPC)`（第 10 步），MKV 和 MP4 报的错一样吗？

<details>
<summary>参考答案</summary>

1. 时长都是 5.038 秒，读回调 5 次共 22051 字节，流参数都完整，这些没变。变的是：`seek=1` 有“码率 891 kbit/s”，`seek=0` 没有码率；
   `seek=1` 问了 4 次大小，`seek=0` 是 0 次。码率是“文件大小 ÷ 时长”算出来的，没有 seek 回调，`avio_size()` 返回 `AVERROR(ENOSYS)`，libavformat 不知道文件多大。
   MKV 的时长写在文件头的 `Segment Info` 里，不用 seek。
2. `audio_44k_stereo.mp2` 正常结束，`video_360p25.m1v` 卡死（`timeout` 返回 124）。MKV、FLV、裸 MPEG-1 视频读到末尾以后，解复用器一直重试，
   MP4、TS、MP2 的解复用器碰巧用了别的办法判断结束（比如已知文件大小）。所以“能不能碰巧通过”不能当成可以返回 0 的依据。
3. 结果都一样：MP4 照样 6 次读、`yuv420p`、码率 901；MKV 照样码率 891。变的是 `seek` 的次数：MP4 从 2 次变成 12 次，MKV 从 0 次变成 8 次。
   libavformat 的 `avio_size()` 在 `AVSEEK_SIZE` 失败后，改用 `seek(-1, SEEK_END)` 得到文件大小，再 seek 回原来的位置，每问一次多出两次 seek（MP4 问了 5 次，多 10 次）。
   所以支持 `AVSEEK_SIZE` 更省事，不支持也不会出错，只是慢一点。
4. MKV：`seek=1` 561036 字节，时长 5.038；`seek=0` 561014 字节，时长 N/A。FLV：大小都是 564393 字节，`seek=1` 时长 5.095，`seek=0` 是 4.960。
   包数都是 125 和 217，不变。没有 seek 回调时，文件头里要回填的字段（时长、大小）没有回填，包本身没有受影响。
5. 返回 `buf_size - 1` 以后，MKV 的输出没有任何变化，程序没有报错，输出还是 561109 字节：libavformat 的 `writeout()` 只检查返回值是不是负数，小于 `buf_size` 的正数不会重试，也不会报错。
   所以写回调要么写完，要么返回负数（想“少写一点以后再试”，要在回调里自己循环到写完）。
   累计超过 100000 字节返回 `ENOSPC`：MKV 和 MP4 都是 `写包失败：No space left on device`，程序退出码 1。错误在第一次发生的写上被记下，以后的写直接返回同样的错误。
   用 `valgrind` 检查，没有内存错误。

</details>

## 下一讲

L19 接着自定义 I/O 往“协议”走：数据不在内存里，在网络上、在目录里。官方示例 `avio_list_dir.c` 用 libavformat 的协议层列出一个目录的内容（本地目录、FTP 等），
`avio_http_serve_files.c` 是一个最小的 HTTP 服务器，把本地的媒体文件通过 HTTP 提供出去，用的是 `AVIOContext` 里的服务端 API。
这两个示例用到的是 `URLContext` 和 `avio_open2()` 这一层，也就是第 1 步图里的“协议层”。
另外 `avio_http_serve_files.c` 在编译时有一个 `-Wpointer-sign` 警告，L19 会一并处理。
