#!/usr/bin/env bash
# PC benchmark of ge_list.c on a gecapture frame: tests/bench.sh <capture dir> [list_bench options]
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$HERE/build"
gcc -O2 -g -Wall -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I"$HERE/host" -I"$HERE/.." \
  -o "$HERE/build/list_bench" "$HERE/list_bench.c" "$HERE/../ge_list.c"
"$HERE/build/list_bench" "$@"
