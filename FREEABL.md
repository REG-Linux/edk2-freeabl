# edk2-freeabl

Qualcomm/CodeLinaro edk2 `uefi.lnx.5.0.r34-rel` (LinuxLoader, the Android
bootloader "ABL") with the FreeABL changes on top: boot Linux from the SD
card through an EFI loader (GRUB), next to the unchanged Android boot.

This tree is the `edk2` submodule of
[FreeABL](https://github.com/REG-Linux/FreeABL), which holds the build
scripts, signing, tests and documentation. Build from there.

Branch `freeabl`:
1. `Import CLO edk2 uefi.lnx.5.0.r34-rel`: the CodeLinaro tree, unmodified.
2. The FreeABL changes, one commit per topic.

## Modified upstream files

FreeABL modifies some CodeLinaro / TianoCore files (each carries a
"Modified for FreeABL" line under its original notice): the build
configuration (`BaseTools`, `Conf`, `makefile`, `QcomModulePkg.dsc/.dec`),
LinuxLoader, the boot library (boot flow, command line, device tree,
fastboot menu, menu keys, shutdown services), fastboot commands and
verified boot (no warning screens). The commit history shows every change.

## Licensing

- Files written for FreeABL carry `SPDX-License-Identifier: GPL-3.0-or-later`
  (text in `COPYING.GPL-3.0`).
- Files from CodeLinaro / Qualcomm and from TianoCore EDK2 keep their original
  licence notices (BSD-3-Clause, BSD-2-Clause-Patent and others), including
  the ones FreeABL modifies.
- `EmbeddedPkg/Library/LibUfdt` is AOSP's libufdt, licensed under Apache-2.0
  (its files carry no headers in the CodeLinaro tree).
- A built ABL image combines both and is distributed under GPL-3.0-or-later.
