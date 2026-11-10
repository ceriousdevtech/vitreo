#pragma once

/// @file winmm_mmio_abi.hpp
/// @brief mmio* and aux* constants and structure layouts (mmiscapi.h,
/// mmeapi.h, mmsyscom.h; mmsystem.h wraps them in pshpack1).
///
/// Every value below is checked against the mingw-w64 SDK headers for both
/// i686 and x86_64 by tools/audit_winmm_mmio_constants.py (CTest
/// vitreo_winmm_mmio_constants_match_sdk). The layouts were also measured on
/// Windows by tools/oracle_probe/winmm_mmio_probe ("layout" lines). The names
/// are the SDK macro names so the audit can compare them one to one.

#include <vitreo/common.hpp>

namespace vitreo::winapi {

namespace mmio_sdk {
constexpr u32 MMIOERR_BASE = 256;
constexpr u32 MMIOERR_FILENOTFOUND = 257;
constexpr u32 MMIOERR_OUTOFMEMORY = 258;
constexpr u32 MMIOERR_CANNOTOPEN = 259;
constexpr u32 MMIOERR_CANNOTCLOSE = 260;
constexpr u32 MMIOERR_CANNOTREAD = 261;
constexpr u32 MMIOERR_CANNOTWRITE = 262;
constexpr u32 MMIOERR_CANNOTSEEK = 263;
constexpr u32 MMIOERR_CANNOTEXPAND = 264;
constexpr u32 MMIOERR_CHUNKNOTFOUND = 265;
constexpr u32 MMIOERR_UNBUFFERED = 266;
constexpr u32 MMIOERR_PATHNOTFOUND = 267;
constexpr u32 MMIOERR_ACCESSDENIED = 268;
constexpr u32 MMIOERR_SHARINGVIOLATION = 269;
constexpr u32 MMIOERR_NETWORKERROR = 270;
constexpr u32 MMIOERR_TOOMANYOPENFILES = 271;
constexpr u32 MMIOERR_INVALIDFILE = 272;

constexpr u32 MMIO_RWMODE = 0x00000003;
constexpr u32 MMIO_SHAREMODE = 0x00000070;
constexpr u32 MMIO_CREATE = 0x00001000;
constexpr u32 MMIO_PARSE = 0x00000100;
constexpr u32 MMIO_DELETE = 0x00000200;
constexpr u32 MMIO_EXIST = 0x00004000;
constexpr u32 MMIO_ALLOCBUF = 0x00010000;
constexpr u32 MMIO_GETTEMP = 0x00020000;
constexpr u32 MMIO_DIRTY = 0x10000000;
constexpr u32 MMIO_READ = 0x00000000;
constexpr u32 MMIO_WRITE = 0x00000001;
constexpr u32 MMIO_READWRITE = 0x00000002;
constexpr u32 MMIO_COMPAT = 0x00000000;
constexpr u32 MMIO_EXCLUSIVE = 0x00000010;
constexpr u32 MMIO_DENYWRITE = 0x00000020;
constexpr u32 MMIO_DENYREAD = 0x00000030;
constexpr u32 MMIO_DENYNONE = 0x00000040;
constexpr u32 MMIO_FHOPEN = 0x0010;
constexpr u32 MMIO_EMPTYBUF = 0x0010;
constexpr u32 MMIO_TOUPPER = 0x0010;
constexpr u32 MMIO_INSTALLPROC = 0x00010000;
constexpr u32 MMIO_GLOBALPROC = 0x10000000;
constexpr u32 MMIO_REMOVEPROC = 0x00020000;
constexpr u32 MMIO_UNICODEPROC = 0x01000000;
constexpr u32 MMIO_FINDPROC = 0x00040000;
constexpr u32 MMIO_FINDCHUNK = 0x0010;
constexpr u32 MMIO_FINDRIFF = 0x0020;
constexpr u32 MMIO_FINDLIST = 0x0040;
constexpr u32 MMIO_CREATERIFF = 0x0020;
constexpr u32 MMIO_CREATELIST = 0x0040;
constexpr u32 MMIOM_READ = 0;
constexpr u32 MMIOM_WRITE = 1;
constexpr u32 MMIOM_SEEK = 2;
constexpr u32 MMIOM_OPEN = 3;
constexpr u32 MMIOM_CLOSE = 4;
constexpr u32 MMIOM_WRITEFLUSH = 5;
constexpr u32 MMIOM_RENAME = 6;
constexpr u32 MMIOM_USER = 0x8000;
constexpr u32 FOURCC_RIFF = 0x46464952;
constexpr u32 FOURCC_LIST = 0x5453494C;
constexpr u32 FOURCC_DOS = 0x20534F44;
constexpr u32 FOURCC_MEM = 0x204D454D;
// SEEK_* are also host <stdio.h> macros: spelled with a _VALUE suffix here
// (the audit strips it).
constexpr u32 SEEK_SET_VALUE = 0;
constexpr u32 SEEK_CUR_VALUE = 1;
constexpr u32 SEEK_END_VALUE = 2;
constexpr u32 MMIO_DEFAULTBUFFER = 8192;

constexpr u32 AUX_MAPPER = 0xFFFFFFFFu;
// winnt.h: the access a DOS file's kernel32 HANDLE is adopted with.
constexpr u32 GENERIC_READ = 0x80000000u;
constexpr u32 GENERIC_WRITE = 0x40000000u;
} // namespace mmio_sdk

/// MMIOINFO member offsets and size for one bitness.
struct MmioInfoLayout {
    u32 off_dwFlags;
    u32 off_fccIOProc;
    u32 off_pIOProc;
    u32 off_wErrorRet;
    u32 off_htask;
    u32 off_cchBuffer;
    u32 off_pchBuffer;
    u32 off_pchNext;
    u32 off_pchEndRead;
    u32 off_pchEndWrite;
    u32 off_lBufOffset;
    u32 off_lDiskOffset;
    u32 off_adwInfo;
    u32 off_dwReserved1;
    u32 off_dwReserved2;
    u32 off_hmmio;
    u32 size;
};
constexpr MmioInfoLayout mmioinfo_layout_32{
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 60, 64, 68, 72};
constexpr MmioInfoLayout mmioinfo_layout_64{
    0, 4, 8, 16, 20, 28, 32, 40, 48, 56, 64, 68, 72, 84, 88, 92, 100};

/// MMCKINFO is five DWORDs on both bitnesses.
struct MmckInfoLayout {
    u32 off_ckid;
    u32 off_cksize;
    u32 off_fccType;
    u32 off_dwDataOffset;
    u32 off_dwFlags;
    u32 size;
};
constexpr MmckInfoLayout mmckinfo_layout{0, 4, 8, 12, 16, 20};

/// AUXCAPSA / AUXCAPSW sizes (identical on both bitnesses).
struct AuxCapsLayout {
    u32 size_a;
    u32 size_w;
};
constexpr AuxCapsLayout auxcaps_layout{48, 80};

[[nodiscard]] constexpr const MmioInfoLayout& mmioinfo_layout(bool is_64bit) noexcept {
    return is_64bit ? mmioinfo_layout_64 : mmioinfo_layout_32;
}

} // namespace vitreo::winapi
