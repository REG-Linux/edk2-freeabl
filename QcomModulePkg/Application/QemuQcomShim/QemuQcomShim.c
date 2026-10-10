/** @file QemuQcomShim.c

  TEST ONLY - never part of an ABL image.

  Minimal stand-ins for the Qualcomm firmware services LinuxLoader needs,
  so the real LinuxLoader.efi can run under QEMU + AAVMF:
    - chip info (SDM845), platform info, RAM partitions, PMIC version,
      memory-card info (UFS), reset reason (normal boot);
    - TrustZone (SCM) calls: every call succeeds with a zeroed response;
    - EFI_HASH2_PROTOCOL, SHA-256 only (AVB hashes through it);
    - QSEECOM (Keymaster TA): apps start, every command answers zeroes (OK);
    - the display panel cmdline variable (the Odin's panel);
    - ExitBootServices: instead of handing over to the kernel, the final DTB
      (it carries /chosen/bootargs) is written to \final.dtb on the shim's
      volume and the VM powers off;
    - "UFS LUN 0": any virtual disk whose GPT has a "devinfo" partition is
      re-exposed under a VenHw(gEfiUfsLU0Guid) root, and each of its GPT
      partitions gets the Qualcomm partition-record / partition-type
      protocols CLO uses to find partitions by name.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/EFIChipInfo.h>
#include <Protocol/EFIPlatformInfo.h>
#include <Protocol/EFIRamPartition.h>
#include <Protocol/EFIPmicVersion.h>
#include <Protocol/EFICardInfo.h>
#include <Protocol/EFIResetReason.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/EFIVerifiedBoot.h>
#include <Library/DeviceInfo.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EFIScm.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/Hash2.h>
#include <Protocol/EFIQseecom.h>

#define SHIM_CHIP_ID   (mProfilePocketS ? 601 : mProfileAirX ? 417 : mProfilePocketMax ? 356 : 321)   /* SM8550 KalamaP SG : SM6115 : SM8250 : SDM845 */

/* Device profile, from the UEFI variable QemuShimProfile (gBootModeVariableGuid),
   set by the test's startup.nsh: "pocketmax" = Mangmi Pocket Max (SM8250),
   "airx66" / "airx65" = Mangmi Air X (SM6115), else AYN Odin. */
STATIC BOOLEAN  mProfilePocketMax;
/* "airx66" / "airx65": Mangmi Air X (SM6115, eMMC), board-id <0x22 subtype>. */
STATIC BOOLEAN  mProfileAirX;
/* "pockets": AYANEO Pocket S (SM8550 KalamaP SG, UFS), board-id <0x1000b 1> (QRD). */
STATIC BOOLEAN  mProfilePocketS;
STATIC UINT32   mAirXSubtype;
#define SHIM_DDR_BASE  0x40000000ULL
#define SHIM_DDR_SIZE  (2ULL * 1024 * 1024 * 1024)

STATIC EFI_STATUS EFIAPI StubUnsupported (VOID) { return EFI_UNSUPPORTED; }

/* Fill every 8-byte slot (function pointers) of a protocol struct with the
   generic stub, so unexpected calls fail cleanly instead of jumping to 0. */
STATIC VOID
FillStubs (VOID *Proto, UINTN Size, UINTN FirstSlot)
{
  UINT64 *Slots = Proto;
  UINTN  i;
  for (i = FirstSlot; i < Size / sizeof (UINT64); i++) {
    Slots[i] = (UINT64)(UINTN)StubUnsupported;
  }
}

