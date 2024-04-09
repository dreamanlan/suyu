// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package dev.suyu.suyu_emu.fragments

import android.app.Dialog
import android.content.DialogInterface
import android.net.Uri
import android.os.Bundle
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import dev.suyu.suyu_emu.R
import dev.suyu.suyu_emu.databinding.DialogAddFolderBinding
import dev.suyu.suyu_emu.model.GameDir
import dev.suyu.suyu_emu.model.GamesViewModel
import dev.suyu.suyu_emu.model.HomeViewModel

class AddGameFolderDialogFragment : DialogFragment() {
    private val homeViewModel: HomeViewModel by activityViewModels()
    private val gamesViewModel: GamesViewModel by activityViewModels()

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        val binding = DialogAddFolderBinding.inflate(layoutInflater)
        val folderUriString = requireArguments().getString(FOLDER_URI_STRING)
        if (folderUriString == null) {
            dismiss()
        }
        binding.path.text = Uri.parse(folderUriString).path

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.add_game_folder)
            .setPositiveButton(android.R.string.ok) { _: DialogInterface, _: Int ->
                val newGameDir = GameDir(folderUriString!!, binding.deepScanSwitch.isChecked)
                homeViewModel.setGamesDirSelected(true)
                gamesViewModel.addFolder(newGameDir)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .setView(binding.root)
            .show()
    }

    companion object {
        const val TAG = "AddGameFolderDialogFragment"

        private const val FOLDER_URI_STRING = "FolderUriString"

        fun newInstance(folderUriString: String): AddGameFolderDialogFragment {
            val args = Bundle()
            args.putString(FOLDER_URI_STRING, folderUriString)
            val fragment = AddGameFolderDialogFragment()
            fragment.arguments = args
            return fragment
        }
    }
}
