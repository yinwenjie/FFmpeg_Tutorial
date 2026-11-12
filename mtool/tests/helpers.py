#!/usr/bin/env python3
"""
run_tests.sh 用的几个小帮手：读 mtool 输出的 JSON，比较两份逐包 CRC 清单。
用法：
  helpers.py streams  x.json            打印 "视频流数 音频流数"
  helpers.py vstat    x.json 键          第一路视频流 stats 里的值（packets keyframes I P B frames rotation gop_count vfr decode_errors corrupt_frames）
  helpers.py astat    x.json 键          第一路音频流的值（packets sample_rate channels）
  helpers.py fmt      x.json 键          format 里的值（duration nb_streams format_name）
  helpers.py slice    全部.txt 一段.txt   一段的每一行是不是全部.txt 里连续的一段（逐包 CRC 的比较）
  helpers.py sheet    x.png              打印图片的宽 高（只认 PNG 和 JPEG）
"""
import json
import struct
import sys


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def first(d, kind):
    for s in d['streams']:
        if s['type'] == kind:
            return s
    return None


def main():
    cmd = sys.argv[1]
    if cmd == 'streams':
        d = load(sys.argv[2])
        print(sum(1 for s in d['streams'] if s['type'] == 'video'),
              sum(1 for s in d['streams'] if s['type'] == 'audio'))
    elif cmd == 'vstat':
        s = first(load(sys.argv[2]), 'video')
        k = sys.argv[3]
        st = s['stats']
        val = {
            'packets': lambda: st['packets'], 'keyframes': lambda: st['keyframes'],
            'I': lambda: st['frame_types']['I'], 'P': lambda: st['frame_types']['P'],
            'B': lambda: st['frame_types']['B'], 'frames': lambda: st['decoded_frames'],
            'rotation': lambda: s['rotation'], 'gop_count': lambda: st['gop']['count'],
            'vfr': lambda: st.get('variable_frame_rate'),
            'decode_errors': lambda: st['decode_errors'], 'corrupt_frames': lambda: st['corrupt_frames'],
            'width': lambda: s['width'], 'height': lambda: s['height'],
        }[k]()
        print(str(val).lower() if isinstance(val, bool) else val)
    elif cmd == 'astat':
        s = first(load(sys.argv[2]), 'audio')
        k = sys.argv[3]
        print(s['stats']['packets'] if k == 'packets' else s[k])
    elif cmd == 'fmt':
        print(load(sys.argv[2])['format'][sys.argv[3]])
    elif cmd == 'slice':
        whole = [l.strip() for l in open(sys.argv[2]) if l.strip()]
        part = [l.strip() for l in open(sys.argv[3]) if l.strip()]
        if not part:
            print('empty')
            sys.exit(1)
        for i in range(len(whole) - len(part) + 1):
            if whole[i:i + len(part)] == part:
                print('ok', i, len(part))
                sys.exit(0)
        print('not-a-slice')
        sys.exit(1)
    elif cmd == 'sheet':
        data = open(sys.argv[2], 'rb').read()
        if data[:8] == b'\x89PNG\r\n\x1a\n':
            w, h = struct.unpack('>II', data[16:24])
        elif data[:2] == b'\xff\xd8':
            i = 2
            while i < len(data):
                if data[i] != 0xff:
                    i += 1
                    continue
                m = data[i + 1]
                if m in (0xc0, 0xc1, 0xc2):
                    h, w = struct.unpack('>HH', data[i + 5:i + 9])
                    break
                i += 2 + struct.unpack('>H', data[i + 2:i + 4])[0]
            else:
                sys.exit(1)
        else:
            sys.exit(1)
        print(w, h)


if __name__ == '__main__':
    main()
