/** @file RawFat.c
    Minimal raw FAT16/FAT32 reader used as a fallback when the Qualcomm
    SimpleFileSystem (FAT) driver refuses to bind a card or partition.

    Background: on some SD cards / partitions the stock Qualcomm FAT driver
    rejects the volume (returning EFI_VOLUME_CORRUPTED / no SimpleFileSystem
    handle) even though the on-disk FAT is perfectly valid. This reader works
    around that by parsing the FAT directly over BlockIo instead of going
    through the SimpleFileSystem driver at all.

    Implemented against the standard EDK2 BlockIo API: given a file path it
    parses the BPB, walks the directory tree (8.3 names, sub-directories
    supported), follows the cluster chain and reads the file out.  It first
    tries the BlockIo behind a caller-supplied FS handle (if any), then falls
    back to probing every BlockIo handle in the system.

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

#include <Library/RawFat.h>

/* Parsed geometry of one FAT volume, all in units of the volume itself. */
typedef struct {
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  UINT32                  BytesPerSector;
  UINT32                  SectorsPerCluster;
  UINT32                  BytesPerCluster;
  UINT32                  ReservedSectors;
  UINT32                  NumFats;
  UINT32                  FatSectors;      /* sectors per FAT copy            */
  UINT32                  RootEntryCount;  /* FAT16 only (0 on FAT32)         */
  UINT32                  RootDirSectors;  /* FAT16 fixed root region         */
  UINT32                  FirstDataSector; /* LBA of cluster 2                */
  UINT32                  RootCluster;     /* FAT32 only                      */
  UINT32                  TotalClusters;
  BOOLEAN                 IsFat32;
} RAW_FAT_VOLUME;

#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_LFN        0x0F   /* long-file-name entry (skip)            */
#define FAT_DIRENT_SIZE     32
#define FAT_DELETED_ENTRY   0xE5
#define FAT_LAST_ENTRY      0x00

STATIC UINT16 FatRead16 (CONST UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
STATIC UINT32 FatRead32 (CONST UINT8 *p) {
  return (UINT32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24));
}

/**
  Read a run of contiguous sectors into Buffer via BlockIo.
**/
STATIC
EFI_STATUS
RawFatReadSectors (
  IN  RAW_FAT_VOLUME  *Vol,
  IN  UINT64          Lba,
  IN  UINTN           SectorCount,
  OUT VOID            *Buffer
  )
{
  return Vol->BlockIo->ReadBlocks (
                         Vol->BlockIo,
                         Vol->BlockIo->Media->MediaId,
                         Lba,
                         (UINTN)SectorCount * Vol->BytesPerSector,
                         Buffer
                         );
}

