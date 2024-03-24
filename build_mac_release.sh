#!/bin/bash

mkdir build
cd build

export Qt5_DIR="/opt/homebrew/opt/qt@5/lib/cmake"

export LIBVULKAN_PATH=/opt/homebrew/lib/libvulkan.dylib

cmake .. -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSUYU_USE_BUNDLED_VCPKG=OFF -DSUYU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF -DCLANG_FORMAT=ON -DSDL2_DISABLE_INSTALL=ON -DSDL_ALTIVEC=ON

ninja
