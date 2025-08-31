#!/bin/bash

export Qt5_DIR="/opt/homebrew/opt/qt@5/lib/cmake"

mkdir -p xcodebuild
cd xcodebuild

cmake .. -GXcode -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Dyuzu_USE_BUNDLED_VCPKG=OFF -Dyuzu_TESTS=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=ON

cd ..

# mkdir -p build
# cd build

# cmake .. -GNinja -DCMAKE_BUILD_TYPE=Debug -Dyuzu_USE_BUNDLED_VCPKG=OFF -Dyuzu_TESTS=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF

# ninja
