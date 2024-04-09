// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package dev.suyu.suyu_emu.model

enum class PatchType(val int: Int) {
    Update(0),
    DLC(1),
    Mod(2);

    companion object {
        fun from(int: Int): PatchType = entries.firstOrNull { it.int == int } ?: Update
    }
}
