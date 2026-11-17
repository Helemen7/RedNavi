#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors
#
# Runs the OpenGL probe (tools/gl_probe.m) on the test machine and collects what the GL renderer says (docs/OPENGL_PLAN.md).
# Prerequisite: the machine booted with rednavi_accel2_exp including bit 64 (IOGLBundleName = AppleMetalOpenGLRenderer), the
# accelerator kext and the Metal plug-in installed as for the M6 tests. This script changes nothing on the machine except ~/rn/gl.
#
#     scripts/gl_experiment.sh [user@host]       default helemen7@mac-desktop.lab
set -eu
HOST=${1:-helemen7@mac-desktop.lab}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/rn_gl_$$
mkdir -p "$OUT"
clang -O1 -fobjc-arc -mmacosx-version-min=12 -framework Foundation -framework OpenGL -framework CoreFoundation \
    -Wno-deprecated-declarations "$HERE/tools/gl_probe.m" -o "$OUT/gl_probe"
ssh "$HOST" 'mkdir -p ~/rn/gl'
scp -q "$OUT/gl_probe" "$HOST:rn/gl/gl_probe"
echo "== registry: GL bundle name on the accelerator"
ssh "$HOST" 'ioreg -rc IOAccelerator -l | grep -E "IOGLBundleName|MetalPluginName|IOClass"' || true
echo "== gl_probe (run in the desktop session if SSH is refused by the window server)"
ssh "$HOST" 'cd ~/rn/gl && ./gl_probe 2>&1 | tee gl_probe.log; echo exit=$?' || true
echo "== GL / Metal messages of the last two minutes"
ssh "$HOST" "log show --last 2m --style compact --predicate 'process == \"gl_probe\" OR eventMessage CONTAINS \"GLDRenderer\" OR eventMessage CONTAINS \"AppleMetalOpenGLRenderer\"' 2>/dev/null | tail -60" || true
