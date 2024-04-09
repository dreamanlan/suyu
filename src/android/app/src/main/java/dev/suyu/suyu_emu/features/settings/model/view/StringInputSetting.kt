// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package dev.suyu.suyu_emu.features.settings.model.view

import androidx.annotation.StringRes
import dev.suyu.suyu_emu.features.settings.model.AbstractStringSetting

class StringInputSetting(
    setting: AbstractStringSetting,
    @StringRes titleId: Int = 0,
    titleString: String = "",
    @StringRes descriptionId: Int = 0,
    descriptionString: String = ""
) : SettingsItem(setting, titleId, titleString, descriptionId, descriptionString) {
    override val type = TYPE_STRING_INPUT

    fun getSelectedValue(needsGlobal: Boolean = false) = setting.getValueAsString(needsGlobal)

    fun setSelectedValue(selection: String) =
        (setting as AbstractStringSetting).setString(selection)
}
