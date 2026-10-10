/** @file BootModeMenu.h
    Boot mode persistence via UEFI runtime variable.
    Stores a boolean "Linux mode" flag in non-volatile storage.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __BOOT_MODE_MENU_H__
#define __BOOT_MODE_MENU_H__

#include <Uefi.h>

/**
  Read the persisted boot mode.

  @retval TRUE   Linux mode (boot to ESP/GRUB).
  @retval FALSE  Android mode (normal Qualcomm boot).
                 Also returned if the variable does not exist yet
                 (default = Linux).
**/
BOOLEAN
GetBootModeLinux (
  VOID
  );

/**
  Persist the boot mode selection.

  @param[in] LinuxMode  TRUE for Linux, FALSE for Android.

  @retval EFI_SUCCESS  Variable written.
  @retval other        SetVariable failure.
**/
EFI_STATUS
SetBootModeLinux (
  IN BOOLEAN  LinuxMode
  );

/**
  Display the boot mode selection menu.

  Shows "Android", "Linux (ESP)", and optionally "Alternate Slot".
  Pre-selects the current persisted mode with 3-second auto-timeout.
  Vol Up/Down navigate, Power confirms.

  If "Alternate Slot" is selected on a multi-slot device, the active
  slot is switched and the device reboots (does not return).

  @retval TRUE   Linux (ESP) mode selected.
  @retval FALSE  Android mode selected.
**/
BOOLEAN
DisplayBootModeMenu (
  VOID
  );

/** Choice from the boot menu shown at every power-on. **/
typedef enum {
  ESP_BOOT_ANDROID = 0,
  ESP_BOOT_LINUX,
  ESP_BOOT_FASTBOOT,
} ESP_BOOT_CHOICE;

/**
  Big-font boot menu (CLO DrawMenu style): Linux (SD card) / Android /
  Fastboot. Vol keys move, Power confirms; after a few seconds without a key
  the default is booted: Android if PreferAndroid, else the saved mode if
  the firmware keeps it, else Linux.
  Needs no UEFI variable to work (the AYN Odin firmware does not keep ours).

  @param[in] PreferAndroid  Pre-select Android (Linux was just not found).

  @return  The selected entry.
**/
ESP_BOOT_CHOICE
DisplayBootChoiceMenu (
  IN BOOLEAN  PreferAndroid
  );

/** System stats helpers — used by both BootModeMenu and FastbootMenu. **/
CONST CHAR8 * GetSocName (IN UINT32 ChipId);
UINT32 GetDdrSizeGB (VOID);

#endif /* __BOOT_MODE_MENU_H__ */