/**
  Parse the BPB from LBA 0 and fill in the volume geometry.

  @retval EFI_SUCCESS         Recognised as FAT16 or FAT32.
  @retval EFI_UNSUPPORTED     Not a FAT volume (or unsupported variant).
**/
STATIC
EFI_STATUS
RawFatMount (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  OUT RAW_FAT_VOLUME         *Vol
  )
{
  EFI_STATUS  Status;
  UINT8       *Bpb;
  UINT32      BlockSize;
  UINT32      TotalSectors;
  UINT32      DataSectors;

  if (BlockIo == NULL || BlockIo->Media == NULL || !BlockIo->Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }

  ZeroMem (Vol, sizeof (*Vol));
  Vol->BlockIo = BlockIo;
  BlockSize    = BlockIo->Media->BlockSize;
  if (BlockSize == 0) {
    return EFI_UNSUPPORTED;
  }

  Bpb = AllocatePool (BlockSize);
  if (Bpb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, BlockSize, Bpb);
  if (EFI_ERROR (Status)) {
    FreePool (Bpb);
    return Status;
  }

  /* Boot-sector signature 0x55 0xAA and a valid jump (0xEB.. or 0xE9..). */
  if (Bpb[510] != 0x55 || Bpb[511] != 0xAA ||
      (Bpb[0] != 0xEB && Bpb[0] != 0xE9)) {
    FreePool (Bpb);
    return EFI_UNSUPPORTED;
  }

  Vol->BytesPerSector    = FatRead16 (Bpb + 0x0B);
  Vol->SectorsPerCluster = Bpb[0x0D];
  Vol->ReservedSectors   = FatRead16 (Bpb + 0x0E);
  Vol->NumFats           = Bpb[0x10];
  Vol->RootEntryCount    = FatRead16 (Bpb + 0x11);

  /* The FAT bytes-per-sector must agree with the block device geometry, or
     none of our LBA maths lines up. */
  if (Vol->BytesPerSector != BlockSize ||
      Vol->SectorsPerCluster == 0 ||
      Vol->NumFats == 0) {
    FreePool (Bpb);
    return EFI_UNSUPPORTED;
  }

  Vol->FatSectors = FatRead16 (Bpb + 0x16);          /* FAT16 FATSz16 */
  if (Vol->FatSectors == 0) {
    Vol->FatSectors = FatRead32 (Bpb + 0x24);        /* FAT32 FATSz32 */
  }

  TotalSectors = FatRead16 (Bpb + 0x13);             /* TotSec16 */
  if (TotalSectors == 0) {
    TotalSectors = FatRead32 (Bpb + 0x20);           /* TotSec32 */
  }

  Vol->RootDirSectors =
    ((Vol->RootEntryCount * FAT_DIRENT_SIZE) + (Vol->BytesPerSector - 1)) /
    Vol->BytesPerSector;

  Vol->FirstDataSector =
    Vol->ReservedSectors + (Vol->NumFats * Vol->FatSectors) + Vol->RootDirSectors;

  Vol->BytesPerCluster = Vol->SectorsPerCluster * Vol->BytesPerSector;

  if (TotalSectors < Vol->FirstDataSector) {
    FreePool (Bpb);
    return EFI_UNSUPPORTED;
  }
  DataSectors        = TotalSectors - Vol->FirstDataSector;
  Vol->TotalClusters = DataSectors / Vol->SectorsPerCluster;

  /* FAT type is defined by the cluster count (per Microsoft's spec). */
  Vol->IsFat32 = (BOOLEAN)(Vol->TotalClusters >= 65525);
  if (Vol->IsFat32) {
    Vol->RootCluster = FatRead32 (Bpb + 0x2C);
  } else {
    Vol->RootCluster = 0;   /* FAT16 uses the fixed root region */
  }

  FreePool (Bpb);
  return EFI_SUCCESS;
}

