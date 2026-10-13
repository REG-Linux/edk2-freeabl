/** @file AndroidBuildVars.c

  The Android build on the active slot, for the FreeABL flashers: they keep
  a stock ABL backup per build and refuse to restore one under another build.
  Read as VerifiedBoot.c's GetOsVerAndSecPactchLevel does: the
  com.android.build.boot (or init_boot) properties of vbmeta<suffix>, else
  the boot (or init_boot) image header, else the vendor properties. Same
  order as web/core.js androidBuild() and tools/freeabl-flash.py in FreeABL.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/LinuxLoaderLib.h>
#include "libavb/libavb.h"
#include "AndroidBuildVars.h"

/* vbmeta partitions are 64 KiB on the devices seen; never read more than this */
#define VBMETA_MAX_BYTES (4 * 1024 * 1024)
#define BOOT_HEADER_BYTES 4096

STATIC CHAR8 OsVersion[16];
STATIC CHAR8 SecurityPatch[16];
STATIC CHAR8 VbmetaSha256[2 * AVB_SHA256_DIGEST_SIZE + 1];

/* The whole partition (up to Max bytes) in a new pool buffer, or NULL. */
STATIC UINT8 *
ReadPartition (IN CONST CHAR16 *Name, IN UINTN Max, OUT UINTN *Size)
{
  EFI_STATUS Status;
  PartiSelectFilter Filter;
  HandleInfo Handles[1];
  UINT32 Count = ARRAY_SIZE (Handles);
  EFI_BLOCK_IO_PROTOCOL *BlkIo;
  UINT8 *Buf;
  UINT64 Bytes;

  Filter.RootDeviceType = NULL;
  Filter.PartitionLabel = (CHAR16 *)Name;
  Filter.VolumeName = NULL;
  Status = GetBlkIOHandles (BLK_IO_SEL_PARTITIONED_MBR | BLK_IO_SEL_PARTITIONED_GPT |
                            BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE | BLK_IO_SEL_MATCH_PARTITION_LABEL,
                            &Filter, Handles, &Count);
  if (EFI_ERROR (Status) || Count != 1) {
    return NULL;
  }
  BlkIo = Handles[0].BlkIo;
  Bytes = MultU64x32 (BlkIo->Media->LastBlock + 1, BlkIo->Media->BlockSize);
  if (Bytes > Max) {
    Bytes = Max;
  }
  Bytes -= Bytes % BlkIo->Media->BlockSize;
  if (Bytes == 0) {
    return NULL;
  }
  Buf = AllocatePool (Bytes);
  if (Buf == NULL) {
    return NULL;
  }
  Status = BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId, 0, Bytes, Buf);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return NULL;
  }
  *Size = Bytes;
  return Buf;
}

/* Out = the value of com.android.build.<Part>.<Key>, if Out is still empty. */
STATIC VOID
CopyProp (IN CONST UINT8 *Vb, IN UINTN Size, IN CONST CHAR8 *Part,
          IN CONST CHAR8 *Key, OUT CHAR8 *Out, IN UINTN OutSize)
{
  CHAR8 Name[64];
  CONST CHAR8 *Value;
  size_t Len = 0;

  if (Out[0] != '\0') {
    return;
  }
  AsciiSPrint (Name, sizeof (Name), "com.android.build.%a.%a", Part, Key);
  Value = avb_property_lookup (Vb, Size, Name, 0, &Len);
  if (Value != NULL && Len > 0 && Len < OutSize) {
    CopyMem (Out, Value, Len);
    Out[Len] = '\0';
  }
}

