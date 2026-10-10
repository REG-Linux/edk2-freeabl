/** @file DistroPartition.c
    Data-driven detection of an installed retro-gaming CFW distro by its GPT
    partition layout.

    Several retro-gaming CFW distros follow the same on-disk convention for an
    internal (eMMC/UFS) install: a dedicated FAT system partition, a dedicated
    data partition, and reuse of Android's "userdata" partition (which an
    uninstall step could later expand to reclaim the freed space once the
    system/data partitions are removed).

    This file matches that convention against a small, extensible table, so
    detection can recognise any CFW distro that follows the same
    "system + data (+ reclaimable Android userdata)" layout.  It returns
    *which* distro is installed and *where* its partitions live; it
    deliberately performs NO destructive action (see the note at the bottom
    of the file).

    IMPORTANT caveat on what is being matched
    -----------------------------------------
    The compare is against the GPT partition-entry *name* field (offset 0x38 of
    each entry). This GPT-name convention fits internal (eMMC/UFS) installs
    whose installer creates GPT entries literally named for the system/data
    partitions, reusing Android's "userdata". For SD-card images the distro
    identity typically lives in the *filesystem volume label* (FAT32 / ext4
    label) instead, while the GPT entry names are generic ("vfat",
    "userdata", ...). The table rows below reflect the distros' documented
    volume labels and are only meaningful for an internal install that
    mirrors those names into the GPT; they are NOT verified against an
    on-disk internal image.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>

#include <Library/DistroPartition.h>

/* GPT on-disk layout constants. */
#define GPT_HEADER_LBA          1
#define GPT_SIGNATURE           "EFI PART"     /* 8 bytes at header offset 0 */
#define GPT_HDR_OFF_PART_LBA    0x48           /* PartitionEntryLBA (8)      */
#define GPT_HDR_OFF_NUM_PARTS   0x50           /* NumberOfPartitionEntries(4)*/
#define GPT_HDR_OFF_PART_SIZE   0x54           /* SizeOfPartitionEntry (4)   */
#define GPT_ENT_OFF_FIRST_LBA   0x20           /* FirstLBA (8)               */
#define GPT_ENT_OFF_LAST_LBA    0x28           /* LastLBA (8)                */
#define GPT_ENT_OFF_NAME        0x38           /* PartitionName, 36 CHAR16   */
#define GPT_ENT_NAME_CHARS      36

/* Known distro schemes.  See the caveat in the file header: no row is
   verified yet.  A row matches a disk when BOTH SystemPartLabel and
   DataPartLabel appear as GPT partition-entry names on that disk.  If
   ReclaimPartLabel is non-NULL and also present, its location is reported. */
STATIC CONST DISTRO_PARTITION_SCHEME mKnownSchemes[] = {
  /* DistroName    System        Data        Reclaim (Android)  */
  { L"REG-Linux", L"REGLINUX", L"SHARE",    L"userdata"  },  /* unverified GPT names */
  { L"Batocera", L"BATOCERA", L"SHARE",    L"userdata"  },  /* unverified GPT names */
  { L"Knulli",   L"BATOCERA", L"SHARE",    L"userdata"  },  /* unverified GPT names */
};
#define SCHEME_COUNT (sizeof (mKnownSchemes) / sizeof (mKnownSchemes[0]))

STATIC UINT32 GptRead32 (CONST UINT8 *p) {
  return (UINT32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24));
}
STATIC UINT64 GptRead64 (CONST UINT8 *p) {
  return (UINT64)GptRead32 (p) | ((UINT64)GptRead32 (p + 4) << 32);
}

