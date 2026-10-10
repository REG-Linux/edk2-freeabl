/** @file EspTrace.c

  DEBUG ONLY: trace the firmware calls of a chainloaded EFI image (see
  EspTrace.h).  Built for the "GRUB hangs right after StartImage with no
  output" problem on the Odin: the screen shows the last calls GRUB made, the
  one it is stuck in (PENDING), or the exception it took.

  Copyright (c) 2026, REG-Linux contributors.
  SPDX-License-Identifier: GPL-3.0-or-later
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextInEx.h>

#include <Library/EspLog.h>
#include <Library/ShutdownServices.h>
#include <Library/EspTrace.h>

#include "EspTraceFont.h"

/* ------------------------------------------------------------------ */
/* Ring of calls                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
  CONST CHAR8  *Name;
  UINT64       A0;
  UINT64       A1;
  UINT64       Ret;
  UINT64       Ra;
  UINT32       Guid;      /* Data1 of the protocol GUID, 0 if none */
  UINT32       Seq;
  UINT32       Rep;       /* identical calls folded into this entry */
  UINT32       Sec;
  BOOLEAN      Done;
  CHAR8        Txt[20];   /* variable name / console text */
} TR_ENT;

#define TR_N       64
#define RANGE_N    64
#define BIO_N      256

STATIC TR_ENT            mTr[TR_N];
STATIC UINT32            mTrSeq;
STATIC UINT32            mFwCalls;
STATIC volatile BOOLEAN  mActive;
STATIC volatile UINT32   mSecs;
STATIC UINT64            mImgBase;
STATIC UINT64            mImgEnd;
STATIC UINT64            mRangeB[RANGE_N];
STATIC UINT64            mRangeE[RANGE_N];
STATIC UINTN             mRangeN;
STATIC CHAR8             mLastOut[64];
STATIC UINT32            mExecOk;
STATIC UINT32            mExecFail;
STATIC EFI_STATUS        mExecLastErr;
STATIC UINT32            mHidden;
STATIC UINT32            mPtEl;
STATIC UINT32            mPtChanged;
STATIC UINT32            mPtMiss;
STATIC EFI_BOOT_SERVICES mBs;      /* original table copy */

#define RA  ((UINT64)(UINTN)__builtin_return_address (0))

