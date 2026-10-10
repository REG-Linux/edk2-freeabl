/** @file BootDispatch.h

  Boot routing decisions for the ESP-enabled LinuxLoader, as pure functions
  (no I/O) so they can be unit-tested on a host PC.

  Every boot ends in exactly one of: the EFI chain (GRUB) on the SD card,
  the stock Android path on a slot that looks bootable, a reboot that goes
  straight to Android, or fastboot.  The Android path is never entered on a
  slot whose boot/dtbo partitions cannot boot, so a non-bootable slot never
  gets its A/B retry counter burned or is marked unbootable.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __BOOT_DISPATCH_H__
#define __BOOT_DISPATCH_H__

#ifdef ESP_HOST_TEST
#include <stdint.h>
typedef uint8_t  BOOLEAN;
#define TRUE  1
#define FALSE 0
#define IN
#define VOID void
#else
#include <Uefi.h>
#endif

typedef enum {
  ESP_ACT_FASTBOOT = 0,      /* enter fastboot                              */
  ESP_ACT_LINUX,             /* SD init + chain-load BOOTAA64.EFI (GRUB)    */
  ESP_ACT_ANDROID,           /* stock CLO Android path (LoadImageAndAuth)   */
  ESP_ACT_REBOOT_ANDROID     /* set one-shot flag, reboot, next boot: Android */
} ESP_BOOT_ACTION;

/**
  Can the active slot's Android path possibly boot?
  BootMagicOk: boot_<slot> starts with "ANDROID!".
  DtboExists / DtboValid: dtbo_<slot> partition present / starts with a
  DTBO table.  A missing dtbo partition is acceptable; a present but
  invalid (e.g. erased) one is not.
**/
BOOLEAN
EspSlotBootable (
  IN BOOLEAN  BootMagicOk,
  IN BOOLEAN  DtboExists,
  IN BOOLEAN  DtboValid
  );

/**
  First decision of a boot.
  BootIntoFastboot: key combo / reboot reason asked for fastboot.
  OneShotAndroid:   previous boot requested Android for this boot.
  MenuLinux:        boot-mode menu result (only meaningful when the menu
                    was shown, i.e. neither of the above).
**/
ESP_BOOT_ACTION
EspFirstAction (
  IN BOOLEAN  BootIntoFastboot,
  IN BOOLEAN  OneShotAndroid,
  IN BOOLEAN  MenuLinux
  );

/**
  Decision after the Linux (EFI chain) attempt.
  EfiStarted:   an EFI image was found and started (and has returned).
  SlotBootable: EspSlotBootable() for the active slot.
  OneShotSet:   the one-shot "Android next boot" flag was stored.
**/
ESP_BOOT_ACTION
EspAfterLinuxAction (
  IN BOOLEAN  EfiStarted,
  IN BOOLEAN  SlotBootable,
  IN BOOLEAN  OneShotSet
  );

/** Decision right before the Android path. **/
ESP_BOOT_ACTION
EspAndroidAction (
  IN BOOLEAN  SlotBootable
  );

#endif /* __BOOT_DISPATCH_H__ */
