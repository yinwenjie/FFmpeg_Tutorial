#!/usr/bin/env bash
# 把 transcode_aac_av 和 ffmpeg 命令行转码同一个输入的结果逐字节比较。
#
# 用法（在仓库根目录）：
#   make -C L17_transcode_aac
#   L17_transcode_aac/compare_cli.sh
#
# 输入文件都由 samples/ 里的素材用 ffmpeg 现做，放在 out/l17_cmp/ 下。
# 每个组合打印一行“相同”或“不同”，最后汇总。

set -u
cd "$(dirname "$0")/.."

B=L17_transcode_aac/build/transcode_aac_av
D=out/l17_cmp
S=samples
mkdir -p "$D"

if [ ! -x "$B" ]; then
    echo "找不到 $B，请先 make -C L17_transcode_aac" >&2
    exit 1
fi
if [ ! -f "$S/audio_44k_stereo.mp2" ]; then
    echo "找不到 $S/audio_44k_stereo.mp2，请先运行 scripts/gen_samples.sh" >&2
    exit 1
fi

# ---- 准备输入 ------------------------------------------------------------
ff() { ffmpeg -v error -y "$@" || { echo "ffmpeg 命令失败：$*" >&2; exit 1; }; }

MP2=$S/audio_44k_stereo.mp2
ff -i $MP2 $D/in.wav
ff -i $D/in.wav $D/in.flac
ff -i $MP2 -c:a vorbis -strict -2 $D/in.ogg
ff -i $MP2 -c:a opus -strict -2 $D/in.opus
ff -i $MP2 -c:a ac3 $D/in.ac3
ff -i $MP2 -c:a wmav2 $D/in.wma
ff -i $MP2 -c:a alac $D/in_alac.m4a
ff -i $MP2 -c:a aac $D/in_aac.m4a
ff -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -c:a pcm_s16le $D/in_mono.wav
ff -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.1,aformat=channel_layouts=5.1" -c:a pcm_s16le $D/in_51.wav
for r in 192000 100000 88200 11025 4000; do
    ff -f lavfi -i "sine=frequency=440:sample_rate=$r:duration=2" -c:a pcm_s16le $D/in_$r.wav
done
ff -f lavfi -i color=c=red:s=64x64 -frames:v 1 $D/cover.png
ff -i $D/in.flac -i $D/cover.png -map 0:a -map 1 -c copy -disposition:v attached_pic \
    -metadata title="My Song" -metadata artist="Me" $D/tagged.flac
ff -i $S/small_360p25.mp4 -i $MP2 -i $D/in_mono.wav -map 0:v -map 1:a -map 2:a \
    -c:v copy -c:a aac -shortest $D/multi.mkv

# ---- 比较 ----------------------------------------------------------------
same=0
diff=0
diff_list=""

# check 名字 输入 输出扩展名 [本程序的参数...] -- [命令行的参数...]
check() {
    local name=$1 in=$2 ext=$3
    shift 3
    local mine=() cli=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do mine+=("$1"); shift; done
    shift
    cli=("$@")
    # ${arr[@]+"${arr[@]}"}：数组为空时在旧版 bash 里也不报错
    "$B" "$in" "$D/$name.$ext" ${mine[@]+"${mine[@]}"} >/dev/null 2>&1
    ffmpeg -v error -y -i "$in" -vn "${cli[@]}" "$D/${name}_cli.$ext" 2>/dev/null
    if cmp -s "$D/$name.$ext" "$D/${name}_cli.$ext"; then
        same=$((same + 1)); printf '相同  %s\n' "$name"
    else
        diff=$((diff + 1)); diff_list="$diff_list $name"; printf '不同  %s\n' "$name"
    fi
}

AAC=(-c:a aac -b:a 96k -ac 2)

echo "== 不同的输入"
check mp2          $MP2             m4a -- "${AAC[@]}"
check wav          $D/in.wav        m4a -- "${AAC[@]}"
check flac         $D/in.flac       m4a -- "${AAC[@]}"
check vorbis       $D/in.ogg        m4a -- "${AAC[@]}"
check opus         $D/in.opus       m4a -- "${AAC[@]}"
check ac3          $D/in.ac3        m4a -- "${AAC[@]}"
check wma          $D/in.wma        m4a -- "${AAC[@]}"
check alac         $D/in_alac.m4a   m4a -- "${AAC[@]}"
check aac          $D/in_aac.m4a    m4a -- "${AAC[@]}"
check mono         $D/in_mono.wav   m4a -- "${AAC[@]}"
check 5.1          $D/in_51.wav     m4a -- "${AAC[@]}"
check tagged_flac  $D/tagged.flac   m4a -- "${AAC[@]}"
check multi_mkv    $D/multi.mkv     m4a -- -map 0:a:0 "${AAC[@]}"
for f in mp4 mkv ts flv; do
    check video_$f $S/small_360p25.$f m4a -- "${AAC[@]}"
done

echo "== 不同的采样率（编码器不支持的，取最接近的）"
for r in 192000 100000 88200 11025 4000; do
    check rate_$r $D/in_$r.wav m4a -- "${AAC[@]}"
done

echo "== 不同的输出容器"
for e in m4a mkv ts flv avi aac; do
    check out_$e $MP2 $e -- "${AAC[@]}"
done

echo "== 不同的编码器"
check enc_ac3  $MP2 ac3  acodec=ac3 br=192000 -- -c:a ac3 -b:a 192k -ac 2
check enc_mp2  $MP2 mp2  acodec=mp2 br=128000 -- -c:a mp2 -b:a 128k -ac 2
check enc_flac $MP2 flac acodec=flac           -- -c:a flac -ac 2
check enc_pcm  $MP2 wav  acodec=pcm_s16le      -- -c:a pcm_s16le -ac 2

echo "== 不同的参数"
check ch1       $MP2 m4a ch=1 br=64000        -- -c:a aac -b:a 64k -ac 1
check ar48000   $MP2 m4a ar=48000             -- "${AAC[@]}" -ar 48000
check ar22050   $MP2 m4a ar=22050             -- "${AAC[@]}" -ar 22050
check coder     $MP2 m4a a:aac_coder=fast     -- "${AAC[@]}" -aac_coder fast
check faststart $MP2 m4a movflags=+faststart  -- "${AAC[@]}" -movflags +faststart

echo
echo "逐字节相同 $same 个，不同 $diff 个${diff_list:+：}${diff_list}"
