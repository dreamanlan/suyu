<!--
SPDX-FileCopyrightText: 2024 yuzu Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Migrating from yuzu

When coming from yuzu, the migration is as easy as renaming some directories.

## Windows

Use the run dialog to go to `%APPDATA%` or manually go to `C:\Users\{USERNAME}\AppData\Roaming` (you may have to enable hidden files) and simply rename the `yuzu` directories and simply rename those to `yuzu`.

## Unix (macOS/Linux)
Similarly, you can simply rename the folders `~/.local/share/yuzu` and `~/.config/yuzu` to `yuzu`, either via a file manager or with the following commands:
```sh
 $ mv ~/.local/share/yuzu ~/.local/share/yuzu
 $ mv ~/.config/yuzu ~/.config/yuzu
```
There is also `~/.cache/yuzu`, which you can safely delete. Yuzu will build a fresh cache in its own directory.

### Linux
Depending on your setup, you may want to substitute those base paths for `$XDG_DATA_HOME` and `$XDG_CONFIG_HOME` respectively.

## Android
TBD