#!/bin/sh
# usage: ./run_tool.sh <program in this directory> [args]
D="$(cd "$(dirname "$0")" && pwd)"
P="$1"; shift
export DYLD_LIBRARY_PATH="$D" VK_ICD_FILENAMES="$D/rn_icd.json" VK_LOADER_DEBUG=${VK_LOADER_DEBUG:-error,warn}
exec "$D/$P" "$@"
