/** @file EspHarness.c

  Test harness (QEMU / AAVMF): runs the ABL's ESP boot chain from BootLib —
  the same calls LinuxLoader makes in Linux mode — and prints markers on the
  console so a host script can check each step.  Not part of the ABL image.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BootEsp.h>
#include <Library/RawFat.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/ComponentName2.h>

/* "nofs" test mode: unload the firmware FAT driver, so no SD filesystem is
   mounted - the situation where only the raw FAT fallback can find GRUB. */
STATIC VOID
UnloadFatDriver (VOID)
{
  EFI_HANDLE                    *H = NULL;
  UINTN                         N = 0, i;
  EFI_COMPONENT_NAME2_PROTOCOL  *Cn;
  CHAR16                        *Name;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiComponentName2ProtocolGuid, NULL, &N, &H))) {
    return;
  }
  for (i = 0; i < N; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (H[i], &gEfiComponentName2ProtocolGuid, (VOID **)&Cn)) ||
        EFI_ERROR (Cn->GetDriverName (Cn, "en", &Name)) ||
        StrStr (Name, L"FAT") == NULL) {
      continue;
    }
    Print (L"ESPTEST: unloading driver '%s': %r\n", Name, gBS->UnloadImage (H[i]));
  }
  FreePool (H);
}

EFI_STATUS
EFIAPI
EspHarnessMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  VOID        *Data = NULL;
  UINTN       Size  = 0;
  BOOLEAN     Found;
  EFI_HANDLE  Vol = NULL;

  Print (L"ESPTEST: start\n");
  {
    EFI_LOADED_IMAGE_PROTOCOL *Li;
    if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li)) &&
        Li->LoadOptions != NULL && StrStr ((CHAR16 *)Li->LoadOptions, L"nofs") != NULL) {
      UnloadFatDriver ();
    }
  }

  InstallSecurityHooks ();
  Print (L"ESPTEST: security hooks done\n");

  InitSdCard ();
  Print (L"ESPTEST: InitSdCard done\n");

  Print (L"ESPTEST: ScanSD=%d\n", ScanSD ());

  Found = FindEspLoader (&Vol);
  Print (L"ESPTEST: FindEspLoader=%d vol=%p\n", Found, Vol);

  Status = RawFatReadFile (NULL, L"\\EFI\\BOOT\\BOOTAA64.EFI", &Data, &Size);
  Print (L"ESPTEST: RawFatReadFile BOOT=%r size=%u\n", Status, Size);
  if (Data != NULL) {
    FreePool (Data);
  }

  Status = StartEspBootLoader ();
  Print (L"ESPTEST: StartEspBootLoader returned %r\n", Status);
  return Status;
}
