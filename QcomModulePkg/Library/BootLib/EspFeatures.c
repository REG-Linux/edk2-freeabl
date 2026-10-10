/** @file EspFeatures.c
    ESP feature NV variables and kernel cmdline builder.

    Provides persistent get/set helpers for GPU preemption, HW fence,
    audio framework, and silent boot mode.  Each value is stored as a
    UEFI NV runtime variable under gBootModeVariableGuid.

    AppendEspCmdLine() reads all variables and builds the corresponding
    kernel command-line fragment.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Library/EspFeatures.h>

/* ------------------------------------------------------------------ */
/* Variable names and shared GUID                                     */
/* ------------------------------------------------------------------ */

extern EFI_GUID gBootModeVariableGuid;

STATIC CHAR16  *mGpuPreemptionVarName  = L"GpuPreemption";
STATIC CHAR16  *mHwFenceEnableVarName  = L"HwFenceEnable";
STATIC CHAR16  *mAudioFrameworkVarName = L"AudioFramework";
STATIC CHAR16  *mSilentBootModeVarName = L"SilentBootMode";

#define ESP_NV_ATTRIBUTES \
  (EFI_VARIABLE_NON_VOLATILE | \
   EFI_VARIABLE_BOOTSERVICE_ACCESS | \
   EFI_VARIABLE_RUNTIME_ACCESS)

/* ------------------------------------------------------------------ */
/* GPU Preemption                                                     */
/* ------------------------------------------------------------------ */

BOOLEAN
GetGpuPreemption (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINTN       Size;

  Size  = sizeof (Value);
  Value = 0;

  Status = gRT->GetVariable (
                  mGpuPreemptionVarName,
                  &gBootModeVariableGuid,
                  NULL,
                  &Size,
                  &Value
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  return (Value != 0);
}

EFI_STATUS
SetGpuPreemption (
  IN BOOLEAN  Enable
  )
{
  UINT8  Value;

  Value = Enable ? 1 : 0;

  return gRT->SetVariable (
                mGpuPreemptionVarName,
                &gBootModeVariableGuid,
                ESP_NV_ATTRIBUTES,
                sizeof (Value),
                &Value
                );
}

/* ------------------------------------------------------------------ */
/* HW Fence Enable                                                    */
/* ------------------------------------------------------------------ */

BOOLEAN
GetHwFenceEnable (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINTN       Size;

  Size  = sizeof (Value);
  Value = 0;

  Status = gRT->GetVariable (
                  mHwFenceEnableVarName,
                  &gBootModeVariableGuid,
                  NULL,
                  &Size,
                  &Value
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  return (Value != 0);
}

EFI_STATUS
SetHwFenceEnable (
  IN BOOLEAN  Enable
  )
{
  UINT8  Value;

  Value = Enable ? 1 : 0;

  return gRT->SetVariable (
                mHwFenceEnableVarName,
                &gBootModeVariableGuid,
                ESP_NV_ATTRIBUTES,
                sizeof (Value),
                &Value
                );
}

/* ------------------------------------------------------------------ */
/* Audio Framework                                                    */
/* ------------------------------------------------------------------ */

EFI_STATUS
GetAudioFramework (
  OUT CHAR8  *Buffer,
  IN  UINTN  BufferSize
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  if (Buffer == NULL || BufferSize == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Buffer[0] = '\0';
  Size      = BufferSize;

  Status = gRT->GetVariable (
                  mAudioFrameworkVarName,
                  &gBootModeVariableGuid,
                  NULL,
                  &Size,
                  Buffer
                  );
  if (EFI_ERROR (Status)) {
    Buffer[0] = '\0';
    return Status;
  }

  /* Ensure NUL termination */
  if (Size < BufferSize) {
    Buffer[Size] = '\0';
  } else {
    Buffer[BufferSize - 1] = '\0';
  }

  return EFI_SUCCESS;
}

EFI_STATUS
SetAudioFramework (
  IN CONST CHAR8  *Value
  )
{
  UINTN  Len;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Len = AsciiStrLen (Value);

  return gRT->SetVariable (
                mAudioFrameworkVarName,
                &gBootModeVariableGuid,
                ESP_NV_ATTRIBUTES,
                (UINT32)(Len + 1),
                (VOID *)Value
                );
}

/* ------------------------------------------------------------------ */
/* Silent Boot Mode                                                   */
/* ------------------------------------------------------------------ */

UINT32
GetSilentBootMode (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINTN       Size;

  Size  = sizeof (Value);
  Value = 0;

  Status = gRT->GetVariable (
                  mSilentBootModeVarName,
                  &gBootModeVariableGuid,
                  NULL,
                  &Size,
                  &Value
                  );
  if (EFI_ERROR (Status)) {
    return 0;
  }

  return (Value <= 3) ? Value : 0;
}

EFI_STATUS
SetSilentBootMode (
  IN UINT32  Mode
  )
{
  if (Mode > 3) {
    return EFI_INVALID_PARAMETER;
  }

  return gRT->SetVariable (
                mSilentBootModeVarName,
                &gBootModeVariableGuid,
                ESP_NV_ATTRIBUTES,
                sizeof (Mode),
                &Mode
                );
}

/* ------------------------------------------------------------------ */
/* Cmdline builder                                                    */
/* ------------------------------------------------------------------ */

STATIC CONST CHAR8  *mSilentModeStr[] = {
  "nonsilent",
  "silent",
  "forcednonsilent",
  "forcedsilent"
};

UINT32
AppendEspCmdLine (
  OUT CHAR8   *Dst,
  IN  UINT32  MaxLen
  )
{
  UINT32   Offset;
  BOOLEAN  GpuPreempt;
  BOOLEAN  HwFence;
  UINT32   SilentMode;
  CHAR8    AudioBuf[32];

  if (Dst == NULL || MaxLen < 2) {
    return 0;
  }

  Dst[0]     = '\0';
  Offset     = 0;
  GpuPreempt = GetGpuPreemption ();
  HwFence    = GetHwFenceEnable ();
  SilentMode = GetSilentBootMode ();

  Offset += AsciiSPrint (
              Dst + Offset,
              MaxLen - Offset,
              " msm_kgsl.preempt_enable=%d",
              GpuPreempt ? 1 : 0
              );

  Offset += AsciiSPrint (
              Dst + Offset,
              MaxLen - Offset,
              " msm_hw_fence.enable=%d",
              HwFence ? 1 : 0
              );

  if (!EFI_ERROR (GetAudioFramework (AudioBuf, sizeof (AudioBuf))) &&
      AudioBuf[0] != '\0') {
    Offset += AsciiSPrint (
                Dst + Offset,
                MaxLen - Offset,
                " androidboot.audio=%a",
                AudioBuf
                );
  }

  if (SilentMode <= 3) {
    Offset += AsciiSPrint (
                Dst + Offset,
                MaxLen - Offset,
                " silent_boot.mode=%a",
                mSilentModeStr[SilentMode]
                );
  }

  return Offset;
}

/* TRUE only while a Linux kernel is being booted through the CLO boot path
   (not the GRUB/EFI chain).  Gates the ESP command-line additions so they
   never reach an Android kernel. */
STATIC BOOLEAN mEspLinuxKernelBoot = FALSE;

BOOLEAN
EspLinuxKernelBoot (
  VOID
  )
{
  return mEspLinuxKernelBoot;
}

VOID
EspSetLinuxKernelBoot (
  IN BOOLEAN  Enable
  )
{
  mEspLinuxKernelBoot = Enable;
}