STATIC
BOOLEAN
FromImage (
  IN UINT64  Ra
  )
{
  UINTN  i;

  if (Ra >= mImgBase && Ra < mImgEnd) {
    return TRUE;
  }
  for (i = 0; i < mRangeN; i++) {
    if (Ra >= mRangeB[i] && Ra < mRangeE[i]) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
VOID
CopyTxt (
  OUT CHAR8         *Dst,
  IN  UINTN         Len,
  IN  CONST CHAR16  *Src
  )
{
  UINTN  i;

  for (i = 0; Src != NULL && Src[i] != 0 && i + 1 < Len; i++) {
    Dst[i] = (Src[i] >= 0x20 && Src[i] < 0x7f) ? (CHAR8)Src[i] : '.';
  }
  Dst[i] = '\0';
}

STATIC
TR_ENT *
TrBegin (
  IN CONST CHAR8     *Name,
  IN UINT64          A0,
  IN UINT64          A1,
  IN CONST EFI_GUID  *Guid,
  IN UINT64          Ra
  )
{
  TR_ENT  *E;
  UINT32  G;

  if (!mActive || !FromImage (Ra)) {
    mFwCalls++;
    return NULL;
  }
  G = (Guid != NULL) ? Guid->Data1 : 0;
  if (mTrSeq > 0) {
    E = &mTr[(mTrSeq - 1) % TR_N];
    if (E->Done && E->Name == Name && E->Ra == Ra && E->A0 == A0 &&
        E->A1 == A1 && E->Guid == G) {
      E->Rep++;
      E->Done = FALSE;
      E->Sec  = mSecs;
      return E;
    }
  }
  E = &mTr[mTrSeq % TR_N];
  E->Name   = Name;
  E->A0     = A0;
  E->A1     = A1;
  E->Ret    = 0;
  E->Ra     = Ra;
  E->Guid   = G;
  E->Seq    = mTrSeq++;
  E->Rep    = 0;
  E->Sec    = mSecs;
  E->Txt[0] = '\0';
  E->Done   = FALSE;
  return E;
}

STATIC
VOID
TrEnd (
  IN TR_ENT  *E,
  IN UINT64  Ret
  )
{
  if (E != NULL) {
    E->Ret  = Ret;
    E->Done = TRUE;
  }
}

/* ------------------------------------------------------------------ */
/* Screen output                                                       */
/* ------------------------------------------------------------------ */

#define SCALE  2
#define CW     (ESP_FONT_W * SCALE)
#define CH     (ESP_FONT_H * SCALE)

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL   *mGop;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *mBlt;
STATIC UINT32                         mW;
STATIC UINT32                         mH;

STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mBg     = { 0x40, 0x00, 0x00, 0 };
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mWhite  = { 0xff, 0xff, 0xff, 0 };
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mYellow = { 0x00, 0xff, 0xff, 0 };
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  mRed    = { 0x40, 0x40, 0xff, 0 };

STATIC
VOID
DrawStr (
  IN UINT32                               Row,
  IN CONST CHAR8                          *S,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Fg
  )
{
  UINT32  Col;
  UINT32  y;
  UINT32  x;
  UINT8   Bits;
  UINT32  Px;
  UINT32  Py;

  for (Col = 0; S[Col] != '\0' && (Col + 1) * CW <= mW; Col++) {
    CHAR8  C = S[Col];
    if (C < 0x20 || C > 0x7e) {
      C = '?';
    }
    for (y = 0; y < ESP_FONT_H; y++) {
      Bits = mEspFont[C - 0x20][y];
      for (x = 0; x < ESP_FONT_W; x++) {
        if ((Bits & (0x80 >> x)) == 0) {
          continue;
        }
        for (Py = 0; Py < SCALE; Py++) {
          for (Px = 0; Px < SCALE; Px++) {
            UINT32 X = Col * CW + x * SCALE + Px;
            UINT32 Y = Row * CH + y * SCALE + Py;
            if (X < mW && Y < mH) {
              mBlt[Y * mW + X] = *Fg;
            }
          }
        }
      }
    }
  }
}

STATIC
CONST CHAR8 *
GuidName (
  IN UINT32  D1
  )
{
  switch (D1) {
  case 0x964e5b21: return "BlockIo";
  case 0x964e5b22: return "SimpleFs";
  case 0xa77b2472: return "BlockIo2";
  case 0xce345171: return "DiskIo";
  case 0x09576e91: return "DevPath";
  case 0x5b1b31a1: return "LoadedImg";
  case 0xbc62157e: return "LdImgDevPath";
  case 0xa19832b9: return "SNP";
  case 0x9042a9de: return "GOP";
  case 0x387477c1: return "TextIn";
  case 0x387477c2: return "TextOut";
  case 0xdd9e7534: return "TextInEx";
  case 0xf42f7782: return "ConsoleCtl";
  case 0x3152bca5: return "RNG";
  case 0xf4560cf6: return "MemAttr";
  case 0x605dab50: return "ShimLock";
  case 0x607f766c: return "TCG2";
  case 0xf541796d: return "TCG";
  case 0x96751a3d: return "CcMeasure";
  case 0x4006c0c1: return "LoadFile2";
  case 0x56ec3091: return "LoadFile";
  case 0xb1b621d5: return "FdtTable";
  case 0xe617d64c: return "DtFixup";
  case 0x8be4df61: return "GlobalVar";
  case 0xbb25cf6f: return "SerialIo";
  case 0x03c4e603: return "PXE";
  case 0x26baccb1: return "CpuArch";
  default:         return NULL;
  }
}

STATIC
VOID
FmtAddr (
  OUT CHAR8   *Buf,
  IN  UINTN   Size,
  IN  UINT64  A
  )
{
  if (A >= mImgBase && A < mImgEnd) {
    AsciiSPrint (Buf, Size, "G+%lx", A - mImgBase);
  } else if (FromImage (A)) {
    AsciiSPrint (Buf, Size, "H:%lx", A);
  } else {
    AsciiSPrint (Buf, Size, "%lx", A);
  }
}

STATIC
VOID
FmtEntry (
  OUT CHAR8         *Line,
  IN  UINTN         Size,
  IN  CONST TR_ENT  *E
  )
{
  CHAR8        Ra[24];
  CHAR8        Ret[24];
  CHAR8        G[24];
  CONST CHAR8  *Gn;

  FmtAddr (Ra, sizeof (Ra), E->Ra);
  if (E->Done) {
    AsciiSPrint (Ret, sizeof (Ret), "%r", (EFI_STATUS)E->Ret);
  } else {
    AsciiStrCpyS (Ret, sizeof (Ret), "**PENDING**");
  }
  G[0] = '\0';
  if (E->Guid != 0) {
    Gn = GuidName (E->Guid);
    if (Gn != NULL) {
      AsciiSPrint (G, sizeof (G), " %a", Gn);
    } else {
      AsciiSPrint (G, sizeof (G), " g%08x", E->Guid);
    }
  }
  AsciiSPrint (Line, Size, "%u%a%u %a%a%a%a %lx %lx >%a %a",
               E->Seq, E->Rep ? "x" : ":", E->Rep + 1, E->Name, G,
               E->Txt[0] ? " " : "", E->Txt, E->A0, E->A1, Ret, Ra);
}

/* Log the ring into the EspLog (read with fastboot oem esp-log). */
STATIC
VOID
LogRing (
  IN CONST CHAR8  *Why
  )
{
  CHAR8   Line[160];
  UINT32  First;
  UINT32  i;

  AsciiSPrint (Line, sizeof (Line),
               "ESPTRACE %a t=%us calls=%u fw=%u img=%lx-%lx ranges=%u exec=%u/%u pte=%u miss=%u el%u hidden=%u last-out=\"%a\"\n",
               Why, mSecs, mTrSeq, mFwCalls, mImgBase, mImgEnd, (UINT32)mRangeN,
               mExecOk, mExecFail, mPtChanged, mPtMiss, mPtEl, mHidden, mLastOut);
  EspLogAppendAscii (Line);
  First = (mTrSeq > TR_N) ? mTrSeq - TR_N : 0;
  for (i = First; i < mTrSeq; i++) {
    FmtEntry (Line, sizeof (Line) - 1, &mTr[i % TR_N]);
    AsciiStrCatS (Line, sizeof (Line), "\n");
    EspLogAppendAscii (Line);
  }
}

STATIC
VOID
Dump (
  IN CONST CHAR8                 *Why,
  IN EFI_EXCEPTION_TYPE          Type,
  IN EFI_SYSTEM_CONTEXT_AARCH64  *C
  )
{
  CHAR8   Line[160];
  CHAR8   A1[24];
  CHAR8   A2[24];
  UINT32  Rows;
  UINT32  Row;
  UINT32  First;
  UINT32  i;
  UINTN   p;

  if (mGop == NULL || mBlt == NULL) {
    return;
  }
  for (p = 0; p < (UINTN)mW * mH; p++) {
    mBlt[p] = mBg;
  }
  Rows = mH / CH;
  Row  = 0;

  AsciiSPrint (Line, sizeof (Line), "ESPTRACE %a t=%us calls=%u fw=%u",
               Why, mSecs, mTrSeq, mFwCalls);
  DrawStr (Row++, Line, &mYellow);
  AsciiSPrint (Line, sizeof (Line), "img %lx-%lx ranges=%u",
               mImgBase, mImgEnd, (UINT32)mRangeN);
  DrawStr (Row++, Line, &mWhite);
  AsciiSPrint (Line, sizeof (Line), "exec ok=%u fail=%u pte=%u miss=%u el%u hidden=%u",
               mExecOk, mExecFail, mPtChanged, mPtMiss, mPtEl, mHidden);
  DrawStr (Row++, Line, &mWhite);
  AsciiSPrint (Line, sizeof (Line), "out:\"%a\"", mLastOut);
  DrawStr (Row++, Line, &mWhite);

  if (C != NULL) {
    FmtAddr (A1, sizeof (A1), C->ELR);
    AsciiSPrint (Line, sizeof (Line), "EXC type %u ESR %lx EC %x ELR %a",
                 (UINT32)Type, C->ESR, (UINT32)((C->ESR >> 26) & 0x3f), A1);
    DrawStr (Row++, Line, &mRed);
    FmtAddr (A2, sizeof (A2), C->LR);
    AsciiSPrint (Line, sizeof (Line), "FAR %lx LR %a SP %lx", C->FAR, A2, C->SP);
    DrawStr (Row++, Line, &mRed);
    AsciiSPrint (Line, sizeof (Line), "X0 %lx X1 %lx", C->X0, C->X1);
    DrawStr (Row++, Line, &mRed);
    AsciiSPrint (Line, sizeof (Line), "X2 %lx X3 %lx", C->X2, C->X3);
    DrawStr (Row++, Line, &mRed);
  }
  Row++;

  First = (mTrSeq > TR_N) ? mTrSeq - TR_N : 0;
  if (mTrSeq - First > Rows - Row) {
    First = mTrSeq - (Rows - Row);
  }
  for (i = First; i < mTrSeq; i++) {
    TR_ENT  *E = &mTr[i % TR_N];
    FmtEntry (Line, sizeof (Line), E);
    DrawStr (Row++, Line, E->Done ? &mWhite : &mYellow);
  }

  /* Command-mode panels only refresh through Blt; write the framebuffer
     too in case Blt itself is what is broken. */
  if (mGop->Mode != NULL && mGop->Mode->FrameBufferBase != 0 &&
      mGop->Mode->Info != NULL &&
      mGop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
    UINT32  *Fb = (UINT32 *)(UINTN)mGop->Mode->FrameBufferBase;
    UINT32  Pitch = mGop->Mode->Info->PixelsPerScanLine;
    UINT32  y;
    for (y = 0; y < mH; y++) {
      CopyMem (Fb + (UINTN)y * Pitch, mBlt + (UINTN)y * mW, mW * 4);
    }
  }
  mGop->Blt (mGop, mBlt, EfiBltBufferToVideo, 0, 0, 0, 0, mW, mH, 0);
}

/* ------------------------------------------------------------------ */
/* Exceptions and watchdog                                             */
/* ------------------------------------------------------------------ */

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu;
STATIC EFI_EVENT              mTimer;
STATIC BOOLEAN                mExcHooked[2];

STATIC
VOID
EFIAPI
ExcHandler (
  IN EFI_EXCEPTION_TYPE  Type,
  IN EFI_SYSTEM_CONTEXT  Ctx
  )
{
  STATIC BOOLEAN  In;

  if (!In) {
    In      = TRUE;
    mActive = FALSE;
    Dump (Type == EXCEPT_AARCH64_SERROR ? "SERROR" : "SYNC EXCEPTION",
          Type, Ctx.SystemContextAArch64);
    LogRing ("EXCEPTION");
  }
  for (;;) {
  }
}

STATIC
VOID
EFIAPI
TimerCb (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  mSecs++;
#ifdef ESP_DEBUG_CONOUT
  /* Debug builds only: a release must not draw over a GRUB menu left open. */
  if (mActive && mSecs >= 20 && (mSecs % 20) == 0) {
    Dump ("WATCHDOG", 0, NULL);
  }
#endif
}

/* ------------------------------------------------------------------ */
/* Fixes applied to what the image sees                                */
/* ------------------------------------------------------------------ */

/* 1. Make every page the image allocates executable: GRUB loads its modules
      into memory it allocates, and firmware with a no-execute policy on
      allocated memory faults on the first jump into a module. */
STATIC CONST BOOLEAN  mFixExec = TRUE;
/* 2. Only show the image the disk it was loaded from: GRUB probes every
      BlockIo device at startup, and the Odin has ~83 of them on UFS. */
STATIC CONST BOOLEAN  mFixHideOtherDisks = TRUE;

/* EFI_MEMORY_ATTRIBUTE_PROTOCOL (UEFI 2.10), not in this MdePkg. */
typedef struct _ESP_MEM_ATTR ESP_MEM_ATTR;
typedef EFI_STATUS (EFIAPI *ESP_MEM_ATTR_FN)(
  IN ESP_MEM_ATTR *This, IN EFI_PHYSICAL_ADDRESS Base, IN UINT64 Length, IN UINT64 Attributes);
struct _ESP_MEM_ATTR {
  VOID             *GetMemoryAttributes;
  ESP_MEM_ATTR_FN  SetMemoryAttributes;
  ESP_MEM_ATTR_FN  ClearMemoryAttributes;
};
STATIC EFI_GUID  mMemAttrGuid = {
  0xf4560cf6, 0x40ec, 0x4b4a, { 0xa1, 0x92, 0xbf, 0x1d, 0x57, 0xd0, 0xb1, 0x89 }
};

STATIC ESP_MEM_ATTR              *mMemAttr;
STATIC EFI_DEVICE_PATH_PROTOCOL  *mBootDisk;
STATIC UINTN                     mBootDiskLen;

/* Clear the execute-never bits on the live translation tables for
   [Base, Base+Len): the firmware's CpuArch SetMemoryAttributes() treats a
   write-back -> write-back change as a no-op and leaves XN set.  Identity
   mapped tables, 4 KB granule.  Clears UXN/PXN on the leaf (page or block)
   and UXNTable/PXNTable on the tables above it.  Returns entries changed,
   or -1 if the tables are not in the expected format. */
#define PTE_XN       ((1ULL << 53) | (1ULL << 54))
#define PTE_TBL_XN   ((1ULL << 59) | (1ULL << 60))
#define PTE_OA(d)    ((d) & 0x0000FFFFFFFFF000ULL)


STATIC
VOID
PtClean (
  IN UINT64  *Entry
  )
{
  __asm__ volatile ("dc civac, %0" : : "r" (Entry) : "memory");
}

STATIC
INTN
ClearXn (
  IN UINT64  Base,
  IN UINT64  Len
  )
{
  UINT64  CurEl;
  UINT64  Ttbr;
  UINT64  Tcr;
  UINT64  Va;
  UINT64  End;
  UINT32  VaBits;
  UINT32  Start;
  UINT32  Lvl;
  INTN    Changed = 0;

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurEl));
  mPtEl = (UINT32)((CurEl >> 2) & 3);
  if (mPtEl == 1) {
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r" (Ttbr));
    __asm__ volatile ("mrs %0, tcr_el1" : "=r" (Tcr));
  } else if (mPtEl == 2) {
    __asm__ volatile ("mrs %0, ttbr0_el2" : "=r" (Ttbr));
    __asm__ volatile ("mrs %0, tcr_el2" : "=r" (Tcr));
  } else {
    return -1;
  }
  if (((Tcr >> 14) & 3) != 0) {      /* TG0 must be 4 KB */
    return -1;
  }
  VaBits = 64 - (UINT32)(Tcr & 0x3f);
  Start  = (VaBits > 39) ? 0 : (VaBits > 30) ? 1 : 2;

  End = Base + Len;
  for (Va = Base & ~0xFFFULL; Va < End; ) {
    UINT64  *T    = (UINT64 *)(UINTN)(Ttbr & 0x0000FFFFFFFFFFFEULL);
    UINT64  Step  = SIZE_4KB;

    for (Lvl = Start; Lvl <= 3; Lvl++) {
      UINT32  Shift = 12 + 9 * (3 - Lvl);
      UINT64  *Ent  = &T[(Va >> Shift) & 0x1FF];
      UINT64  D     = *Ent;

      Step = 1ULL << Shift;
      if ((D & 1) == 0) {
        mPtMiss++;
        break;
      }
      if (Lvl < 3 && (D & 3) == 3) {     /* table */
        if ((D & PTE_TBL_XN) != 0) {
          *Ent = D & ~PTE_TBL_XN;
          PtClean (Ent);
          Changed++;
        }
        T = (UINT64 *)(UINTN)PTE_OA (D);
        continue;
      }
      if ((D & PTE_XN) != 0) {           /* block or page */
        *Ent = D & ~PTE_XN;
        PtClean (Ent);
        Changed++;
      }
      break;
    }
    Va = (Va & ~(Step - 1)) + Step;
  }

  __asm__ volatile ("dsb ish" : : : "memory");
  if (mPtEl == 1) {
    __asm__ volatile ("tlbi vmalle1" : : : "memory");
  } else {
    __asm__ volatile ("tlbi alle2" : : : "memory");
  }
  __asm__ volatile ("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" : : : "memory");
  return Changed;
}

