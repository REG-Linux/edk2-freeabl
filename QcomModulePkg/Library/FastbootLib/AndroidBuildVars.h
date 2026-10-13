/** @file AndroidBuildVars.h

  The Android build on the active slot, for fastboot getvar (see
  AndroidBuildVars.c).

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#ifndef __ANDROID_BUILD_VARS_H__
#define __ANDROID_BUILD_VARS_H__

#include <Uefi.h>

/* Strings stay valid; each is "" when not found. */
typedef struct {
  CONST CHAR8 *OsVersion;     /* e.g. "13" */
  CONST CHAR8 *SecurityPatch; /* e.g. "2024-05-05", or "2024-05" from a boot header */
  CONST CHAR8 *VbmetaSha256;  /* sha256 of the whole vbmeta<suffix> partition: first 16 bytes, lower-case hex */
} ANDROID_BUILD_VARS;

VOID
ReadAndroidBuildVars (IN CONST CHAR16 *Suffix, OUT ANDROID_BUILD_VARS *Vars);

#endif
