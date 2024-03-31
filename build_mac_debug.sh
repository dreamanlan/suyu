#!/bin/bash

export Qt5_DIR="/opt/homebrew/opt/qt@5/lib/cmake"

mkdir -p xcodebuild
cd xcodebuild

cmake .. -GXcode -Dsuyu_USE_BUNDLED_VCPKG=OFF -Dsuyu_TESTS=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF

cd ..
mkdir -p build
cd build

cmake .. -GNinja -DCMAKE_BUILD_TYPE=Debug -Dsuyu_USE_BUNDLED_VCPKG=OFF -Dsuyu_TESTS=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF

ninja