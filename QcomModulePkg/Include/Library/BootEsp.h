/** @file BootEsp.h
    ESP partition boot chain - locate and launch BOOTAA64.EFI from
    an EFI System Partition (SD card or internal storage).

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __BOOT_ESP_H__
#define __BOOT_ESP_H__

#include <Uefi.h>

/**
  Install security protocol hooks that allow loading unsigned EFI
  binaries.  Without these, gBS->LoadImage() rejects GRUB.
**/
VOID
InstallSecurityHooks (
  VOID
  );

/**
  Early SD/MMC initialization — signal the SD detection events and
  reconnect all controllers.  Must be called early in LinuxLoaderEntry,
  BEFORE boot mode selection, to give the SDHCI driver time to
  enumerate media.
**/
VOID
InitSdCard (
  VOID
  );

/**
  Check whether any SimpleFileSystem handles exist (i.e. whether
  InitSdCard successfully triggered SD/MMC enumeration).

  @retval TRUE   At least one filesystem handle found.
  @retval FALSE  No handles found.
**/
BOOLEAN
ScanSD (
  VOID
  );

/**
  Probe every Simple File System volume for \\EFI\\BOOT\\BOOTAA64.EFI.

  @param[out] FoundVolume  Handle of the volume containing the file,
                           or NULL if not found.

  @retval TRUE   BOOTAA64.EFI was found.
  @retval FALSE  Not found on any volume.
**/
BOOLEAN
FindEspLoader (
  OUT EFI_HANDLE  *FoundVolume
  );

/**
  Load and start \\EFI\\BOOT\\BOOTAA64.EFI from the first volume
  that contains it.

  @retval EFI_SUCCESS       Image started (control may or may not return).
  @retval EFI_NOT_FOUND     BOOTAA64.EFI not present on any volume.
  @retval other             LoadImage / StartImage failure.
**/
EFI_STATUS
StartEspBootLoader (
  VOID
  );

/**
  Display the "No Linux on SD card" error screen.

  Shows an on-screen warning when no bootable EFI image is found.
  Power key or 5-second timeout returns to caller (Android boot).
**/
VOID
LinuxNotFoundScreen (
  VOID
  );

/**
  Power the panel off when the started image calls ExitBootServices.
  Does nothing when the firmware has no display power protocol.
**/
VOID
EspPanelOffAtExitBootServices (
  VOID
  );

#endif /* __BOOT_ESP_H__ */
