#!/bin/bash

mkdir build
cd build

export Qt5_DIR="/opt/homebrew/opt/qt@5/lib/cmake"

cmake .. -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo -Dsuyu_USE_BUNDLED_VCPKG=OFF -Dsuyu_TESTS=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_LIBUSB=OFF

ninja