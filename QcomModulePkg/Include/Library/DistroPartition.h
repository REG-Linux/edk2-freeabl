/** @file DistroPartition.h
    Data-driven detection of an installed retro-gaming CFW distro by its GPT
    partition layout.  Detection only — no destructive action is performed.

    Copyright (c) 2026, REG-Linux contributors.
    SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __DISTRO_PARTITION_H__
#define __DISTRO_PARTITION_H__

#include <Uefi.h>

/* One distro's expected GPT partition-entry names. */
typedef struct {
  CONST CHAR16  *DistroName;         /* for display / logging               */
  CONST CHAR16  *SystemPartLabel;    /* CFW OS partition, e.g. L"REGLINUX"   */
  CONST CHAR16  *DataPartLabel;      /* CFW data partition, e.g. L"SHARE"    */
  CONST CHAR16  *ReclaimPartLabel;   /* Android partition to expand on       */
                                     /* uninstall (e.g. L"userdata"); may    */
                                     /* be NULL if not applicable            */
} DISTRO_PARTITION_SCHEME;

/* Location of one matched partition on the disk. */
typedef struct {
  UINT32  EntryIndex;   /* index into the GPT partition-entry array */
  UINT64  FirstLba;
  UINT64  LastLba;
} DISTRO_PARTITION_MATCH;

/* Outcome of a successful detection. */
typedef struct {
  CONST DISTRO_PARTITION_SCHEME  *Scheme;      /* which distro matched       */
  EFI_HANDLE                     DiskHandle;   /* whole-disk BlockIo handle  */
  DISTRO_PARTITION_MATCH         System;       /* system partition location  */
  DISTRO_PARTITION_MATCH         Data;         /* data partition location    */
  BOOLEAN                        HasReclaim;   /* Reclaim below is valid      */
  DISTRO_PARTITION_MATCH         Reclaim;      /* Android userdata location   */
} DISTRO_PARTITION_RESULT;

/**
  Scan every whole disk's GPT and report the first known CFW distro whose
  system and data partitions are both present.

  Table-driven, so new distro schemes can be added without touching the scan
  logic itself.  It performs NO destructive action; the returned handle /
  entry indices / LBA ranges are what a later (separately reviewed) nuke or
  uninstall action would consume.

  @param[out] Result  Populated on EFI_SUCCESS; zeroed otherwise.

  @retval EFI_SUCCESS            A known distro was detected.
  @retval EFI_NOT_FOUND          No known distro layout found on any disk.
  @retval EFI_INVALID_PARAMETER  Result was NULL.
**/
EFI_STATUS
DetectDistroPartitionScheme (
  OUT DISTRO_PARTITION_RESULT  *Result
  );

#endif /* __DISTRO_PARTITION_H__ */
