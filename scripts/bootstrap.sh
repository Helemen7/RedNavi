#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Fetch the MacKernelSDK (kernel headers and kext libraries for building on any macOS version).
# GitHub runners occasionally drop the connection, so the clone is retried.
set -e
if [ ! -d MacKernelSDK ]; then
    tries=0
    until git clone --depth 1 https://github.com/acidanthera/MacKernelSDK.git; do
        tries=$((tries + 1))
        rm -rf MacKernelSDK
        if [ "$tries" -ge 4 ]; then
            echo "could not clone MacKernelSDK after $tries attempts" >&2
            exit 1
        fi
        echo "clone failed, retrying in $((tries * 10)) s" >&2
        sleep $((tries * 10))
    done
fi
echo "MacKernelSDK ready"