STATIC
VOID
MakeExec (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINTN                 Pages
  )
{
  EFI_STATUS  S = EFI_UNSUPPORTED;

  INTN        Changed;

  if (mMemAttr != NULL) {
    S = mMemAttr->ClearMemoryAttributes (mMemAttr, Base, EFI_PAGES_TO_SIZE (Pages),
                                         EFI_MEMORY_XP);
  }
  /* Whatever the protocol said, make sure the tables really allow it. */
  Changed = ClearXn (Base, EFI_PAGES_TO_SIZE (Pages));
  if (Changed >= 0) {
    mPtChanged += (UINT32)Changed;
    S = EFI_SUCCESS;
  }
  if (EFI_ERROR (S)) {
    mExecFail++;
    mExecLastErr = S;
  } else {
    mExecOk++;
  }
}

STATIC
BOOLEAN
IsDiskGuid (
  IN CONST EFI_GUID  *G
  )
{
  return G != NULL && (CompareGuid (G, &gEfiBlockIoProtocolGuid) ||
                       CompareGuid (G, &gEfiBlockIo2ProtocolGuid) ||
                       CompareGuid (G, &gEfiDiskIoProtocolGuid));
}

STATIC
BOOLEAN
OnBootDisk (
  IN EFI_HANDLE  H
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;

  if (EFI_ERROR (mBs.HandleProtocol (H, &gEfiDevicePathProtocolGuid, (VOID **)&Dp))) {
    return FALSE;
  }
  return GetDevicePathSize (Dp) >= mBootDiskLen &&
         CompareMem (Dp, mBootDisk, mBootDiskLen) == 0;
}

