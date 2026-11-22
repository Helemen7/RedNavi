#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Runs every Metal test of mtl_init_harness on the installed plug-in, each with its .metallib from this folder and a 60 s limit. Run as: sh run_all_tests.sh 2>&1 | tee tests.txt
cd "$(dirname "$0")"
D=$(pwd)
V=/Library/GPUBundles/RedNaviMTLDriver.bundle/Contents/Resources/vk
run() {
  name=$1; shift
  echo "== $name"
  ( env RN_METAL_COMPUTE=1 RN_VK_DIR=$V VK_ICD_FILENAMES=$V/rn_icd.json "$@" ./mtl_init_harness /Library/GPUBundles/RedNaviMTLDriver.bundle 2>&1 & p=$!; ( sleep 60; kill -9 $p 2>/dev/null && echo "TIMEOUT after 60 s" >&2 ) >/dev/null & wait $p; echo "exit $?" ) | grep -v '^objc\[' | grep -v 'implemented in both'
}
echo "rednavi_gfx_queue=1 in boot-args: $(sysctl -n kern.bootargs | tr " " "\n" | grep -c "^rednavi_gfx_queue=1$"); gfx-queue: $(ioreg -rn RedNaviProbe -l | grep -o "\"gfx-queue\" = [^}]*}" | head -1)"
run COMPUTE   RN_COMPUTE_TEST=1 RN_VK_SELFTEST=1
run AIR       RN_AIR_TEST=$D/vk_dispatch.metallib RN_AIR_FUNC=dispatch_add
run TRI       RN_TRI_TEST=$D/vk_tri.metallib
run QUAD      RN_QUAD_TEST=$D/vk_quad.metallib
run DEPTH     RN_DEPTH_TEST=$D/vk_tri.metallib
run BLIT      RN_BLIT_TEST=1
run RASTER    RN_RASTER_TEST=$D/vk_quad.metallib
run LOADSTORE RN_LOADSTORE_TEST=$D/vk_quad.metallib
run FORMAT    RN_FORMAT_TEST=1
run VIEW      RN_VIEW_TEST=$D/vk_texcopy.metallib
run FC        RN_FC_TEST=$D/vk_fc.metallib
run TEX       RN_TEX_TEST=$D/vk_texcopy.metallib
run IOSURFACE RN_IOSURFACE_TEST=$D/vk_quad.metallib
run EVENT     RN_EVENT_TEST=1
