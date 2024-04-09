// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-FileCopyrightText: 2024 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package dev.suyu.suyu_emu.features.settings.ui.viewholder

import android.view.View
import dev.suyu.suyu_emu.databinding.ListItemSettingBinding
import dev.suyu.suyu_emu.features.settings.model.view.SettingsItem
import dev.suyu.suyu_emu.features.settings.model.view.StringInputSetting
import dev.suyu.suyu_emu.features.settings.ui.SettingsAdapter
import dev.suyu.suyu_emu.utils.ViewUtils.setVisible

class StringInputViewHolder(val binding: ListItemSettingBinding, adapter: SettingsAdapter) :
    SettingViewHolder(binding.root, adapter) {
    private lateinit var setting: StringInputSetting

    override fun bind(item: SettingsItem) {
        setting = item as StringInputSetting
        binding.textSettingName.text = setting.title
        binding.textSettingDescription.setVisible(setting.description.isNotEmpty())
        binding.textSettingDescription.text = setting.description
        binding.textSettingValue.setVisible(true)
        binding.textSettingValue.text = setting.getSelectedValue()

        binding.buttonClear.setVisible(setting.clearable)
        binding.buttonClear.setOnClickListener {
            adapter.onClearClick(setting, bindingAdapterPosition)
        }

        setStyle(setting.isEditable, binding)
    }

    override fun onClick(clicked: View) {
        if (setting.isEditable) {
            adapter.onStringInputClick(setting, bindingAdapterPosition)
        }
    }

    override fun onLongClick(clicked: View): Boolean {
        if (setting.isEditable) {
            return adapter.onLongClick(setting, bindingAdapterPosition)
        }
        return false
    }
}
