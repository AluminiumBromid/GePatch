#!/usr/bin/env bash
# Host test of the XMB boot-video pass-through (ge_list.c), with ASan/UBSan.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HERE/build"
gcc -O1 -g -Wall -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -fsanitize=undefined,address \
  -I"$HERE/host" -I"$HERE/.." -o "$HERE/build/xmb_video_test" "$HERE/xmb_video_test.c" "$HERE/../ge_list.c"
"$HERE/build/xmb_video_test"
