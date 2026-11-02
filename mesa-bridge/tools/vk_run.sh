#!/bin/sh
# usage: ./run.sh [vulkaninfo args]   (run from this directory)
D="$(cd "$(dirname "$0")" && pwd)"
export DYLD_LIBRARY_PATH="$D" VK_ICD_FILENAMES="$D/rn_icd.json" VK_LOADER_DEBUG=${VK_LOADER_DEBUG:-error,warn}
exec "$D/vulkaninfo" "$@"
