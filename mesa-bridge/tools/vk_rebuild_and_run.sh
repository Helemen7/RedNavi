#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# rebuild the ICD, push it to the desktop, run vulkaninfo there
export PATH=/private/tmp/claude-501/venv/bin:$PATH:/opt/local/bin
cd /private/tmp/claude-501/build-radv && ninja 2>&1 | grep -E "error|FAILED" | head
cp src/amd/vulkan/libvulkan_radeon.dylib /private/tmp/claude-501/rnvk/ && cd /private/tmp/claude-501/rnvk && install_name_tool -change /usr/local/opt/zstd/lib/libzstd.1.dylib @loader_path/libzstd.1.dylib libvulkan_radeon.dylib
rsync -a libvulkan_radeon.dylib helemen7@mac-desktop.lab:rn/vk/ && ssh helemen7@mac-desktop.lab "cd ~/rn/vk && xattr -cr . ; ./run.sh $* 2>&1 | head -${LINES_MAX:-80}"
