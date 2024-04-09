// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package dev.suyu.suyu_emu.model

import android.os.Parcelable
import kotlinx.parcelize.Parcelize

@Parcelize
data class GameDir(
    val uriString: String,
    var deepScan: Boolean
) : Parcelable
