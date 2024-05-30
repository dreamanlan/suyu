#!/bin/bash -ex
# SPDX-FileCopyrightText: 2019 yuzu Emulator Project
# SPDX-FileCopyrightText: 2024 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Exit on error, rather than continuing with the rest of the script.
set -e

ccache -s

git submodule update --init --recursive

mkdir build || true && cd build
cmake .. \
      -DBoost_USE_STATIC_LIBS=ON \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
			-DSUYU_USE_PRECOMPILED_HEADERS=OFF \
			-DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF \
      -DCMAKE_CXX_FLAGS="-march=x86-64-v2" \
      -DCMAKE_CXX_COMPILER=/usr/local/bin/g++ \
      -DCMAKE_C_COMPILER=/usr/local/bin/gcc \
      -DCMAKE_INSTALL_PREFIX="/usr" \
      -DDISPLAY_VERSION=$1 \
      -DENABLE_COMPATIBILITY_LIST_DOWNLOAD=OFF \
      -DENABLE_QT_TRANSLATION=OFF \
      -DUSE_DISCORD_PRESENCE=ON \
      -DSUYU_ENABLE_COMPATIBILITY_REPORTING=${ENABLE_COMPATIBILITY_REPORTING:-"OFF"} \
      -DSUYU_USE_BUNDLED_FFMPEG=ON \
      -DSUYU_ENABLE_LTO=OFF \
      -DSUYU_CRASH_DUMPS=ON \
      -DSUYU_USE_FASTER_LD=ON \
      -GNinja

ninja

ccache -sv

ctest -VV -C Release

# Separate debug symbols from specified executables
for EXE in suyu; do
    EXE_PATH="bin/$EXE"
    # Copy debug symbols out
    objcopy --only-keep-debug $EXE_PATH $EXE_PATH.debug
    # Add debug link and strip debug symbols
    objcopy -g --add-gnu-debuglink=$EXE_PATH.debug $EXE_PATH $EXE_PATH.out
    # Overwrite original with stripped copy
    mv $EXE_PATH.out $EXE_PATH
done
# Strip debug symbols from all executables
find bin/ -type f -not -regex '.*.debug' -exec strip -g {} ';'

DESTDIR="$PWD/AppDir" ninja install
rm -vf AppDir/usr/bin/suyu-cmd AppDir/usr/bin/suyu-tester

# Download tools needed to build an AppImage
wget -nc https://git.suyu.dev/suyu/ext-linux-bin/raw/branch/main/appimage/deploy-linux.sh
wget -nc https://git.suyu.dev/suyu/ext-linux-bin/raw/branch/main/appimage/exec-x86_64.so
wget -nc https://git.suyu.dev/suyu/AppImageKit-checkrt/raw/branch/gh-workflow/AppRun

# Set executable bit
chmod 755 \
    deploy-linux.sh \
    AppRun.sh \
    exec-x86_64.so \

# Workaround for https://github.com/AppImage/AppImageKit/issues/828
export APPIMAGE_EXTRACT_AND_RUN=1

mkdir -p AppDir/usr/optional
mkdir -p AppDir/usr/optional/libstdc++
mkdir -p AppDir/usr/optional/libgcc_s

# Deploy suyu's needed dependencies
DEPLOY_QT=1 ./deploy-linux.sh AppDir/usr/bin/suyu AppDir

# Workaround for libQt5MultimediaGstTools indirectly requiring libwayland-client and breaking Vulkan usage on end-user systems
find AppDir -type f -regex '.*libwayland-client\.so.*' -delete -print

# Workaround for building suyu with GCC 10 but also trying to distribute it to Ubuntu 18.04 et al.
# See https://github.com/darealshinji/AppImageKit-checkrt
cp exec-x86_64.so AppDir/usr/optional/exec.so
cp AppRun.sh AppDir/AppRun
cp --dereference /usr/lib/x86_64-linux-gnu/libstdc++.so.6 AppDir/usr/optional/libstdc++/libstdc++.so.6
cp --dereference /lib/x86_64-linux-gnu/libgcc_s.so.1 AppDir/usr/optional/libgcc_s/libgcc_s.so.1