/* os_version "A[.B[.C]]" and patch "YYYY-MM" from a boot image header. */
STATIC BOOLEAN
FromBootHeader (IN CONST CHAR16 *Name)
{
  UINTN Size = 0;
  UINT8 *Hdr = ReadPartition (Name, BOOT_HEADER_BYTES, &Size);
  UINT32 HeaderVersion;
  UINT32 Raw;
  UINT32 Ver;
  UINT32 A, B, C;
  UINT32 Level;

  if (Hdr == NULL) {
    return FALSE;
  }
  if (Size < 48 || CompareMem (Hdr, "ANDROID!", 8) != 0) {
    FreePool (Hdr);
    return FALSE;
  }
  CopyMem (&HeaderVersion, Hdr + 40, sizeof (HeaderVersion));
  CopyMem (&Raw, Hdr + (HeaderVersion >= 3 ? 16 : 44), sizeof (Raw));
  FreePool (Hdr);
  if (Raw == 0) {
    return FALSE;
  }
  Ver = Raw >> 11;
  A = (Ver >> 14) & 0x7f;
  B = (Ver >> 7) & 0x7f;
  C = Ver & 0x7f;
  Level = Raw & 0x7ff;
  if (C != 0) {
    AsciiSPrint (OsVersion, sizeof (OsVersion), "%u.%u.%u", A, B, C);
  } else if (B != 0) {
    AsciiSPrint (OsVersion, sizeof (OsVersion), "%u.%u", A, B);
  } else {
    AsciiSPrint (OsVersion, sizeof (OsVersion), "%u", A);
  }
  AsciiSPrint (SecurityPatch, sizeof (SecurityPatch), "%u-%02u", 2000 + (Level >> 4), Level & 0xf);
  return TRUE;
}

VOID
ReadAndroidBuildVars (IN CONST CHAR16 *Suffix, OUT ANDROID_BUILD_VARS *Vars)
{
  CHAR16 Name[32];
  UINTN Size = 0;
  UINT8 *Vb;
  UINTN Index;

  OsVersion[0] = SecurityPatch[0] = VbmetaSha256[0] = '\0';
  Vars->OsVersion = OsVersion;
  Vars->SecurityPatch = SecurityPatch;
  Vars->VbmetaSha256 = VbmetaSha256;

  UnicodeSPrint (Name, sizeof (Name), L"vbmeta%s", Suffix);
  Vb = ReadPartition (Name, VBMETA_MAX_BYTES, &Size);
  if (Vb != NULL) {
    AvbSHA256Ctx Ctx;
    UINT8 *Digest;

    avb_sha256_init (&Ctx);
    avb_sha256_update (&Ctx, Vb, Size);
    Digest = avb_sha256_final (&Ctx);
    for (Index = 0; Index < AVB_SHA256_DIGEST_SIZE; Index++) {
      AsciiSPrint (VbmetaSha256 + 2 * Index, 3, "%02x", Digest[Index]);
    }
    CopyProp (Vb, Size, "boot", "os_version", OsVersion, sizeof (OsVersion));
    CopyProp (Vb, Size, "init_boot", "os_version", OsVersion, sizeof (OsVersion));
    CopyProp (Vb, Size, "boot", "security_patch", SecurityPatch, sizeof (SecurityPatch));
    CopyProp (Vb, Size, "init_boot", "security_patch", SecurityPatch, sizeof (SecurityPatch));
  }
  if (OsVersion[0] == '\0' || SecurityPatch[0] == '\0') {
    OsVersion[0] = SecurityPatch[0] = '\0';
    UnicodeSPrint (Name, sizeof (Name), L"boot%s", Suffix);
    if (!FromBootHeader (Name)) {
      UnicodeSPrint (Name, sizeof (Name), L"init_boot%s", Suffix);
      FromBootHeader (Name);
    }
  }
  if (Vb != NULL) {
    CopyProp (Vb, Size, "vendor", "os_version", OsVersion, sizeof (OsVersion));
    CopyProp (Vb, Size, "vendor", "security_patch", SecurityPatch, sizeof (SecurityPatch));
    FreePool (Vb);
  }
  DEBUG ((EFI_D_INFO, "Android build: %a %a vbmeta %a\n", OsVersion, SecurityPatch, VbmetaSha256));
}