/**
  Read this disk's GPT partition-entry array into an allocated buffer.

  @param[out] Entries    Receives the allocated entry array.
  @param[out] NumParts   Receives NumberOfPartitionEntries.
  @param[out] PartSize   Receives SizeOfPartitionEntry (stride).
**/
STATIC
EFI_STATUS
ReadGptEntries (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  OUT UINT8                  **Entries,
  OUT UINT32                 *NumParts,
  OUT UINT32                 *PartSize
  )
{
  EFI_STATUS  Status;
  UINT32      BlockSize;
  UINT8       *Header;
  UINT64      PartLba;
  UINT32      NumEntries;
  UINT32      EntrySize;
  UINTN       ArrayBytes;
  UINTN       ArraySectors;
  UINT8       *Array;

  *Entries  = NULL;
  *NumParts = 0;
  *PartSize = 0;

  if (BlockIo->Media == NULL || !BlockIo->Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }
  BlockSize = BlockIo->Media->BlockSize;
  if (BlockSize == 0) {
    return EFI_UNSUPPORTED;
  }

  Header = AllocatePool (BlockSize);
  if (Header == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (
                      BlockIo, BlockIo->Media->MediaId,
                      GPT_HEADER_LBA, BlockSize, Header
                      );
  if (EFI_ERROR (Status) ||
      CompareMem (Header, GPT_SIGNATURE, 8) != 0) {
    FreePool (Header);
    return EFI_NOT_FOUND;
  }

  PartLba    = GptRead64 (Header + GPT_HDR_OFF_PART_LBA);
  NumEntries = GptRead32 (Header + GPT_HDR_OFF_NUM_PARTS);
  EntrySize  = GptRead32 (Header + GPT_HDR_OFF_PART_SIZE);
  FreePool (Header);

  if (NumEntries == 0 || EntrySize < 128 || NumEntries > 512) {
    return EFI_UNSUPPORTED;
  }

  ArrayBytes   = (UINTN)NumEntries * EntrySize;
  ArraySectors = (ArrayBytes + BlockSize - 1) / BlockSize;

  Array = AllocatePool (ArraySectors * BlockSize);
  if (Array == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (
                      BlockIo, BlockIo->Media->MediaId,
                      PartLba, ArraySectors * BlockSize, Array
                      );
  if (EFI_ERROR (Status)) {
    FreePool (Array);
    return Status;
  }

  *Entries  = Array;
  *NumParts = NumEntries;
  *PartSize = EntrySize;
  return EFI_SUCCESS;
}

/**
  Look for a partition entry whose name equals Label.  On success fills in the
  matched partition's location.

  @retval TRUE   Found; *Match populated.
  @retval FALSE  No entry with that name.
**/
STATIC
BOOLEAN
FindPartitionByName (
  IN  UINT8                    *Entries,
  IN  UINT32                   NumParts,
  IN  UINT32                   PartSize,
  IN  CONST CHAR16             *Label,
  OUT DISTRO_PARTITION_MATCH   *Match  OPTIONAL
  )
{
  UINT32  i;

  for (i = 0; i < NumParts; i++) {
    UINT8  *Ent  = Entries + ((UINTN)i * PartSize);
    CHAR16 *Name = (CHAR16 *)(Ent + GPT_ENT_OFF_NAME);
    UINT64  First = GptRead64 (Ent + GPT_ENT_OFF_FIRST_LBA);

    /* Skip unused entries (FirstLBA == 0 and empty name). */
    if (First == 0 && Name[0] == 0) {
      continue;
    }

    /* Names are up to 36 CHAR16, NUL-terminated within the field. */
    if (StrnCmp (Name, Label, GPT_ENT_NAME_CHARS) == 0) {
      if (Match != NULL) {
        Match->EntryIndex = i;
        Match->FirstLba   = First;
        Match->LastLba    = GptRead64 (Ent + GPT_ENT_OFF_LAST_LBA);
      }
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Detect which known CFW distro (if any) is installed.  See DistroPartition.h.
**/
EFI_STATUS
DetectDistroPartitionScheme (
  OUT DISTRO_PARTITION_RESULT  *Result
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;

  if (Result == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Result, sizeof (*Result));

  Handles     = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_BLOCK_IO_PROTOCOL  *BlockIo;
    UINT8                  *Entries;
    UINT32                 NumParts;
    UINT32                 PartSize;
    UINTN                  Scheme;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                                        &gEfiBlockIoProtocolGuid,
                                        (VOID **)&BlockIo))) {
      continue;
    }

    /* Only whole disks carry a GPT; skip logical (partition) BlockIo. */
    if (BlockIo->Media == NULL || BlockIo->Media->LogicalPartition) {
      continue;
    }

    if (EFI_ERROR (ReadGptEntries (BlockIo, &Entries, &NumParts, &PartSize))) {
      continue;
    }

    for (Scheme = 0; Scheme < SCHEME_COUNT; Scheme++) {
      CONST DISTRO_PARTITION_SCHEME  *S = &mKnownSchemes[Scheme];
      DISTRO_PARTITION_MATCH         SysMatch;
      DISTRO_PARTITION_MATCH         DataMatch;

      if (!FindPartitionByName (Entries, NumParts, PartSize, S->SystemPartLabel, &SysMatch)) {
        continue;
      }
      if (!FindPartitionByName (Entries, NumParts, PartSize, S->DataPartLabel, &DataMatch)) {
        continue;
      }

      /* A distro matched on this disk. */
      Result->Scheme     = S;
      Result->DiskHandle = Handles[Index];
      Result->System     = SysMatch;
      Result->Data       = DataMatch;

      Result->HasReclaim = FALSE;
      if (S->ReclaimPartLabel != NULL) {
        Result->HasReclaim = FindPartitionByName (
                               Entries, NumParts, PartSize,
                               S->ReclaimPartLabel, &Result->Reclaim
                               );
      }

      DEBUG ((EFI_D_INFO,
              "DistroDetect: found %s (system=%s data=%s reclaim=%a)\n",
              S->DistroName, S->SystemPartLabel, S->DataPartLabel,
              Result->HasReclaim ? "yes" : "no"));

      FreePool (Entries);
      FreePool (Handles);
      return EFI_SUCCESS;
    }

    FreePool (Entries);
  }

  FreePool (Handles);
  return Status;   /* EFI_NOT_FOUND: nothing recognised */
}

/*
 * Destructive actions (NUKE / UNINSTALL) are intentionally NOT implemented here.
 * ---------------------------------------------------------------------------
 * Two follow-on operations are conceivable once detection succeeds:
 *
 *   NUKE      - overwrites the first few MiB of the system partition's data
 *               with zeros, destroying its filesystem.
 *   UNINSTALL - real GPT surgery: zero the system and data partition
 *               entries, rewrite the Android "userdata" entry to start at the
 *               freed FirstLBA (absorbing the reclaimed space), recompute the
 *               partition-array and header CRC32s, and write back BOTH the
 *               primary and backup GPTs.
 *
 * Both are irreversible and operate on the internal eMMC/UFS GPT. They are
 * left out of this pass and should be designed and reviewed separately,
 * with real device-safety review, before any menu item is wired to them.
 * DetectDistroPartitionScheme() above returns all the location data (disk
 * handle, entry indices, LBA ranges) such an action would need, without
 * taking the action.
 */
