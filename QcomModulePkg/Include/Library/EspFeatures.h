/** @file EspFeatures.h
    ESP feature variables: GPU preemption, HW fence, audio framework,
    silent boot mode.  Backed by UEFI NV runtime variables.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __ESP_FEATURES_H__
#define __ESP_FEATURES_H__

#include <Uefi.h>

/**
  Get/Set GPU preemption flag (NV variable L"GpuPreemption").
  Default when unset: FALSE (disabled).
**/
BOOLEAN
GetGpuPreemption (
  VOID
  );

EFI_STATUS
SetGpuPreemption (
  IN BOOLEAN  Enable
  );

/**
  Get/Set HW fence enable flag (NV variable L"HwFenceEnable").
  Default when unset: FALSE (disabled).
**/
BOOLEAN
GetHwFenceEnable (
  VOID
  );

EFI_STATUS
SetHwFenceEnable (
  IN BOOLEAN  Enable
  );

/**
  Get/Set audio framework string (NV variable L"AudioFramework").
  Up to 31 ASCII chars.  Default when unset: empty string.
**/
EFI_STATUS
GetAudioFramework (
  OUT CHAR8  *Buffer,
  IN  UINTN  BufferSize
  );

EFI_STATUS
SetAudioFramework (
  IN CONST CHAR8  *Value
  );

/**
  Get/Set silent boot mode (NV variable L"SilentBootMode").
  Values: 0=nonsilent, 1=silent, 2=forcednonsilent, 3=forcedsilent.
  Default when unset: 0 (nonsilent).
**/
UINT32
GetSilentBootMode (
  VOID
  );

EFI_STATUS
SetSilentBootMode (
  IN UINT32  Mode
  );

/**
  Append ESP-specific kernel cmdline parameters built from NV variables.

  Appends: msm_kgsl.preempt_enable=N msm_hw_fence.enable=N
           androidboot.audio=VALUE silent_boot.mode=MODE

  @param[out] Dst     Destination ASCII buffer.
  @param[in]  MaxLen  Size of Dst in bytes.

  @return Number of characters written (excluding NUL).
**/
UINT32
AppendEspCmdLine (
  OUT CHAR8   *Dst,
  IN  UINT32  MaxLen
  );

/**
  TRUE while a Linux kernel is booted through the CLO boot path (not the
  GRUB/EFI chain).  ESP command-line additions are applied only then.
**/
BOOLEAN
EspLinuxKernelBoot (
  VOID
  );

VOID
EspSetLinuxKernelBoot (
  IN BOOLEAN  Enable
  );

#endif /* __ESP_FEATURES_H__ */
