// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

// This file contains suyu's HLE API version constants.

namespace HLE::ApiVersion {

// Horizon OS version constants.

constexpr u8 HOS_VERSION_MAJOR = 17;
constexpr u8 HOS_VERSION_MINOR = 0;
constexpr u8 HOS_VERSION_MICRO = 1;

// NintendoSDK version constants.

constexpr u8 SDK_REVISION_MAJOR = 1;
constexpr u8 SDK_REVISION_MINOR = 0;

constexpr char PLATFORM_STRING[] = "NX";
constexpr char VERSION_HASH[] = "30dd7d0584cd38e3a1db26a5719566d21d77110e";
constexpr char DISPLAY_VERSION[] = "17.0.1";
constexpr char DISPLAY_TITLE[] = "NintendoSDK Firmware for NX 17.0.1-1.0";

// Atmosphere version constants.

constexpr u8 ATMOSPHERE_RELEASE_VERSION_MAJOR = 1;
constexpr u8 ATMOSPHERE_RELEASE_VERSION_MINOR = 0;
constexpr u8 ATMOSPHERE_RELEASE_VERSION_MICRO = 0;

constexpr u32 AtmosphereTargetFirmwareWithRevision(u8 major, u8 minor, u8 micro, u8 rev) {
    return u32{major} << 24 | u32{minor} << 16 | u32{micro} << 8 | u32{rev};
}

constexpr u32 AtmosphereTargetFirmware(u8 major, u8 minor, u8 micro) {
    return AtmosphereTargetFirmwareWithRevision(major, minor, micro, 0);
}

constexpr u32 GetTargetFirmware() {
    return AtmosphereTargetFirmware(HOS_VERSION_MAJOR, HOS_VERSION_MINOR, HOS_VERSION_MICRO);
}

} // namespace HLE::ApiVersion