/**
  Return the next cluster in the chain, or >= EOC when the chain ends.
**/
STATIC
EFI_STATUS
RawFatNextCluster (
  IN  RAW_FAT_VOLUME  *Vol,
  IN  UINT32          Cluster,
  OUT UINT32          *Next
  )
{
  EFI_STATUS  Status;
  UINT64      FatOffset;
  UINT64      SectorLba;
  UINT32      EntOffsetInSector;
  UINT8       *Sector;

  FatOffset = Vol->IsFat32 ? (UINT64)Cluster * 4 : (UINT64)Cluster * 2;
  SectorLba = Vol->ReservedSectors + (FatOffset / Vol->BytesPerSector);
  EntOffsetInSector = (UINT32)(FatOffset % Vol->BytesPerSector);

  Sector = AllocatePool (Vol->BytesPerSector);
  if (Sector == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = RawFatReadSectors (Vol, SectorLba, 1, Sector);
  if (EFI_ERROR (Status)) {
    FreePool (Sector);
    return Status;
  }

  if (Vol->IsFat32) {
    *Next = FatRead32 (Sector + EntOffsetInSector) & 0x0FFFFFFF;
  } else {
    *Next = FatRead16 (Sector + EntOffsetInSector);
    if (*Next >= 0xFFF8) {
      *Next |= 0x0FFF0000;   /* normalise so >= EOC test below works */
    }
  }

  FreePool (Sector);
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
RawFatIsEoc (
  IN RAW_FAT_VOLUME  *Vol,
  IN UINT32          Cluster
  )
{
  if (Vol->IsFat32) {
    return (BOOLEAN)(Cluster >= 0x0FFFFFF8);
  }
  return (BOOLEAN)(Cluster >= 0xFFF8);
}

STATIC
UINT64
RawFatClusterToLba (
  IN RAW_FAT_VOLUME  *Vol,
  IN UINT32          Cluster
  )
{
  return (UINT64)Vol->FirstDataSector +
         (UINT64)(Cluster - 2) * Vol->SectorsPerCluster;
}

/**
  Uppercase an ASCII byte (locale-independent).
**/
STATIC UINT8 RawFatUpper (UINT8 c) {
  return (UINT8)((c >= 'a' && c <= 'z') ? (c - 0x20) : c);
}

/**
  Convert one path component (CHAR16, already isolated) to a padded 11-byte
  8.3 directory-entry name for comparison.  Case-insensitive; anything past
  8 name chars / 3 extension chars is truncated, matching FAT short names.
**/
STATIC
VOID
RawFatMakeShortName (
  IN  CONST CHAR16  *Name,
  IN  UINTN         Len,
  OUT UINT8         Short[11]
  )
{
  UINTN  i;
  UINTN  Out;

  SetMem (Short, 11, ' ');

  /* Name part (up to 8 chars, stops at '.'). */
  Out = 0;
  for (i = 0; i < Len && Out < 8; i++) {
    if (Name[i] == L'.') {
      break;
    }
    Short[Out++] = RawFatUpper ((UINT8)Name[i]);
  }

  /* Skip to the extension, if any. */
  while (i < Len && Name[i] != L'.') {
    i++;
  }
  if (i < Len && Name[i] == L'.') {
    i++;   /* skip the dot */
    Out = 8;
    for (; i < Len && Out < 11; i++) {
      Short[Out++] = RawFatUpper ((UINT8)Name[i]);
    }
  }
}

/**
  Read a whole directory (given its first cluster, or 0 for the FAT16 root)
  into a freshly-allocated buffer.  *DirSize receives the byte length.
**/
STATIC
EFI_STATUS
RawFatReadDirectory (
  IN  RAW_FAT_VOLUME  *Vol,
  IN  UINT32          FirstCluster,
  OUT UINT8           **DirData,
  OUT UINTN           *DirSize
  )
{
  EFI_STATUS  Status;
  UINT8       *Buf;
  UINTN       Cap;
  UINTN       Used;
  UINT32      Cluster;

  *DirData = NULL;
  *DirSize = 0;

  /* FAT16 fixed root directory. */
  if (!Vol->IsFat32 && FirstCluster == 0) {
    UINTN  Bytes = (UINTN)Vol->RootDirSectors * Vol->BytesPerSector;
    UINT64 Lba   = Vol->ReservedSectors + (Vol->NumFats * Vol->FatSectors);

    Buf = AllocatePool (Bytes);
    if (Buf == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Status = RawFatReadSectors (Vol, Lba, Vol->RootDirSectors, Buf);
    if (EFI_ERROR (Status)) {
      FreePool (Buf);
      return Status;
    }
    *DirData = Buf;
    *DirSize = Bytes;
    return EFI_SUCCESS;
  }

  /* Cluster-chained directory (FAT32 root or any sub-directory). */
  Cluster = (FirstCluster == 0) ? Vol->RootCluster : FirstCluster;
  Cap  = 0;
  Used = 0;
  Buf  = NULL;

  while (Cluster >= 2 && !RawFatIsEoc (Vol, Cluster)) {
    UINT8  *NewBuf;

    NewBuf = AllocatePool (Cap + Vol->BytesPerCluster);
    if (NewBuf == NULL) {
      if (Buf != NULL) { FreePool (Buf); }
      return EFI_OUT_OF_RESOURCES;
    }
    if (Used > 0) {
      CopyMem (NewBuf, Buf, Used);
    }
    if (Buf != NULL) { FreePool (Buf); }
    Buf = NewBuf;
    Cap += Vol->BytesPerCluster;

    Status = RawFatReadSectors (
               Vol,
               RawFatClusterToLba (Vol, Cluster),
               Vol->SectorsPerCluster,
               Buf + Used
               );
    if (EFI_ERROR (Status)) {
      FreePool (Buf);
      return Status;
    }
    Used += Vol->BytesPerCluster;

    Status = RawFatNextCluster (Vol, Cluster, &Cluster);
    if (EFI_ERROR (Status)) {
      FreePool (Buf);
      return Status;
    }

    /* Bound directory growth to something sane (16 MiB). */
    if (Used > (16 * 1024 * 1024)) {
      break;
    }
  }

  *DirData = Buf;
  *DirSize = Used;
  return (Buf == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

/**
  Search a directory buffer for a short-name match.

  @param[out] FirstCluster  First cluster of the matched entry.
  @param[out] FileSize      File size (0 for directories).
  @param[out] IsDir         TRUE if the entry is a sub-directory.
**/
STATIC
EFI_STATUS
RawFatFindInDir (
  IN  UINT8    *Dir,
  IN  UINTN    DirSize,
  IN  UINT8    Short[11],
  OUT UINT32   *FirstCluster,
  OUT UINT32   *FileSize,
  OUT BOOLEAN  *IsDir
  )
{
  UINTN  Off;

  for (Off = 0; Off + FAT_DIRENT_SIZE <= DirSize; Off += FAT_DIRENT_SIZE) {
    UINT8  *Ent  = Dir + Off;
    UINT8  Attr  = Ent[0x0B];

    if (Ent[0] == FAT_LAST_ENTRY) {
      break;                                /* no more entries */
    }
    if (Ent[0] == FAT_DELETED_ENTRY) {
      continue;
    }
    if ((Attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
      continue;                             /* long-name fragment */
    }
    if ((Attr & FAT_ATTR_VOLUME_ID) != 0) {
      continue;                             /* volume label */
    }

    if (CompareMem (Ent, Short, 11) == 0) {
      *FirstCluster = ((UINT32)FatRead16 (Ent + 0x14) << 16) | FatRead16 (Ent + 0x1A);
      *FileSize     = FatRead32 (Ent + 0x1C);
      *IsDir        = (BOOLEAN)((Attr & FAT_ATTR_DIRECTORY) != 0);
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

/**
  Resolve a backslash-separated path to its directory entry, then read the
  file contents into a freshly-allocated buffer.
**/
STATIC
EFI_STATUS
RawFatReadPath (
  IN  RAW_FAT_VOLUME  *Vol,
  IN  CONST CHAR16    *Path,
  OUT VOID            **OutData,
  OUT UINTN           *OutSize
  )
{
  EFI_STATUS   Status;
  CONST CHAR16 *p;
  UINT32       DirCluster;
  UINT32       FirstCluster;
  UINT32       FileSize;
  BOOLEAN      IsDir;

  DirCluster = 0;   /* start at the volume root */
  FirstCluster = 0;
  FileSize = 0;
  IsDir = TRUE;

  p = Path;
  while (*p == L'\\' || *p == L'/') {
    p++;
  }

  while (*p != L'\0') {
    CONST CHAR16  *Start = p;
    UINTN         Len;
    UINT8         Short[11];
    UINT8         *Dir;
    UINTN         DirSize;

    while (*p != L'\0' && *p != L'\\' && *p != L'/') {
      p++;
    }
    Len = (UINTN)(p - Start);

    RawFatMakeShortName (Start, Len, Short);

    Status = RawFatReadDirectory (Vol, DirCluster, &Dir, &DirSize);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = RawFatFindInDir (Dir, DirSize, Short, &FirstCluster, &FileSize, &IsDir);
    FreePool (Dir);
    if (EFI_ERROR (Status)) {
      return EFI_NOT_FOUND;
    }

    while (*p == L'\\' || *p == L'/') {
      p++;
    }

    if (*p == L'\0') {
      break;                                /* last component reached */
    }
    if (!IsDir) {
      return EFI_NOT_FOUND;                 /* path continues but this is a file */
    }
    DirCluster = FirstCluster;
  }

  if (IsDir) {
    return EFI_NOT_FOUND;                    /* asked for a file, got a directory */
  }

  /* Follow the file's cluster chain and copy it out. */
  {
    UINT8   *Buf;
    UINTN   Copied;
    UINT32  Cluster;

    Buf = AllocatePool (FileSize + 1);
    if (Buf == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Copied  = 0;
    Cluster = FirstCluster;
    while (Cluster >= 2 && !RawFatIsEoc (Vol, Cluster) && Copied < FileSize) {
      UINTN   ClusterBytes = Vol->BytesPerCluster;
      UINTN   Remain       = FileSize - Copied;
      UINT8   *Tmp;

      Tmp = AllocatePool (ClusterBytes);
      if (Tmp == NULL) {
        FreePool (Buf);
        return EFI_OUT_OF_RESOURCES;
      }
      Status = RawFatReadSectors (
                 Vol,
                 RawFatClusterToLba (Vol, Cluster),
                 Vol->SectorsPerCluster,
                 Tmp
                 );
      if (EFI_ERROR (Status)) {
        FreePool (Tmp);
        FreePool (Buf);
        return Status;
      }
      CopyMem (Buf + Copied, Tmp, (Remain < ClusterBytes) ? Remain : ClusterBytes);
      FreePool (Tmp);
      Copied += (Remain < ClusterBytes) ? Remain : ClusterBytes;

      Status = RawFatNextCluster (Vol, Cluster, &Cluster);
      if (EFI_ERROR (Status)) {
        FreePool (Buf);
        return Status;
      }
    }

    Buf[FileSize] = '\0';   /* convenience NUL for text callers */
    *OutData = Buf;
    *OutSize = FileSize;
  }

  return EFI_SUCCESS;
}

/**
  Public entry point.  See RawFat.h.
**/
EFI_STATUS
RawFatReadFile (
  IN  EFI_HANDLE    FsHandleHint  OPTIONAL,
  IN  CONST CHAR16  *Path,
  OUT VOID          **OutData,
  OUT UINTN         *OutSize
  )
{
  return RawFatReadFileEx (FsHandleHint, Path, OutData, OutSize, NULL);
}

/**
  Same as RawFatReadFile(), and also returns the BlockIo handle (partition)
  the file was read from, so a caller can build a device path for it.
**/
EFI_STATUS
RawFatReadFileEx (
  IN  EFI_HANDLE    FsHandleHint  OPTIONAL,
  IN  CONST CHAR16  *Path,
  OUT VOID          **OutData,
  OUT UINTN         *OutSize,
  OUT EFI_HANDLE    *FromHandle   OPTIONAL
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  RAW_FAT_VOLUME         Vol;
  EFI_HANDLE             *Handles;
  UINTN                  HandleCount;
  UINTN                  Index;

  if (Path == NULL || OutData == NULL || OutSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *OutData = NULL;
  *OutSize = 0;

  /* 1. Preferred: the BlockIo directly behind the caller's FS handle. */
  if (FsHandleHint != NULL) {
    Status = gBS->HandleProtocol (FsHandleHint, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status) &&
        !EFI_ERROR (RawFatMount (BlockIo, &Vol)) &&
        !EFI_ERROR (RawFatReadPath (&Vol, Path, OutData, OutSize))) {
      DEBUG ((EFI_D_INFO, "RawFat: %s read directly from the given partition\n", Path));
      if (FromHandle != NULL) {
        *FromHandle = FsHandleHint;
      }
      return EFI_SUCCESS;
    }
  }

  /* 2. Fallback: probe every BlockIo handle in the system. */
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
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index],
                                        &gEfiBlockIoProtocolGuid,
                                        (VOID **)&BlockIo))) {
      continue;
    }
    if (EFI_ERROR (RawFatMount (BlockIo, &Vol))) {
      continue;
    }
    if (!EFI_ERROR (RawFatReadPath (&Vol, Path, OutData, OutSize))) {
      DEBUG ((EFI_D_INFO, "RawFat: %s read directly from block device %u (%u bytes)\n",
              Path, (UINT32)Index, (UINT32)*OutSize));
      if (FromHandle != NULL) {
        *FromHandle = Handles[Index];
      }
      Status = EFI_SUCCESS;
      break;
    }
  }

  FreePool (Handles);
  return Status;
}