/* Compact Handles[] to the boot disk and its partitions; returns the count. */
STATIC
UINTN
KeepBootDisk (
  IN OUT EFI_HANDLE  *Handles,
  IN     UINTN       N
  )
{
  UINTN  i;
  UINTN  j = 0;

  for (i = 0; i < N; i++) {
    if (OnBootDisk (Handles[i])) {
      Handles[j++] = Handles[i];
    } else {
      mHidden++;
    }
  }
  return j;
}

/* Device path of the disk the image was loaded from (the partition's path
   without its HardDrive node), or NULL. */
STATIC
VOID
FindBootDisk (
  IN EFI_HANDLE  DeviceHandle
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Dp;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_DEVICE_PATH_PROTOCOL  *Last = NULL;

  mBootDisk    = NULL;
  mBootDiskLen = 0;
  if (DeviceHandle == NULL ||
      EFI_ERROR (gBS->HandleProtocol (DeviceHandle, &gEfiDevicePathProtocolGuid,
                                      (VOID **)&Dp))) {
    return;
  }
  mBootDisk = DuplicateDevicePath (Dp);
  if (mBootDisk == NULL) {
    return;
  }
  for (Node = mBootDisk; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    Last = Node;
  }
  if (Last != NULL && DevicePathType (Last) == MEDIA_DEVICE_PATH &&
      DevicePathSubType (Last) == MEDIA_HARDDRIVE_DP) {
    SetDevicePathEndNode (Last);
  }
  mBootDiskLen = GetDevicePathSize (mBootDisk) - END_DEVICE_PATH_LENGTH;
  if (mBootDiskLen == 0) {
    FreePool (mBootDisk);
    mBootDisk = NULL;
  }
}

/* ------------------------------------------------------------------ */
/* Wrappers                                                            */
/* ------------------------------------------------------------------ */

STATIC EFI_RUNTIME_SERVICES             mRt;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  mOut;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *mOutP;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL   mIn;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL   *mInP;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mInExP;
STATIC EFI_INPUT_READ_KEY_EX            mInExRead;

STATIC EFI_BLOCK_IO_PROTOCOL  *mBio[BIO_N];
STATIC EFI_BLOCK_READ         mBioRead[BIO_N];
STATIC UINTN                  mBioN;

STATIC VOID Unhook (VOID);

#define WRAP(Name, A0, A1, Guid, Call)            \
  {                                               \
    TR_ENT      *E = TrBegin (Name, (UINT64)(A0), (UINT64)(A1), Guid, RA); \
    EFI_STATUS  S  = Call;                        \
    TrEnd (E, S);                                 \
    return S;                                     \
  }

