// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package dev.suyu.suyu_emu.adapters

import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.Toast
import androidx.core.content.res.ResourcesCompat
import androidx.fragment.app.FragmentActivity
import androidx.navigation.findNavController
import dev.suyu.suyu_emu.HomeNavigationDirections
import dev.suyu.suyu_emu.NativeLibrary
import dev.suyu.suyu_emu.R
import dev.suyu.suyu_emu.SuyuApplication
import dev.suyu.suyu_emu.databinding.CardSimpleOutlinedBinding
import dev.suyu.suyu_emu.model.Applet
import dev.suyu.suyu_emu.model.AppletInfo
import dev.suyu.suyu_emu.model.Game
import dev.suyu.suyu_emu.viewholder.AbstractViewHolder

class AppletAdapter(val activity: FragmentActivity, applets: List<Applet>) :
    AbstractListAdapter<Applet, AppletAdapter.AppletViewHolder>(applets) {
    override fun onCreateViewHolder(
        parent: ViewGroup,
        viewType: Int
    ): AppletAdapter.AppletViewHolder {
        CardSimpleOutlinedBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            .also { return AppletViewHolder(it) }
    }

    inner class AppletViewHolder(val binding: CardSimpleOutlinedBinding) :
        AbstractViewHolder<Applet>(binding) {
        override fun bind(model: Applet) {
            binding.title.setText(model.titleId)
            binding.description.setText(model.descriptionId)
            binding.icon.setImageDrawable(
                ResourcesCompat.getDrawable(
                    binding.icon.context.resources,
                    model.iconId,
                    binding.icon.context.theme
                )
            )

            binding.root.setOnClickListener { onClick(model) }
        }

        fun onClick(applet: Applet) {
            val appletPath = NativeLibrary.getAppletLaunchPath(applet.appletInfo.entryId)
            if (appletPath.isEmpty()) {
                Toast.makeText(
                    binding.root.context,
                    R.string.applets_error_applet,
                    Toast.LENGTH_SHORT
                ).show()
                return
            }

            if (applet.appletInfo == AppletInfo.Cabinet) {
                binding.root.findNavController()
                    .navigate(R.id.action_appletLauncherFragment_to_cabinetLauncherDialogFragment)
                return
            }

            NativeLibrary.setCurrentAppletId(applet.appletInfo.appletId)
            val appletGame = Game(
                title = SuyuApplication.appContext.getString(applet.titleId),
                path = appletPath
            )
            val action = HomeNavigationDirections.actionGlobalEmulationActivity(appletGame)
            binding.root.findNavController().navigate(action)
        }
    }
}
