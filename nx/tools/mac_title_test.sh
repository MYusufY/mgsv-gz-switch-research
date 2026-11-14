#!/bin/bash
# Quick native-renderer check on the title screen (a lit 3D scene behind the logo, reached ~35 s after launch with no
# input). Runs the Mac test build for SECONDS (default 50), dumps a frame every 5 s into OUT, and converts the last one
# to OUT/last.png. Extra arguments go to the game; environment passes through.
#   nx/tools/mac_title_test.sh <out dir> [--sa2_... flags]
set -u
OUT=${1:?out dir}; shift
SECONDS_RUN=${SECONDS_RUN:-50}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$ROOT/nx/out/mac"
MGSV_FRAME_DUMP_SECONDS=5 MGSV_FRAME_DIR="$OUT" python3 -c "
import subprocess,sys
p=subprocess.Popen(['./mgsv_gz','--game_data_root=$ROOT/extracted','--sa2_renderer_native=true']+sys.argv[1:],stdout=open('$OUT/run.log','wb'),stderr=subprocess.STDOUT)
try: p.wait(timeout=$SECONDS_RUN)
except subprocess.TimeoutExpired: p.kill(); p.wait()
" "$@"
python3 - "$OUT" <<'PY'
import glob,sys,zlib,struct
fs=sorted(glob.glob(sys.argv[1]+'/*.ppm'))
if not fs: sys.exit('no frames')
d=open(fs[-1],'rb').read().split(b'\n',3);w,h=map(int,d[1].split());px=d[3]
W,H=w//2,h//2
rows=[b'\x00'+b''.join(px[(2*y)*w*3+x*6:(2*y)*w*3+x*6+3] for x in range(W)) for y in range(H)]
ch=lambda t,b: struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b))
open(sys.argv[1]+'/last.png','wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+ch(b'IDAT',zlib.compress(b''.join(rows)))+ch(b'IEND',b''))
print(len(fs),'frames; last ->',sys.argv[1]+'/last.png')
PY