STATIC EFI_STATUS EFIAPI
W_AllocatePages (EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN N, EFI_PHYSICAL_ADDRESS *Mem)
{
  UINT64      Ra = RA;
  TR_ENT      *E = TrBegin ("AllocPages", N, M, NULL, Ra);
  EFI_STATUS  S  = mBs.AllocatePages (T, M, N, Mem);

  /* Also for callers we do not trace: the kernel's EFI stub copies the
     kernel into pages it allocates and jumps there after ExitBootServices. */
  if (!EFI_ERROR (S) && mActive && mFixExec &&
      (E != NULL || M == EfiLoaderCode || M == EfiLoaderData)) {
    MakeExec (*Mem, N);
  }
  if (E != NULL && !EFI_ERROR (S)) {
    E->A1 = *Mem;
    if (mRangeN < RANGE_N) {
      mRangeB[mRangeN] = *Mem;
      mRangeE[mRangeN] = *Mem + EFI_PAGES_TO_SIZE (N);
      mRangeN++;
    }
  }
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_FreePages (EFI_PHYSICAL_ADDRESS Mem, UINTN N)
WRAP ("FreePages", Mem, N, NULL, mBs.FreePages (Mem, N))

STATIC EFI_STATUS EFIAPI
W_GetMemoryMap (UINTN *Sz, EFI_MEMORY_DESCRIPTOR *Map, UINTN *Key, UINTN *DSz, UINT32 *DVer)
WRAP ("GetMemMap", Sz ? *Sz : 0, 0, NULL, mBs.GetMemoryMap (Sz, Map, Key, DSz, DVer))

STATIC EFI_STATUS EFIAPI
W_AllocatePool (EFI_MEMORY_TYPE T, UINTN Sz, VOID **Buf)
WRAP ("AllocPool", Sz, T, NULL, mBs.AllocatePool (T, Sz, Buf))

STATIC EFI_STATUS EFIAPI
W_FreePool (VOID *Buf)
WRAP ("FreePool", (UINTN)Buf, 0, NULL, mBs.FreePool (Buf))

STATIC EFI_STATUS EFIAPI
W_CreateEvent (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Fn, VOID *Ctx, EFI_EVENT *Ev)
WRAP ("CreateEvent", Type, Tpl, NULL, mBs.CreateEvent (Type, Tpl, Fn, Ctx, Ev))

STATIC EFI_STATUS EFIAPI
W_CreateEventEx (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Fn, CONST VOID *Ctx,
                 CONST EFI_GUID *Grp, EFI_EVENT *Ev)
WRAP ("CreateEventEx", Type, Tpl, Grp, mBs.CreateEventEx (Type, Tpl, Fn, Ctx, Grp, Ev))

STATIC EFI_STATUS EFIAPI
W_SetTimer (EFI_EVENT Ev, EFI_TIMER_DELAY Type, UINT64 Trigger)
WRAP ("SetTimer", Type, Trigger, NULL, mBs.SetTimer (Ev, Type, Trigger))

STATIC EFI_STATUS EFIAPI
W_WaitForEvent (UINTN N, EFI_EVENT *Ev, UINTN *Idx)
WRAP ("WaitForEvent", N, (N && Ev) ? (UINTN)Ev[0] : 0, NULL, mBs.WaitForEvent (N, Ev, Idx))

STATIC EFI_STATUS EFIAPI
W_SignalEvent (EFI_EVENT Ev)
WRAP ("SignalEvent", (UINTN)Ev, 0, NULL, mBs.SignalEvent (Ev))

STATIC EFI_STATUS EFIAPI
W_CloseEvent (EFI_EVENT Ev)
WRAP ("CloseEvent", (UINTN)Ev, 0, NULL, mBs.CloseEvent (Ev))

STATIC EFI_STATUS EFIAPI
W_CheckEvent (EFI_EVENT Ev)
WRAP ("CheckEvent", (UINTN)Ev, 0, NULL, mBs.CheckEvent (Ev))

STATIC EFI_STATUS EFIAPI
W_HandleProtocol (EFI_HANDLE H, EFI_GUID *G, VOID **If)
WRAP ("HandleProt", (UINTN)H, 0, G, mBs.HandleProtocol (H, G, If))

STATIC EFI_STATUS EFIAPI
W_LocateHandle (EFI_LOCATE_SEARCH_TYPE T, EFI_GUID *G, VOID *Key, UINTN *Sz, EFI_HANDLE *Buf)
{
  TR_ENT      *E = TrBegin ("LocateHandle", T, Sz ? *Sz : 0, G, RA);
  EFI_STATUS  S  = mBs.LocateHandle (T, G, Key, Sz, Buf);

  if (E != NULL && !EFI_ERROR (S) && mFixHideOtherDisks && mBootDisk != NULL &&
      T == ByProtocol && IsDiskGuid (G) && Sz != NULL && Buf != NULL) {
    UINTN  N = KeepBootDisk (Buf, *Sz / sizeof (EFI_HANDLE));
    *Sz  = N * sizeof (EFI_HANDLE);
    E->A1 = N;
    if (N == 0) {
      S = EFI_NOT_FOUND;
    }
  }
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_LocateDevicePath (EFI_GUID *G, EFI_DEVICE_PATH_PROTOCOL **Dp, EFI_HANDLE *H)
WRAP ("LocateDevPath", 0, 0, G, mBs.LocateDevicePath (G, Dp, H))

STATIC EFI_STATUS EFIAPI
W_InstallConfigurationTable (EFI_GUID *G, VOID *T)
WRAP ("InstCfgTable", (UINTN)T, 0, G, mBs.InstallConfigurationTable (G, T))

STATIC EFI_STATUS EFIAPI
W_LoadImage (BOOLEAN Bp, EFI_HANDLE Parent, EFI_DEVICE_PATH_PROTOCOL *Dp, VOID *Src,
             UINTN Sz, EFI_HANDLE *H)
{
  TR_ENT                     *E = TrBegin ("LoadImage", (UINTN)Src, Sz, NULL, RA);
  EFI_STATUS                 S  = mBs.LoadImage (Bp, Parent, Dp, Src, Sz, H);
  EFI_LOADED_IMAGE_PROTOCOL  *Li;

  /* An image loaded by a traced image (GRUB -> kernel EFI stub) is traced
     too. */
  if (E != NULL && !EFI_ERROR (S) && mRangeN < RANGE_N &&
      !EFI_ERROR (mBs.HandleProtocol (*H, &gEfiLoadedImageProtocolGuid, (VOID **)&Li))) {
    E->A1 = (UINTN)Li->ImageBase;
    mRangeB[mRangeN] = (UINTN)Li->ImageBase;
    mRangeE[mRangeN] = (UINTN)Li->ImageBase + Li->ImageSize;
    mRangeN++;
  }
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_StartImage (EFI_HANDLE H, UINTN *Sz, CHAR16 **Data)
WRAP ("StartImage", (UINTN)H, 0, NULL, mBs.StartImage (H, Sz, Data))

STATIC EFI_STATUS EFIAPI
W_Exit (EFI_HANDLE H, EFI_STATUS St, UINTN Sz, CHAR16 *Data)
WRAP ("Exit", (UINTN)H, St, NULL, mBs.Exit (H, St, Sz, Data))

STATIC EFI_STATUS EFIAPI
W_ExitBootServices (EFI_HANDLE H, UINTN Key)
{
  TrBegin ("ExitBootSvc", (UINTN)H, Key, NULL, RA);
  LogRing ("EXIT-BOOT-SERVICES");
  /* Nothing of ours may run after this: no allocation here, so the
     caller's MapKey stays valid. */
  Unhook ();
  return mBs.ExitBootServices (H, Key);
}

STATIC EFI_STATUS EFIAPI
W_Stall (UINTN Us)
WRAP ("Stall", Us, 0, NULL, mBs.Stall (Us))

STATIC EFI_STATUS EFIAPI
W_SetWatchdogTimer (UINTN T, UINT64 Code, UINTN Sz, CHAR16 *Data)
WRAP ("SetWatchdog", T, Code, NULL, mBs.SetWatchdogTimer (T, Code, Sz, Data))

STATIC EFI_STATUS EFIAPI
W_ConnectController (EFI_HANDLE H, EFI_HANDLE *Drv, EFI_DEVICE_PATH_PROTOCOL *Dp, BOOLEAN Rec)
WRAP ("ConnectCtrl", (UINTN)H, Rec, NULL, mBs.ConnectController (H, Drv, Dp, Rec))

STATIC EFI_STATUS EFIAPI
W_DisconnectController (EFI_HANDLE H, EFI_HANDLE Drv, EFI_HANDLE Child)
WRAP ("DisconnCtrl", (UINTN)H, 0, NULL, mBs.DisconnectController (H, Drv, Child))

STATIC EFI_STATUS EFIAPI
W_OpenProtocol (EFI_HANDLE H, EFI_GUID *G, VOID **If, EFI_HANDLE Ag, EFI_HANDLE Ct, UINT32 At)
WRAP ("OpenProt", (UINTN)H, At, G, mBs.OpenProtocol (H, G, If, Ag, Ct, At))

STATIC EFI_STATUS EFIAPI
W_CloseProtocol (EFI_HANDLE H, EFI_GUID *G, EFI_HANDLE Ag, EFI_HANDLE Ct)
WRAP ("CloseProt", (UINTN)H, 0, G, mBs.CloseProtocol (H, G, Ag, Ct))

STATIC EFI_STATUS EFIAPI
W_LocateHandleBuffer (EFI_LOCATE_SEARCH_TYPE T, EFI_GUID *G, VOID *Key, UINTN *N, EFI_HANDLE **Buf)
{
  TR_ENT      *E = TrBegin ("LocHandleBuf", T, 0, G, RA);
  EFI_STATUS  S  = mBs.LocateHandleBuffer (T, G, Key, N, Buf);

  if (E != NULL && !EFI_ERROR (S) && N != NULL) {
    if (mFixHideOtherDisks && mBootDisk != NULL && T == ByProtocol && IsDiskGuid (G) &&
        Buf != NULL && *Buf != NULL) {
      *N = KeepBootDisk (*Buf, *N);
      if (*N == 0) {
        mBs.FreePool (*Buf);
        *Buf = NULL;
        S    = EFI_NOT_FOUND;
      }
    }
    E->A1 = *N;
  }
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_LocateProtocol (EFI_GUID *G, VOID *Reg, VOID **If)
WRAP ("LocateProt", 0, 0, G, mBs.LocateProtocol (G, Reg, If))

STATIC EFI_STATUS EFIAPI
W_GetNextMonotonicCount (UINT64 *C)
WRAP ("MonoCount", 0, 0, NULL, mBs.GetNextMonotonicCount (C))

/* Runtime services */

STATIC EFI_STATUS EFIAPI
W_GetVariable (CHAR16 *Name, EFI_GUID *G, UINT32 *Attr, UINTN *Sz, VOID *Data)
{
  TR_ENT      *E = TrBegin ("GetVar", Sz ? *Sz : 0, 0, G, RA);
  EFI_STATUS  S;

  if (E != NULL) {
    CopyTxt (E->Txt, sizeof (E->Txt), Name);
  }
  S = mRt.GetVariable (Name, G, Attr, Sz, Data);
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_SetVariable (CHAR16 *Name, EFI_GUID *G, UINT32 Attr, UINTN Sz, VOID *Data)
{
  TR_ENT      *E = TrBegin ("SetVar", Sz, Attr, G, RA);
  EFI_STATUS  S;

  if (E != NULL) {
    CopyTxt (E->Txt, sizeof (E->Txt), Name);
  }
  S = mRt.SetVariable (Name, G, Attr, Sz, Data);
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_GetNextVariableName (UINTN *Sz, CHAR16 *Name, EFI_GUID *G)
WRAP ("GetNextVar", Sz ? *Sz : 0, 0, NULL, mRt.GetNextVariableName (Sz, Name, G))

STATIC EFI_STATUS EFIAPI
W_GetTime (EFI_TIME *T, EFI_TIME_CAPABILITIES *Cap)
WRAP ("GetTime", 0, 0, NULL, mRt.GetTime (T, Cap))

STATIC VOID EFIAPI
W_ResetSystem (EFI_RESET_TYPE T, EFI_STATUS St, UINTN Sz, VOID *Data)
{
  TR_ENT  *E = TrBegin ("ResetSystem", T, Sz, NULL, RA);

  LogRing ("RESET");
  /* GRUB's "reboot" passes no reset data; the Qualcomm ResetSystem then
     returns without resetting (GRUB spins). Reboot the way the ABL does
     (fastboot reboot): RebootDevice() calls back here with its data. */
  if ((T == EfiResetCold || T == EfiResetWarm) && (Sz == 0 || Data == NULL)) {
    RebootDevice (NORMAL_MODE);
  }
  mRt.ResetSystem (T, St, Sz, Data);
  TrEnd (E, EFI_DEVICE_ERROR);   /* only reached if the reset failed */
}

/* Console */

STATIC EFI_STATUS EFIAPI
W_OutReset (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Ext)
WRAP ("OutReset", Ext, 0, NULL, mOut.Reset (This, Ext))

STATIC EFI_STATUS EFIAPI
W_OutputString (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Str)
{
  TR_ENT      *E = TrBegin ("OutString", 0, 0, NULL, RA);
  EFI_STATUS  S;

  if (E != NULL) {
    UINTN  i;
    UINTN  n = AsciiStrLen (mLastOut);
    CopyTxt (E->Txt, sizeof (E->Txt), Str);
    /* GRUB prints one character at a time: keep a rolling tail. */
    for (i = 0; Str != NULL && Str[i] != 0; i++) {
      if (n + 1 >= sizeof (mLastOut)) {
        CopyMem (mLastOut, mLastOut + 1, n);
        n--;
      }
      mLastOut[n++] = (Str[i] >= 0x20 && Str[i] < 0x7f) ? (CHAR8)Str[i] : '|';
      mLastOut[n]   = '\0';
    }
  }
  S = mOut.OutputString (This, Str);
  TrEnd (E, S);
  return S;
}

STATIC EFI_STATUS EFIAPI
W_TestString (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Str)
WRAP ("TestString", 0, 0, NULL, mOut.TestString (This, Str))

STATIC EFI_STATUS EFIAPI
W_QueryMode (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN M, UINTN *Cols, UINTN *Rows)
WRAP ("QueryMode", M, 0, NULL, mOut.QueryMode (This, M, Cols, Rows))

STATIC EFI_STATUS EFIAPI
W_SetMode (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN M)
WRAP ("SetMode", M, 0, NULL, mOut.SetMode (This, M))

STATIC EFI_STATUS EFIAPI
W_SetAttribute (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN A)
WRAP ("SetAttr", A, 0, NULL, mOut.SetAttribute (This, A))

STATIC EFI_STATUS EFIAPI
W_ClearScreen (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This)
WRAP ("ClearScreen", 0, 0, NULL, mOut.ClearScreen (This))

STATIC EFI_STATUS EFIAPI
W_SetCursorPosition (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN C, UINTN R)
WRAP ("SetCursorPos", C, R, NULL, mOut.SetCursorPosition (This, C, R))

STATIC EFI_STATUS EFIAPI
W_EnableCursor (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN On)
WRAP ("EnableCursor", On, 0, NULL, mOut.EnableCursor (This, On))

STATIC EFI_STATUS EFIAPI
W_InReset (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, BOOLEAN Ext)
WRAP ("InReset", Ext, 0, NULL, mIn.Reset (This, Ext))

/* The power key reaches EFI apps as SCAN_SUSPEND, which GRUB ignores: give
   it to the image as Enter, so the Odin's buttons drive GRUB (Vol = Up/Down,
   Power = select). */
STATIC VOID
PowerIsEnter (EFI_INPUT_KEY *Key)
{
  if (Key->ScanCode == SCAN_SUSPEND) {
    Key->ScanCode    = SCAN_NULL;
    Key->UnicodeChar = CHAR_CARRIAGE_RETURN;
  }
}

STATIC EFI_STATUS EFIAPI
W_ReadKeyStroke (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key)
{
  EFI_STATUS  S = mIn.ReadKeyStroke (This, Key);

  if (!EFI_ERROR (S) && mActive) {
    PowerIsEnter (Key);
  }
  return S;
}

STATIC EFI_STATUS EFIAPI
W_ReadKeyStrokeEx (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This, EFI_KEY_DATA *KeyData)
{
  EFI_STATUS  S = mInExRead (This, KeyData);

  if (!EFI_ERROR (S) && mActive) {
    PowerIsEnter (&KeyData->Key);
  }
  return S;
}

/* BlockIo: one wrapper for every instance, original found by This. */
STATIC EFI_STATUS EFIAPI
W_ReadBlocks (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Sz, VOID *Buf)
{
  UINTN       i;
  TR_ENT      *E;
  EFI_STATUS  S;

  for (i = 0; i < mBioN && mBio[i] != This; i++) {
  }
  if (i == mBioN) {
    return EFI_DEVICE_ERROR;
  }
  E = TrBegin ("BioRead", ((UINT64)i << 32) | Sz, Lba, NULL, RA);
  S = mBioRead[i] (This, MediaId, Lba, Sz, Buf);
  TrEnd (E, S);
  return S;
}

/* ------------------------------------------------------------------ */
/* Install / remove                                                    */
/* ------------------------------------------------------------------ */

STATIC BOOLEAN  mHooked;

STATIC
VOID
FixCrc (
  IN OUT EFI_TABLE_HEADER  *Hdr
  )
{
  UINT32  Crc = 0;

  Hdr->CRC32 = 0;
  mBs.CalculateCrc32 ((UINT8 *)Hdr, Hdr->HeaderSize, &Crc);
  Hdr->CRC32 = Crc;
}

STATIC
VOID
HookBlockIo (
  VOID
  )
{
  EFI_HANDLE             *H = NULL;
  UINTN                  N  = 0;
  UINTN                  i;
  UINTN                  j;
  EFI_BLOCK_IO_PROTOCOL  *Bio;

  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                          NULL, &N, &H))) {
    return;
  }
  for (i = 0; i < N && mBioN < BIO_N; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (H[i], &gEfiBlockIoProtocolGuid, (VOID **)&Bio))) {
      continue;
    }
    for (j = 0; j < mBioN && mBio[j] != Bio; j++) {
    }
    if (j < mBioN || Bio->ReadBlocks == W_ReadBlocks) {
      continue;
    }
    mBio[mBioN]     = Bio;
    mBioRead[mBioN] = Bio->ReadBlocks;
    Bio->ReadBlocks = W_ReadBlocks;
    mBioN++;
  }
  FreePool (H);
}

STATIC
VOID
Unhook (
  VOID
  )
{
  UINTN  i;

  if (!mHooked) {
    return;
  }
  mActive = FALSE;
  if (mTimer != NULL) {
    mBs.SetTimer (mTimer, TimerCancel, 0);
  }
  for (i = 0; i < 2; i++) {
    if (mExcHooked[i]) {
      mCpu->RegisterInterruptHandler (mCpu, i == 0 ? EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS
                                                   : EXCEPT_AARCH64_SERROR, NULL);
      mExcHooked[i] = FALSE;
    }
  }
  for (i = 0; i < mBioN; i++) {
    mBio[i]->ReadBlocks = mBioRead[i];
  }
  mBioN = 0;
  if (mOutP != NULL) {
    CopyMem (mOutP, &mOut, OFFSET_OF (EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, Mode));
  }
  if (mInP != NULL) {
    CopyMem (mInP, &mIn, OFFSET_OF (EFI_SIMPLE_TEXT_INPUT_PROTOCOL, WaitForKey));
  }
  if (mInExP != NULL) {
    mInExP->ReadKeyStrokeEx = mInExRead;
    mInExP = NULL;
  }
  CopyMem (gRT, &mRt, sizeof (mRt));
  CopyMem (gBS, &mBs, sizeof (mBs));
  FixCrc (&gBS->Hdr);
  FixCrc (&gRT->Hdr);
  mHooked = FALSE;
}

VOID
EspTraceStart (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *Li;
  UINTN                      i;

  if (mHooked) {
    return;
  }
  mTrSeq   = 0;
  mFwCalls = 0;
  mSecs    = 0;
  mRangeN  = 0;
  mImgBase = 0;
  mImgEnd  = 0;
  mLastOut[0] = '\0';
  mExecOk      = 0;
  mExecFail    = 0;
  mExecLastErr = EFI_SUCCESS;
  mHidden      = 0;
  mBootDisk    = NULL;
  mPtChanged   = 0;
  mPtMiss      = 0;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                       (VOID **)&Li))) {
    mImgBase = (UINT64)(UINTN)Li->ImageBase;
    mImgEnd  = mImgBase + Li->ImageSize;
    FindBootDisk (Li->DeviceHandle);
  }
  mMemAttr = NULL;
  gBS->LocateProtocol (&mMemAttrGuid, NULL, (VOID **)&mMemAttr);

  mGop = NULL;
  mBlt = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL,
                                       (VOID **)&mGop)) &&
      mGop->Mode != NULL && mGop->Mode->Info != NULL) {
    mW   = mGop->Mode->Info->HorizontalResolution;
    mH   = mGop->Mode->Info->VerticalResolution;
    mBlt = AllocatePool ((UINTN)mW * mH * sizeof (*mBlt));
  }

  mCpu = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu))) {
    for (i = 0; i < 2; i++) {
      EFI_EXCEPTION_TYPE  T = (i == 0) ? EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS
                                       : EXCEPT_AARCH64_SERROR;
      Status = mCpu->RegisterInterruptHandler (mCpu, T, ExcHandler);
      if (Status == EFI_ALREADY_STARTED) {
        mCpu->RegisterInterruptHandler (mCpu, T, NULL);
        Status = mCpu->RegisterInterruptHandler (mCpu, T, ExcHandler);
      }
      mExcHooked[i] = !EFI_ERROR (Status);
      EspPrint ("ESPTRACE: exception %u handler: %r\n", (UINT32)T, Status);
    }
  }

  mTimer = NULL;
  if (EFI_ERROR (gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                                   TimerCb, NULL, &mTimer)) ||
      EFI_ERROR (gBS->SetTimer (mTimer, TimerPeriodic, 10000000))) {
    EspPrint ("ESPTRACE: no watchdog timer\n");
  }

  HookBlockIo ();

  CopyMem (&mBs, gBS, sizeof (mBs));
  CopyMem (&mRt, gRT, sizeof (mRt));
  mOutP = gST->ConOut;
  mInP  = gST->ConIn;
  if (mOutP != NULL) {
    CopyMem (&mOut, mOutP, sizeof (mOut));
  }
  if (mInP != NULL) {
    CopyMem (&mIn, mInP, sizeof (mIn));
  }
  mInExP = NULL;
  if (gST->ConsoleInHandle != NULL &&
      !EFI_ERROR (gBS->HandleProtocol (gST->ConsoleInHandle,
                                       &gEfiSimpleTextInputExProtocolGuid,
                                       (VOID **)&mInExP)) &&
      mInExP != NULL) {
    mInExRead = mInExP->ReadKeyStrokeEx;
  } else {
    mInExP = NULL;
  }

  EspPrint ("ESPTRACE: img %lx-%lx, %u BlockIo, GOP %ux%u, tracing\n",
              mImgBase, mImgEnd, (UINT32)mBioN, mW, mH);
  EspPrint ("ESPTRACE: fix exec=%d via %a, hide other disks=%d (boot disk path %u bytes)\n",
              mFixExec, mMemAttr != NULL ? "MemoryAttribute+PT" : "PT",
              mFixHideOtherDisks, (UINT32)mBootDiskLen);

  gBS->AllocatePages            = W_AllocatePages;
  gBS->FreePages                = W_FreePages;
  gBS->GetMemoryMap             = W_GetMemoryMap;
  gBS->AllocatePool             = W_AllocatePool;
  gBS->FreePool                 = W_FreePool;
  gBS->CreateEvent              = W_CreateEvent;
  gBS->CreateEventEx            = W_CreateEventEx;
  gBS->SetTimer                 = W_SetTimer;
  gBS->WaitForEvent             = W_WaitForEvent;
  gBS->SignalEvent              = W_SignalEvent;
  gBS->CloseEvent               = W_CloseEvent;
  gBS->CheckEvent               = W_CheckEvent;
  gBS->HandleProtocol           = W_HandleProtocol;
  gBS->LocateHandle             = W_LocateHandle;
  gBS->LocateDevicePath         = W_LocateDevicePath;
  gBS->InstallConfigurationTable = W_InstallConfigurationTable;
  gBS->LoadImage                = W_LoadImage;
  gBS->StartImage               = W_StartImage;
  gBS->Exit                     = W_Exit;
  gBS->ExitBootServices         = W_ExitBootServices;
  gBS->Stall                    = W_Stall;
  gBS->SetWatchdogTimer         = W_SetWatchdogTimer;
  gBS->ConnectController        = W_ConnectController;
  gBS->DisconnectController     = W_DisconnectController;
  gBS->OpenProtocol             = W_OpenProtocol;
  gBS->CloseProtocol            = W_CloseProtocol;
  gBS->LocateHandleBuffer       = W_LocateHandleBuffer;
  gBS->LocateProtocol           = W_LocateProtocol;
  gBS->GetNextMonotonicCount    = W_GetNextMonotonicCount;
  gRT->GetVariable              = W_GetVariable;
  gRT->SetVariable              = W_SetVariable;
  gRT->GetNextVariableName      = W_GetNextVariableName;
  gRT->GetTime                  = W_GetTime;
  gRT->ResetSystem              = W_ResetSystem;
  if (mOutP != NULL) {
    mOutP->Reset             = W_OutReset;
    mOutP->OutputString      = W_OutputString;
    mOutP->TestString        = W_TestString;
    mOutP->QueryMode         = W_QueryMode;
    mOutP->SetMode           = W_SetMode;
    mOutP->SetAttribute      = W_SetAttribute;
    mOutP->ClearScreen       = W_ClearScreen;
    mOutP->SetCursorPosition = W_SetCursorPosition;
    mOutP->EnableCursor      = W_EnableCursor;
  }
  if (mInP != NULL) {
    mInP->Reset         = W_InReset;
    mInP->ReadKeyStroke = W_ReadKeyStroke;
  }
  if (mInExP != NULL) {
    mInExP->ReadKeyStrokeEx = W_ReadKeyStrokeEx;
  }
  FixCrc (&gBS->Hdr);
  FixCrc (&gRT->Hdr);
  mHooked = TRUE;
  mActive = TRUE;
}

VOID
EspTraceStop (
  VOID
  )
{
  BOOLEAN  Was = mHooked;

  Unhook ();
  if (Was) {
    LogRing ("RETURNED");
  }
  if (mTimer != NULL) {
    gBS->CloseEvent (mTimer);
    mTimer = NULL;
  }
  if (mBlt != NULL) {
    FreePool (mBlt);
    mBlt = NULL;
  }
  if (mBootDisk != NULL) {
    FreePool (mBootDisk);
    mBootDisk = NULL;
  }
}
