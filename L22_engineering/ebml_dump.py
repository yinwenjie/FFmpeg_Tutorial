#!/usr/bin/env python3
"""
列出 Matroska（.mkv/.webm）文件头部的元素：名字、占多少字节、内容的前几个字节。

用来回答“两个 mkv 文件为什么字节不一样”：把两个文件各列一遍，diff 一下，
哪个元素多了、少了、内容不同，一眼就看得出来（L22 README 第 6 步）。

用法：
    python3 L22_engineering/ebml_dump.py a.mkv > a.txt
    python3 L22_engineering/ebml_dump.py b.mkv > b.txt
    diff a.txt b.txt

只展开 Segment、Info、Tracks 及其里面的元素，不列 Cluster（数据包）、Cues（索引）、Tags。
每行的格式：缩进表示层次，名字，元素的总字节数（头加内容），内容的前 16 个字节（十六进制，长的不列）。
不认识的元素显示它的 ID（十六进制）。
"""
import sys

NAMES = {
    0x1A45DFA3: 'EBML', 0x18538067: 'Segment', 0x114D9B74: 'SeekHead', 0x1549A966: 'Info',
    0x1654AE6B: 'Tracks', 0xAE: 'TrackEntry', 0xE0: 'Video', 0xE1: 'Audio', 0x55B0: 'Colour',
    0x73A4: 'SegmentUID', 0x2AD7B1: 'TimecodeScale', 0x4489: 'Duration', 0x4D80: 'MuxingApp',
    0x5741: 'WritingApp', 0xD7: 'TrackNumber', 0x73C5: 'TrackUID', 0x83: 'TrackType',
    0x86: 'CodecID', 0x23E383: 'DefaultDuration', 0x9A: 'FlagInterlaced', 0x9D: 'FieldOrder',
    0x55B1: 'MatrixCoefficients', 0x55B2: 'BitsPerChannel', 0x55B7: 'ChromaSitingHorz',
    0x55B8: 'ChromaSitingVert', 0x55B9: 'Range', 0x55BA: 'TransferCharacteristics',
    0x55BB: 'Primaries', 0xB0: 'PixelWidth', 0xBA: 'PixelHeight', 0x9C: 'FlagLacing',
    0x63A2: 'CodecPrivate', 0x22B59C: 'Language', 0x9F: 'Channels', 0xB5: 'SamplingFrequency',
    0x88: 'FlagDefault', 0x56AA: 'CodecDelay', 0x56BB: 'SeekPreRoll', 0x4461: 'DateUTC',
    0x1F43B675: 'Cluster', 0x1C53BB6B: 'Cues', 0x1254C367: 'Tags', 0x1043A770: 'Chapters',
    0xEC: 'Void', 0xBF: 'CRC-32',
}
EXPAND = {'Segment', 'Info', 'Tracks', 'TrackEntry', 'Video', 'Colour', 'Audio'}
SKIP = {'Cluster', 'Cues', 'Tags', 'Void'}


def vint(data, pos, keep_marker):
    """读一个 EBML 变长整数：第一个字节里前导 0 的个数决定一共几个字节。返回（值，字节数）。"""
    first = data[pos]
    length = 1
    while length <= 8 and not (first & (0x80 >> (length - 1))):
        length += 1
    if length > 8:
        raise ValueError('位置 %d 不是合法的 EBML 变长整数' % pos)
    value = first if keep_marker else first & ((0x80 >> (length - 1)) - 1)
    for k in range(1, length):
        value = (value << 8) | data[pos + k]
    return value, length


def walk(data, pos, end, depth):
    while pos < end:
        elem_id, n = vint(data, pos, True)
        size, m = vint(data, pos + n, False)
        body = pos + n + m
        name = NAMES.get(elem_id, '0x%x' % elem_id)
        if name not in SKIP:
            shown = data[body:body + size].hex() if size <= 16 else ''
            print('%s%s %d %s' % ('  ' * depth, name, n + m + size, shown))
        if name in EXPAND:
            walk(data, body, min(body + size, end), depth + 1)
        pos = body + size


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().split('\n\n')[0], file=sys.stderr)
        print('用法：%s 文件.mkv' % sys.argv[0], file=sys.stderr)
        return 2
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    walk(data, 0, len(data), 0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
