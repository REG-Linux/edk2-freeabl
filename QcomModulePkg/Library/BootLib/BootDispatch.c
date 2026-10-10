/** @file BootDispatch.c

  Boot routing decisions, pure functions.  See BootDispatch.h.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifdef ESP_HOST_TEST
#include "BootDispatch.h"
#else
#include <Library/BootDispatch.h>
#endif

BOOLEAN
EspSlotBootable (
  IN BOOLEAN  BootMagicOk,
  IN BOOLEAN  DtboExists,
  IN BOOLEAN  DtboValid
  )
{
  if (!BootMagicOk) {
    return FALSE;
  }
  if (DtboExists && !DtboValid) {
    return FALSE;
  }
  return TRUE;
}

ESP_BOOT_ACTION
EspFirstAction (
  IN BOOLEAN  BootIntoFastboot,
  IN BOOLEAN  OneShotAndroid,
  IN BOOLEAN  MenuLinux
  )
{
  if (BootIntoFastboot) {
    return ESP_ACT_FASTBOOT;
  }
  if (OneShotAndroid) {
    return ESP_ACT_ANDROID;
  }
  return MenuLinux ? ESP_ACT_LINUX : ESP_ACT_ANDROID;
}

ESP_BOOT_ACTION
EspAfterLinuxAction (
  IN BOOLEAN  EfiStarted,
  IN BOOLEAN  SlotBootable,
  IN BOOLEAN  OneShotSet
  )
{
  /* The EFI app ran and came back: never fall into Android behind its back. */
  if (EfiStarted) {
    return ESP_ACT_FASTBOOT;
  }
  /* Nothing to boot on the card: Android via a clean reboot, if possible. */
  if (SlotBootable && OneShotSet) {
    return ESP_ACT_REBOOT_ANDROID;
  }
  return ESP_ACT_FASTBOOT;
}

ESP_BOOT_ACTION
EspAndroidAction (
  IN BOOLEAN  SlotBootable
  )
{
  return SlotBootable ? ESP_ACT_ANDROID : ESP_ACT_FASTBOOT;
}