/* ---------------------------------------------------------------- chip */
STATIC EFI_STATUS EFIAPI ShimGetChipId (EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoIdType *Id)
{ *Id = (EFIChipInfoIdType)SHIM_CHIP_ID; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimGetRawChipId (EFI_CHIPINFO_PROTOCOL *This, UINT32 *Id)
{ *Id = SHIM_CHIP_ID; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimGetChipVersion (EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoVersionType *V)
{ *V = mProfileAirX ? EFI_CHIPINFO_VERSION (1, 0) : mProfilePocketS ? EFI_CHIPINFO_VERSION (2, 0) : EFI_CHIPINFO_VERSION (2, 1); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimGetRawChipVersion (EFI_CHIPINFO_PROTOCOL *This, UINT32 *V)
{ *V = mProfileAirX ? EFI_CHIPINFO_VERSION (1, 0) : mProfilePocketS ? EFI_CHIPINFO_VERSION (2, 0) : EFI_CHIPINFO_VERSION (2, 1); return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimGetFoundryId (EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoFoundryIdType *F)
{ *F = (EFIChipInfoFoundryIdType)0; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimGetModemSupport (EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoModemType *M)
{ *M = 0; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimGetSerialNumber (EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoSerialNumType *S)
{ *S = 0x51454d55; return EFI_SUCCESS; }  /* "QEMU" */
STATIC EFI_STATUS EFIAPI ShimGetChipIdString (EFI_CHIPINFO_PROTOCOL *This, CHAR8 *Str, UINT32 Max)
{ AsciiStrCpyS (Str, Max, "SDM845"); return EFI_SUCCESS; }

/* ------------------------------------------------------------ platform */
STATIC EFI_STATUS EFIAPI ShimGetPlatformInfo (EFI_PLATFORMINFO_PROTOCOL *This,
                                              EFI_PLATFORMINFO_PLATFORM_INFO_TYPE *Info)
{
  ZeroMem (Info, sizeof (*Info));
  /* AYN Odin: qcom,board-id = <5 0> */
  /* Mangmi Pocket Max (ThunderComm): board-id <0x10008 0> = MTP (0x08),
     major version 1 (bits 16-23), subtype 0 (bits 24-31) */
  Info->platform = mProfilePocketS ? EFI_PLATFORMINFO_TYPE_QRD :
                   mProfileAirX ? EFI_PLATFORMINFO_TYPE_IDP :
                   mProfilePocketMax ? EFI_PLATFORMINFO_TYPE_MTP
                                                       : EFI_PLATFORMINFO_TYPE_OEM;
  Info->subtype  = mProfileAirX ? mAirXSubtype : mProfilePocketS ? 1 : 0;
  Info->version  = 0x10000;
  return EFI_SUCCESS;
}

/* ----------------------------------------------------------------- RAM */
STATIC EFI_STATUS EFIAPI ShimGetRamPartitions (EFI_RAMPARTITION_PROTOCOL *This,
                                               RamPartitionEntry *Parts, UINT32 *Num)
{
  if (Parts == NULL || *Num < 1) {
    *Num = 1;
    return EFI_BUFFER_TOO_SMALL;
  }
  Parts[0].Base = SHIM_DDR_BASE;
  Parts[0].AvailableLength = SHIM_DDR_SIZE;
  *Num = 1;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimGetMinPasrSize (EFI_RAMPARTITION_PROTOCOL *This, UINT32 *Size)
{ *Size = 0x200000; return EFI_SUCCESS; }

/* ---------------------------------------------------------------- PMIC */
STATIC EFI_STATUS EFIAPI ShimGetPmicInfo (UINT32 Index, EFI_PM_DEVICE_INFO_TYPE *Info)
{
  ZeroMem (Info, sizeof (*Info));
  if (Index != 0) {
    return EFI_UNSUPPORTED;
  }
  Info->PmicModel = (EFI_PM_MODEL_TYPE)30;   /* PM8998 */
  Info->PmicAllLayerRevision = 2;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimGetPmicInfoExt (UINT32 Index, EFI_PM_DEVICE_INFO_EXT_TYPE *Info)
{
  ZeroMem (Info, sizeof (*Info));
  if (Index != 0) {
    return EFI_UNSUPPORTED;
  }
  Info->PmicModel = (EFI_PM_MODEL_TYPE)30;
  Info->PmicAllLayerRevision = 2;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------ card info */
STATIC EFI_STATUS EFIAPI ShimGetCardInfo (EFI_MEM_CARDINFO_PROTOCOL *This, MEM_CARD_INFO *Info)
{
  ZeroMem (Info, sizeof (*Info));
  Info->mfr_id = 0x1ad;
  if (mProfileAirX) {
    CopyMem (Info->card_type, "EMMC", 4);
  } else {
    CopyMem (Info->card_type, "UFS", 3);
  }
  CopyMem (Info->product_serial_num, "QEMU0001", 8);
  Info->serial_num_len = 8;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimGetBootLU (EFI_MEM_CARDINFO_PROTOCOL *This, UINT32 *Lu)
{ *Lu = 1; return EFI_SUCCESS; }

/* --------------------------------------------------------- reset reason */
STATIC EFI_STATUS EFIAPI ShimGetResetReason (EFI_RESETREASON_PROTOCOL *This, UINT32 *Reason,
                                             CHAR16 *Str, UINT32 *StrLen)
{ *Reason = 0; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimClearResetReason (EFI_RESETREASON_PROTOCOL *This)
{ return EFI_SUCCESS; }


/* ------------------------------------------------- verified boot (TZ) */
/* Device info normally lives in TrustZone-backed storage; keep it in RAM,
   pre-filled as an unlocked device (like the AYN Odin under test). */
STATIC DeviceInfo mDevInfo;

STATIC EFI_STATUS EFIAPI ShimVBRwDeviceState (QCOM_VERIFIEDBOOT_PROTOCOL *This,
                                             vb_device_state_op_t Op, UINT8 *Buf, UINT32 Len)
{
  UINT32 n = MIN (Len, (UINT32)sizeof (mDevInfo));
  if (Op == READ_CONFIG) {
    ZeroMem (Buf, Len);
    CopyMem (Buf, &mDevInfo, n);
  } else {
    CopyMem (&mDevInfo, Buf, n);
    Print (L"QEMUSHIM: devinfo written (unlocked=%d)\n", mDevInfo.is_unlocked);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimVBOk (VOID) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimVBIsDeviceSecure (QCOM_VERIFIEDBOOT_PROTOCOL *This, BOOLEAN *State)
{ *State = FALSE; return EFI_SUCCESS; }

/* ---------------------------------------------------------- TrustZone */
STATIC EFI_STATUS EFIAPI ShimScmSipSysCall (QCOM_SCM_PROTOCOL *This, UINT32 SmcId, UINT32 ParamId,
                                            UINT64 Parameters[SCM_MAX_NUM_PARAMETERS],
                                            UINT64 Results[SCM_MAX_NUM_RESULTS])
{
  ZeroMem (Results, sizeof (UINT64) * SCM_MAX_NUM_RESULTS);
  Results[0] = 1;   /* common_rsp.status: success */
  Print (L"QEMUSHIM: SCM SIP call 0x%x\n", SmcId);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimScmSysCall (QCOM_SCM_PROTOCOL *This, CONST VOID *Cmd)
{ return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimScmFastCall2 (QCOM_SCM_PROTOCOL *This, UINT32 Id, UINT32 P0, UINT32 P1)
{ return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimScmGetVersion (QCOM_SCM_PROTOCOL *This, UINT32 *Version)
{ *Version = 0x00040000; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimScmSendCommand (QCOM_SCM_PROTOCOL *This, AppCmdType CmdId, UINT32 *AppId,
                                             VOID *Req, UINTN ReqLen, VOID *Rsp, UINTN RspLen)
{
  if (AppId != NULL && *AppId == 0) {
    *AppId = 1;
  }
  if (Rsp != NULL) {
    ZeroMem (Rsp, RspLen);
  }
  Print (L"QEMUSHIM: SCM command %u (req %u, rsp %u)\n", (UINT32)CmdId, (UINT32)ReqLen, (UINT32)RspLen);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimScmEbs (QCOM_SCM_PROTOCOL *This)
{ return EFI_SUCCESS; }

/* ------------------------------------------------------------- QSEECOM */
STATIC EFI_STATUS EFIAPI ShimQseeStartApp (QCOM_QSEECOM_PROTOCOL *This, CHAR8 *Name, UINT32 *Handle)
{
  *Handle = 0x10;
  Print (L"QEMUSHIM: QSEECOM start app %a\n", Name);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ShimQseeShutdownApp (QCOM_QSEECOM_PROTOCOL *This, UINT32 Handle)
{ return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI ShimQseeSendCmd (QCOM_QSEECOM_PROTOCOL *This, UINT32 Handle, UINT8 *Req,
                                          UINT32 ReqLen, UINT8 *Rsp, UINT32 RspLen)
{
  if (Rsp != NULL) {
    ZeroMem (Rsp, RspLen);
    /* KEYMASTER_GET_VERSION: { Status, Major, Minor, AppMajor, AppMinor }; LinuxLoader needs Major >= 2. */
    if (ReqLen >= 4 && *(UINT32 *)Req == 0x200 && RspLen >= 8) {
      ((UINT32 *)Rsp)[1] = 4;
    }
  }
  Print (L"QEMUSHIM: QSEECOM cmd 0x%x (req %u, rsp %u)\n",
         (ReqLen >= 4) ? *(UINT32 *)Req : 0, ReqLen, RspLen);
  return EFI_SUCCESS;
}

/* -------------------------------------------------------------- SHA-256 */
STATIC CONST UINT32 mK256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

typedef struct {
  UINT32  H[8];
  UINT8   Buf[64];
  UINTN   BufLen;
  UINT64  Total;
} SHIM_SHA256;

STATIC SHIM_SHA256  mSha;
STATIC BOOLEAN      mShaActive;

#define ROR32(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))

STATIC VOID
Sha256Block (SHIM_SHA256 *C, CONST UINT8 *P)
{
  UINT32  W[64];
  UINT32  a, b, c, d, e, f, g, h, t1, t2;
  UINTN   i;

  for (i = 0; i < 16; i++) {
    W[i] = ((UINT32)P[4 * i] << 24) | ((UINT32)P[4 * i + 1] << 16) |
           ((UINT32)P[4 * i + 2] << 8) | P[4 * i + 3];
  }
  for (i = 16; i < 64; i++) {
    UINT32 s0 = ROR32 (W[i - 15], 7) ^ ROR32 (W[i - 15], 18) ^ (W[i - 15] >> 3);
    UINT32 s1 = ROR32 (W[i - 2], 17) ^ ROR32 (W[i - 2], 19) ^ (W[i - 2] >> 10);
    W[i] = W[i - 16] + s0 + W[i - 7] + s1;
  }
  a = C->H[0]; b = C->H[1]; c = C->H[2]; d = C->H[3];
  e = C->H[4]; f = C->H[5]; g = C->H[6]; h = C->H[7];
  for (i = 0; i < 64; i++) {
    t1 = h + (ROR32 (e, 6) ^ ROR32 (e, 11) ^ ROR32 (e, 25)) + ((e & f) ^ (~e & g)) + mK256[i] + W[i];
    t2 = (ROR32 (a, 2) ^ ROR32 (a, 13) ^ ROR32 (a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  C->H[0] += a; C->H[1] += b; C->H[2] += c; C->H[3] += d;
  C->H[4] += e; C->H[5] += f; C->H[6] += g; C->H[7] += h;
}

STATIC VOID
Sha256Init (SHIM_SHA256 *C)
{
  STATIC CONST UINT32 Iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  CopyMem (C->H, Iv, sizeof (Iv));
  C->BufLen = 0;
  C->Total  = 0;
}

STATIC VOID
Sha256Update (SHIM_SHA256 *C, CONST UINT8 *P, UINTN Len)
{
  C->Total += Len;
  while (Len > 0) {
    UINTN n = MIN (Len, 64 - C->BufLen);
    CopyMem (C->Buf + C->BufLen, P, n);
    C->BufLen += n; P += n; Len -= n;
    if (C->BufLen == 64) {
      Sha256Block (C, C->Buf);
      C->BufLen = 0;
    }
  }
}

STATIC VOID
Sha256Final (SHIM_SHA256 *C, UINT8 *Out)
{
  UINT64  Bits = C->Total * 8;
  UINT8   Pad  = 0x80;
  UINT8   Zero = 0;
  UINT8   Len[8];
  UINTN   i;

  Sha256Update (C, &Pad, 1);
  while (C->BufLen != 56) {
    Sha256Update (C, &Zero, 1);
  }
  for (i = 0; i < 8; i++) {
    Len[i] = (UINT8)(Bits >> (56 - 8 * i));
  }
  Sha256Update (C, Len, 8);
  for (i = 0; i < 8; i++) {
    Out[4 * i]     = (UINT8)(C->H[i] >> 24);
    Out[4 * i + 1] = (UINT8)(C->H[i] >> 16);
    Out[4 * i + 2] = (UINT8)(C->H[i] >> 8);
    Out[4 * i + 3] = (UINT8)C->H[i];
  }
}

STATIC EFI_STATUS EFIAPI
ShimHashSize (CONST EFI_HASH2_PROTOCOL *This, CONST EFI_GUID *Alg, UINTN *Size)
{
  if (!CompareGuid (Alg, &gEfiHashAlgorithmSha256Guid)) {
    return EFI_UNSUPPORTED;
  }
  *Size = 32;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
ShimHash (CONST EFI_HASH2_PROTOCOL *This, CONST EFI_GUID *Alg, CONST UINT8 *Msg, UINTN Len,
          EFI_HASH2_OUTPUT *Out)
{
  SHIM_SHA256 C;
  if (!CompareGuid (Alg, &gEfiHashAlgorithmSha256Guid)) {
    return EFI_UNSUPPORTED;
  }
  Sha256Init (&C);
  Sha256Update (&C, Msg, Len);
  Sha256Final (&C, Out->Sha256Hash);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
ShimHashInit (CONST EFI_HASH2_PROTOCOL *This, CONST EFI_GUID *Alg)
{
  if (!CompareGuid (Alg, &gEfiHashAlgorithmSha256Guid)) {
    return EFI_UNSUPPORTED;
  }
  Sha256Init (&mSha);
  mShaActive = TRUE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
ShimHashUpdate (CONST EFI_HASH2_PROTOCOL *This, CONST UINT8 *Msg, UINTN Len)
{
  if (!mShaActive) {
    return EFI_NOT_READY;
  }
  Sha256Update (&mSha, Msg, Len);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI
ShimHashFinal (CONST EFI_HASH2_PROTOCOL *This, EFI_HASH2_OUTPUT *Out)
{
  if (!mShaActive) {
    return EFI_NOT_READY;
  }
  Sha256Final (&mSha, Out->Sha256Hash);
  mShaActive = FALSE;
  return EFI_SUCCESS;
}

/* ------------------------------------------------ kernel handoff capture */
STATIC EFI_HANDLE  mSelfDevice;

STATIC BOOLEAN
BlobHas (CONST UINT8 *B, UINTN Len, CONST CHAR8 *Str)
{
  UINTN  n = AsciiStrLen (Str);
  UINTN  i;
  for (i = 0; i + n <= Len; i++) {
    if (B[i] == (UINT8)Str[0] && CompareMem (B + i, Str, n) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

/* The DTB LinuxLoader hands to the kernel: the FDT whose /chosen got the initrd
   properties (only the final one has them). */
STATIC UINT8 *
FindFinalDtb (UINT32 *Size)
{
  UINTN                  MapSize = 0;
  UINTN                  Key;
  UINTN                  DescSize;
  UINT32                 DescVer;
  EFI_MEMORY_DESCRIPTOR  *Map = NULL;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINT8                  *Found = NULL;
  UINTN                  i;

  gBS->GetMemoryMap (&MapSize, NULL, &Key, &DescSize, &DescVer);
  MapSize += 16 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL || EFI_ERROR (gBS->GetMemoryMap (&MapSize, Map, &Key, &DescSize, &DescVer))) {
    return NULL;
  }
  /* Pass 0: memory LinuxLoader allocated (where its DTB is); pass 1: the rest.
     Scanning boot-services / free memory first can hit pages AAVMF protects. */
  for (i = 0; i < 2 * (MapSize / DescSize) && Found == NULL; i++) {
    UINT64   p;
    UINT64   End;
    BOOLEAN  Pass0 = (i < MapSize / DescSize);
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + (i % (MapSize / DescSize)) * DescSize);
    if (Pass0 ? (D->Type != EfiLoaderCode && D->Type != EfiLoaderData)
              : (D->Type != EfiConventionalMemory)) {
      continue;
    }
    End = D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages);
    for (p = D->PhysicalStart; p + 40 <= End; p += 8) {
      UINT32  *W = (UINT32 *)(UINTN)p;
      UINT32  Sz;
      if (W[0] != 0xedfe0dd0) {          /* d0 0d fe ed, big endian */
        continue;
      }
      Sz = SwapBytes32 (W[1]);
      if (Sz < 0x100 || Sz > SIZE_8MB || p + Sz > End ||
          !BlobHas ((UINT8 *)W, Sz, "linux,initrd-start")) {
        continue;
      }
      Found = (UINT8 *)W;
      *Size = Sz;
      Print (L"QEMUSHIM: final DTB at 0x%lx, %u bytes\n", p, Sz);
      break;
    }
  }
  FreePool (Map);
  return Found;
}

/* Header v4 (GKI): androidboot.* go to the bootconfig block appended to the
   ramdisk: <text><u32 size><u32 checksum>"#BOOTCONFIG\n". */
STATIC CHAR8 *
FindBootconfig (UINT32 *Size)
{
  UINTN                  MapSize = 0;
  UINTN                  Key;
  UINTN                  DescSize;
  UINT32                 DescVer;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *D;
  CHAR8                  *Found = NULL;
  UINTN                  i;

  gBS->GetMemoryMap (&MapSize, NULL, &Key, &DescSize, &DescVer);
  MapSize += 16 * DescSize;
  Map = AllocatePool (MapSize);
  if (Map == NULL || EFI_ERROR (gBS->GetMemoryMap (&MapSize, Map, &Key, &DescSize, &DescVer))) {
    return NULL;
  }
  for (i = 0; i < MapSize / DescSize && Found == NULL; i++) {
    UINT64 p;
    UINT64 End;
    D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + i * DescSize);
    if (D->Type != EfiLoaderData && D->Type != EfiConventionalMemory) {
      continue;
    }
    End = D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages);
    for (p = D->PhysicalStart + 8; p + 12 <= End; p += 4) {
      if (*(UINT32 *)(UINTN)p == 0x4f4f4223 &&      /* "#BOO" */
          CompareMem ((VOID *)(UINTN)p, "#BOOTCONFIG\n", 12) == 0) {
        UINT32 Sz = *(UINT32 *)(UINTN)(p - 8);
        if (Sz > 0 && Sz < SIZE_64KB && p - 8 - Sz >= D->PhysicalStart) {
          Found = (CHAR8 *)(UINTN)(p - 8 - Sz);
          *Size = Sz;
          Print (L"QEMUSHIM: bootconfig at 0x%lx, %u bytes\n", p - 8 - Sz, Sz);
          break;
        }
      }
    }
  }
  FreePool (Map);
  return Found;
}

STATIC EFI_STATUS EFIAPI
ShimExitBootServices (EFI_HANDLE ImageHandle, UINTN MapKey)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *F;
  UINT8                            *Dtb;
  UINT32                           Sz = 0;
  UINTN                            Len;
  EFI_STATUS                       Status = EFI_NOT_FOUND;

  Print (L"QEMUSHIM: ExitBootServices: kernel handoff, capturing the DTB\n");
  Dtb = FindFinalDtb (&Sz);
  if (Dtb != NULL &&
      !EFI_ERROR (gBS->HandleProtocol (mSelfDevice, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)) &&
      !EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
    Status = Root->Open (Root, &F, L"\\final.dtb",
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (!EFI_ERROR (Status)) {
      Len    = Sz;
      Status = F->Write (F, &Len, Dtb);
      F->Close (F);
    }
    {
      UINT32  BcSz = 0;
      /* Only boot header v4 builds pass "bootconfig" on the cmdline. */
      CHAR8   *Bc  = BlobHas (Dtb, Sz, "bootconfig") ? FindBootconfig (&BcSz) : NULL;
      if (Bc != NULL &&
          !EFI_ERROR (Root->Open (Root, &F, L"\\bootconfig.txt",
                                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
        Len = BcSz;
        F->Write (F, &Len, Bc);
        F->Close (F);
      }
    }
    Root->Close (Root);
  }
  Print (L"LLTEST-HANDOFF dtb=%r size=%u\n", Status, Sz);
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  return EFI_DEVICE_ERROR;
}

/* ------------------------------------------------------- UFS LUN 0 wrap */
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} SHIM_UFS_DEVICE_PATH;
#pragma pack()


/* Own BlockIo for the UFS root, forwarding to the virtual disk's current
   BlockIo.  ABL's SD init disconnects/reconnects every block device; the
   disk driver then re-creates its BlockIo, so look it up on every call. */
STATIC EFI_MEM_CARDINFO_PROTOCOL Card;   /* also on the UFS root handle */
STATIC EFI_HANDLE             mUfsDiskHandle;
STATIC EFI_BLOCK_IO_MEDIA     mUfsMedia;
STATIC EFI_BLOCK_IO_PROTOCOL  mUfsBlkIo;
STATIC EFI_DEVICE_PATH_PROTOCOL *mUfsRoot;

STATIC EFI_BLOCK_IO_PROTOCOL *
UfsUnder (VOID)
{
  EFI_BLOCK_IO_PROTOCOL *B = NULL;
  if (EFI_ERROR (gBS->HandleProtocol (mUfsDiskHandle, &gEfiBlockIoProtocolGuid, (VOID **)&B))) {
    gBS->ConnectController (mUfsDiskHandle, NULL, NULL, FALSE);
    if (EFI_ERROR (gBS->HandleProtocol (mUfsDiskHandle, &gEfiBlockIoProtocolGuid, (VOID **)&B))) {
      return NULL;
    }
  }
  return B;
}
STATIC EFI_STATUS EFIAPI UfsReset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Ext)
{ EFI_BLOCK_IO_PROTOCOL *B = UfsUnder (); return B ? B->Reset (B, Ext) : EFI_NO_MEDIA; }
STATIC EFI_STATUS EFIAPI UfsRead (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id, EFI_LBA Lba, UINTN Sz, VOID *Buf)
{ EFI_BLOCK_IO_PROTOCOL *B = UfsUnder (); return B ? B->ReadBlocks (B, B->Media->MediaId, Lba, Sz, Buf) : EFI_NO_MEDIA; }
STATIC EFI_STATUS EFIAPI UfsWrite (EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id, EFI_LBA Lba, UINTN Sz, VOID *Buf)
{ EFI_BLOCK_IO_PROTOCOL *B = UfsUnder (); return B ? B->WriteBlocks (B, B->Media->MediaId, Lba, Sz, Buf) : EFI_NO_MEDIA; }
STATIC EFI_STATUS EFIAPI UfsFlush (EFI_BLOCK_IO_PROTOCOL *This)
{ EFI_BLOCK_IO_PROTOCOL *B = UfsUnder (); return B ? B->FlushBlocks (B) : EFI_NO_MEDIA; }

STATIC BOOLEAN
DiskHasPartition (EFI_BLOCK_IO_PROTOCOL *BlkIo, CONST CHAR16 *Name)
{
  EFI_PARTITION_TABLE_HEADER *Hdr;
  EFI_PARTITION_ENTRY        *Entries;
  UINT32                     Bs = BlkIo->Media->BlockSize;
  UINTN                      Len;
  UINT32                     i;
  BOOLEAN                    Found = FALSE;

  Hdr = AllocatePool (Bs);
  if (Hdr == NULL) {
    return FALSE;
  }
  if (EFI_ERROR (BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId, 1, Bs, Hdr)) ||
      Hdr->Header.Signature != EFI_PTAB_HEADER_ID ||
      Hdr->SizeOfPartitionEntry != sizeof (EFI_PARTITION_ENTRY)) {
    FreePool (Hdr);
    return FALSE;
  }
  Len = ALIGN_VALUE (Hdr->NumberOfPartitionEntries * sizeof (EFI_PARTITION_ENTRY), Bs);
  Entries = AllocatePool (Len);
  if (Entries != NULL &&
      !EFI_ERROR (BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId,
                                     Hdr->PartitionEntryLBA, Len, Entries))) {
    for (i = 0; i < Hdr->NumberOfPartitionEntries; i++) {
      if (StrCmp (Entries[i].PartitionName, Name) == 0) {
        Found = TRUE;
        break;
      }
    }
  }
  if (Entries != NULL) {
    FreePool (Entries);
  }
  FreePool (Hdr);
  return Found;
}

STATIC VOID
TagUfsPartitions (EFI_DEVICE_PATH_PROTOCOL *Root)
{
  EFI_HANDLE                   *Handles = NULL;
  UINTN                        Count = 0;
  UINTN                        i;
  EFI_DEVICE_PATH_PROTOCOL     *Dp;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  UINTN                        RootLen = GetDevicePathSize (Root) - END_DEVICE_PATH_LENGTH;
  UINTN                        Tagged = 0;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return;
  }
  for (i = 0; i < Count; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&Dp)) ||
        CompareMem (Dp, Root, RootLen) != 0 ||
        EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info)) ||
        Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }
    {
      VOID *Already;
      if (!EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiPartitionRecordGuid, &Already))) {
        continue;
      }
    }
    gBS->InstallMultipleProtocolInterfaces (&Handles[i],
           &gEfiPartitionRecordGuid, &Info->Info.Gpt,
           &gEfiPartitionTypeGuid, &Info->Info.Gpt.PartitionTypeGUID,
           NULL);
    Tagged++;
  }
  FreePool (Handles);
  Print (L"QEMUSHIM: tagged %u UFS partitions\n", Tagged);
}

STATIC VOID EFIAPI
OnNewPartition (EFI_EVENT Event, VOID *Context)
{
  /* Qualcomm's partition driver tags partitions on every (re)connect;
     do the same for partitions re-created after ABL's SD init. */
  TagUfsPartitions (mUfsRoot);
}

STATIC VOID
ExposeUfsLun0 (VOID)
{
  EFI_HANDLE                *Handles = NULL;
  UINTN                     Count = 0;
  UINTN                     i;
  EFI_BLOCK_IO_PROTOCOL     *BlkIo;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  SHIM_UFS_DEVICE_PATH      *UfsDp;
  EFI_HANDLE                UfsHandle = NULL;

  /* AAVMF only connects boot devices: bring every controller up first. */
  if (!EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) {
    for (i = 0; i < Count; i++) {
      gBS->ConnectController (Handles[i], NULL, NULL, TRUE);
    }
    FreePool (Handles);
    Handles = NULL;
  }
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                          NULL, &Count, &Handles))) {
    return;
  }
  for (i = 0; i < Count; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlkIo)) ||
        BlkIo->Media->LogicalPartition ||
        EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&Dp))) {
      continue;
    }
    Print (L"QEMUSHIM: disk %u: %u blocks of %u, removable=%d\n", i,
           (UINT32)BlkIo->Media->LastBlock + 1, BlkIo->Media->BlockSize,
           BlkIo->Media->RemovableMedia);
    if (!DiskHasPartition (BlkIo, L"devinfo")) {
      continue;
    }
    UfsDp = AllocateZeroPool (sizeof (*UfsDp));
    UfsDp->Vendor.Header.Type = HARDWARE_DEVICE_PATH;
    UfsDp->Vendor.Header.SubType = HW_VENDOR_DP;
    SetDevicePathNodeLength (&UfsDp->Vendor.Header, sizeof (VENDOR_DEVICE_PATH));
    /* Air X boots from eMMC: CLO looks for the eMMC user partition root. */
    CopyGuid (&UfsDp->Vendor.Guid, mProfileAirX ? &gEfiEmmcUserPartitionGuid : &gEfiUfsLU0Guid);
    SetDevicePathEndNode (&UfsDp->End);
    mUfsDiskHandle = Handles[i];
    CopyMem (&mUfsMedia, BlkIo->Media, sizeof (mUfsMedia));
    mUfsBlkIo.Revision    = BlkIo->Revision;
    mUfsBlkIo.Media       = &mUfsMedia;
    mUfsBlkIo.Reset       = UfsReset;
    mUfsBlkIo.ReadBlocks  = UfsRead;
    mUfsBlkIo.WriteBlocks = UfsWrite;
    mUfsBlkIo.FlushBlocks = UfsFlush;
    mUfsRoot = (EFI_DEVICE_PATH_PROTOCOL *)UfsDp;
    gBS->InstallMultipleProtocolInterfaces (&UfsHandle,
           &gEfiDevicePathProtocolGuid, UfsDp,
           &gEfiBlockIoProtocolGuid, &mUfsBlkIo,
           &gEfiMemCardInfoProtocolGuid, &Card,
           NULL);
    gBS->ConnectController (UfsHandle, NULL, NULL, TRUE);
    Print (L"QEMUSHIM: UFS LU0 = virtual disk %u\n", i);
    TagUfsPartitions ((EFI_DEVICE_PATH_PROTOCOL *)UfsDp);
    {
      EFI_EVENT Ev;
      VOID      *Reg;
      if (!EFI_ERROR (gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnNewPartition, NULL, &Ev))) {
        gBS->RegisterProtocolNotify (&gEfiPartitionInfoProtocolGuid, Ev, &Reg);
      }
    }
    break;
  }
  FreePool (Handles);
}

/* --------------------------------------------------------------- entry */
EFI_STATUS
EFIAPI
QemuQcomShimEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC EFI_CHIPINFO_PROTOCOL           Chip;
  STATIC EFI_PLATFORMINFO_PROTOCOL       Plat;
  STATIC EFI_RAMPARTITION_PROTOCOL       Ram;
  STATIC EFI_QCOM_PMIC_VERSION_PROTOCOL  Pmic;
  STATIC EFI_RESETREASON_PROTOCOL        Reset;
  STATIC QCOM_VERIFIEDBOOT_PROTOCOL      Vb;
  STATIC QCOM_SCM_PROTOCOL               Scm;
  STATIC EFI_HASH2_PROTOCOL              Hash2 = {
    ShimHashSize, ShimHash, ShimHashInit, ShimHashUpdate, ShimHashFinal
  };
  EFI_HANDLE                             HashHandle = NULL;
  STATIC QCOM_QSEECOM_PROTOCOL           Qsee;
  EFI_HANDLE                             QseeHandle = NULL;
  STATIC CHAR8                           Panel[] =
    " msm_drm.dsi_display0=dsi_innolux_td4328_1080p_cmd_display:";
  EFI_HANDLE                             ScmHandle = NULL;
  EFI_HANDLE                             Handle = NULL;

  {
    CHAR8  Prof[16];
    UINTN  Sz = sizeof (Prof) - 1;
    ZeroMem (Prof, sizeof (Prof));
    if (!EFI_ERROR (gRT->GetVariable (L"QemuShimProfile", &gBootModeVariableGuid,
                                      NULL, &Sz, Prof))) {
      mProfilePocketMax = (AsciiStrnCmp (Prof, "pocketmax", 9) == 0);
      mProfileAirX      = (AsciiStrnCmp (Prof, "airx", 4) == 0);
      mProfilePocketS   = (AsciiStrnCmp (Prof, "pockets", 7) == 0);
      /* MQ66: subtype = revision stage (PVT 0x02, the owner's stock DT);
         MQ65: feature bits, 0x58 = "Panda WIFI I2C GH7005 IDP" as an example. */
      mAirXSubtype      = (AsciiStrnCmp (Prof, "airx65", 6) == 0) ? 0x58 : 0x02;
    }
    Print (L"QEMUSHIM: profile %a\n", mProfilePocketMax ? "pocketmax (Mangmi, SM8250)" :
                                       mProfileAirX ? "airx (Mangmi Air X, SM6115, eMMC)" :
                                       mProfilePocketS ? "pockets (AYANEO Pocket S, SM8550)" : "odin");
  }

  FillStubs (&Chip, sizeof (Chip), 1);
  Chip.Revision          = EFI_CHIPINFO_PROTOCOL_REVISION_5;
  Chip.GetChipId         = ShimGetChipId;
  Chip.GetRawChipId      = ShimGetRawChipId;
  Chip.GetChipVersion    = ShimGetChipVersion;
  Chip.GetRawChipVersion = ShimGetRawChipVersion;
  Chip.GetFoundryId      = ShimGetFoundryId;
  Chip.GetModemSupport   = ShimGetModemSupport;
  Chip.GetSerialNumber   = ShimGetSerialNumber;
  Chip.GetChipIdString   = ShimGetChipIdString;

  FillStubs (&Plat, sizeof (Plat), 1);
  Plat.Version         = EFI_PLATFORMINFO_PROTOCOL_VERSION;
  Plat.GetPlatformInfo = ShimGetPlatformInfo;

  FillStubs (&Ram, sizeof (Ram), 1);
  Ram.Revision         = EFI_RAMPARTITION_PROTOCOL_REVISION;
  Ram.GetRamPartitions = ShimGetRamPartitions;
  Ram.GetMinPasrSize   = ShimGetMinPasrSize;

  FillStubs (&Pmic, sizeof (Pmic), 1);
  Pmic.Revision       = PMIC_VERSION_REVISION;
  Pmic.GetPmicInfo    = ShimGetPmicInfo;
  Pmic.GetPmicInfoExt = ShimGetPmicInfoExt;

  FillStubs (&Card, sizeof (Card), 1);
  Card.Revision    = EFI_MEM_CARD_INFO_PROTOCOL_REVISION_3;
  Card.GetCardInfo = ShimGetCardInfo;
  Card.GetBootLU   = ShimGetBootLU;

  FillStubs (&Vb, sizeof (Vb), 1);
  Vb.Revision          = QCOM_VERIFIEDBOOT_PROTOCOL_REVISION;
  Vb.VBRwDeviceState   = ShimVBRwDeviceState;
  Vb.VBDeviceInit      = (QCOM_VB_DEVICE_INIT)ShimVBOk;
  Vb.VBSendMilestone   = (QCOM_VB_SEND_MILESTONE)ShimVBOk;
  Vb.VBIsDeviceSecure  = ShimVBIsDeviceSecure;
  CopyMem (mDevInfo.magic, DEVICE_MAGIC, DEVICE_MAGIC_SIZE);
  mDevInfo.is_unlocked = TRUE;
  mDevInfo.is_unlock_critical = TRUE;

  Reset.GetResetReason   = ShimGetResetReason;
  Reset.ClearResetReason = ShimClearResetReason;

  gBS->InstallMultipleProtocolInterfaces (&Handle,
         &gEfiChipInfoProtocolGuid, &Chip,
         &gEfiPlatformInfoProtocolGuid, &Plat,
         &gEfiRamPartitionProtocolGuid, &Ram,
         &gQcomPmicVersionProtocolGuid, &Pmic,
         &gEfiMemCardInfoProtocolGuid, &Card,
         &gEfiResetReasonProtocolGuid, &Reset,
         &gEfiQcomVerifiedBootProtocolGuid, &Vb,
         NULL);
  FillStubs (&Scm, sizeof (Scm), 1);
  Scm.Revision                   = 1;
  Scm.ScmSysCall                 = ShimScmSysCall;
  Scm.ScmFastCall2               = ShimScmFastCall2;
  Scm.ScmGetVersion              = ShimScmGetVersion;
  Scm.ScmSendCommand             = ShimScmSendCommand;
  Scm.ScmExitBootServicesHandler = (QCOM_SCM_EXIT_BOOT_SERVICES)ShimScmEbs;
  Scm.ScmSipSysCall              = ShimScmSipSysCall;
  gBS->InstallMultipleProtocolInterfaces (&ScmHandle, &gQcomScmProtocolGuid, &Scm, NULL);
  gBS->InstallMultipleProtocolInterfaces (&HashHandle, &gEfiHash2ProtocolGuid, &Hash2, NULL);
  FillStubs (&Qsee, sizeof (Qsee), 1);
  Qsee.Revision           = 1;
  Qsee.QseecomStartApp    = ShimQseeStartApp;
  Qsee.QseecomShutdownApp = ShimQseeShutdownApp;
  Qsee.QseecomSendCmd     = ShimQseeSendCmd;
  gBS->InstallMultipleProtocolInterfaces (&QseeHandle, &gQcomQseecomProtocolGuid, &Qsee, NULL);

  /* Set by the firmware on the device: UFS host controller base (the cmdline's
     androidboot.bootdevice=1d84000.ufshc). */
  {
    UINT64 BootDev = 0x1d84000;
    gRT->SetVariable (L"BootDeviceBaseAddr", &gQcomTokenSpaceGuid,
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                      sizeof (BootDev), &BootDev);
  }
  /* Set by the display driver on the device (the Odin's panel). */
  if (!mProfilePocketMax && !mProfileAirX && !mProfilePocketS) {
    gRT->SetVariable (L"DisplayPanelConfiguration", &gQcomTokenSpaceGuid,
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                      sizeof (Panel), Panel);
  }

  Print (L"QEMUSHIM: Qualcomm stand-in services installed (chip %u)\n", SHIM_CHIP_ID);

  ExposeUfsLun0 ();

  /* Launch LinuxLoader.efi from our own volume; we stay loaded meanwhile,
     so the stand-in services above remain valid. */
  {
    EFI_LOADED_IMAGE_PROTOCOL  *Self;
    EFI_DEVICE_PATH_PROTOCOL   *LlPath;
    EFI_HANDLE                 Ll = NULL;
    EFI_STATUS                 Status;

    gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
    mSelfDevice = Self->DeviceHandle;
    gBS->ExitBootServices = ShimExitBootServices;
    gBS->Hdr.CRC32 = 0;
    gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
    LlPath = FileDevicePath (Self->DeviceHandle, L"\\LinuxLoader.efi");
    Status = gBS->LoadImage (FALSE, ImageHandle, LlPath, NULL, 0, &Ll);
    Print (L"QEMUSHIM: LoadImage LinuxLoader.efi: %r\n", Status);
    if (!EFI_ERROR (Status)) {
      Status = gBS->StartImage (Ll, NULL, NULL);
      Print (L"QEMUSHIM: LinuxLoader returned %r\n", Status);
    }
    return Status;
  }
}
