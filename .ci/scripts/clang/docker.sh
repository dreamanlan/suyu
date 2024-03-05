#!/bin/bash -ex

# SPDX-FileCopyrightText: 2024 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
# 
# Modified by AMA25 on 3/5/24

# Exit on error, rather than continuing with the rest of the script.
set -e

ccache -s

mkdir build || true && cd build
cmake .. \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
      -DCMAKE_CXX_COMPILER=/usr/lib/ccache/clang++ \
      -DCMAKE_C_COMPILER=/usr/lib/ccache/clang \
      -DCMAKE_INSTALL_PREFIX="/usr" \
      -DDISPLAY_VERSION=$1 \
      -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=ON \
      -DENABLE_QT_TRANSLATION=ON \
      -DUSE_DISCORD_PRESENCE=ON \
      -DSUYU_CRASH_DUMPS=ON \
      -DSUYU_ENABLE_COMPATIBILITY_REPORTING=${ENABLE_COMPATIBILITY_REPORTING:-"OFF"} \
      -DSUYU_USE_BUNDLED_FFMPEG=ON \
      -GNinja

ninja

ccache -s

ctest -VV -C Release

