#pragma once

/// @file avifil32_contract.hpp
/// @brief AVIFile constants and structure layouts from vfw.h / aviriff.h.
///
/// Every value here is printed by tools/oracle_probe/avifil32_probe.c from
/// the SDK header it is compiled against ("avi const" / "avi layout" lines of
/// avifil32_probe{32,64}.expected); tests/winapi/media/test_avifil32.cpp
/// checks this file against those lines, so a typo fails the build's tests.

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::winapi::avifil32 {

// MAKE_AVIERR(n) = MAKE_SCODE(SEVERITY_ERROR, FACILITY_ITF, 0x4000 + n)
inline constexpr u32 AVIERR_OK = 0;
inline constexpr u32 AVIERR_UNSUPPORTED = 0x80044065u;
inline constexpr u32 AVIERR_BADFORMAT = 0x80044066u;
inline constexpr u32 AVIERR_MEMORY = 0x80044067u;
inline constexpr u32 AVIERR_INTERNAL = 0x80044068u;
inline constexpr u32 AVIERR_BADFLAGS = 0x80044069u;
inline constexpr u32 AVIERR_BADPARAM = 0x8004406Au;
inline constexpr u32 AVIERR_BADSIZE = 0x8004406Bu;
inline constexpr u32 AVIERR_BADHANDLE = 0x8004406Cu;
inline constexpr u32 AVIERR_FILEREAD = 0x8004406Du;
inline constexpr u32 AVIERR_FILEWRITE = 0x8004406Eu;
inline constexpr u32 AVIERR_FILEOPEN = 0x8004406Fu;
inline constexpr u32 AVIERR_COMPRESSOR = 0x80044070u;
inline constexpr u32 AVIERR_NOCOMPRESSOR = 0x80044071u;
inline constexpr u32 AVIERR_READONLY = 0x80044072u;
inline constexpr u32 AVIERR_NODATA = 0x80044073u;
inline constexpr u32 AVIERR_BUFFERTOOSMALL = 0x80044074u;
inline constexpr u32 AVIERR_CANTCOMPRESS = 0x80044075u;
inline constexpr u32 AVIERR_USERABORT = 0x800440C6u;
inline constexpr u32 AVIERR_ERROR = 0x800440C7u;

// winerror.h
inline constexpr u32 S_OK = 0;
inline constexpr u32 S_FALSE = 1;
inline constexpr u32 E_NOTIMPL = 0x80004001u;
inline constexpr u32 E_NOINTERFACE = 0x80004002u;
inline constexpr u32 E_POINTER = 0x80004003u;
inline constexpr u32 E_FAIL = 0x80004005u;
inline constexpr u32 REGDB_E_CLASSNOTREG = 0x80040154u;
inline constexpr u32 CLASS_E_CLASSNOTAVAILABLE = 0x80040111u;
inline constexpr u32 CO_E_NOTINITIALIZED = 0x800401F0u;

// FindSample flags
inline constexpr u32 FIND_DIR = 0x0000000F;
inline constexpr u32 FIND_NEXT = 0x00000001;
inline constexpr u32 FIND_PREV = 0x00000004;
inline constexpr u32 FIND_FROM_START = 0x00000008;
inline constexpr u32 FIND_TYPE = 0x000000F0;
inline constexpr u32 FIND_KEY = 0x00000010;
inline constexpr u32 FIND_ANY = 0x00000020;
inline constexpr u32 FIND_FORMAT = 0x00000040;
inline constexpr u32 FIND_RET = 0x0000F000;
inline constexpr u32 FIND_POS = 0x00000000;
inline constexpr u32 FIND_LENGTH = 0x00001000;
inline constexpr u32 FIND_OFFSET = 0x00002000;
inline constexpr u32 FIND_SIZE = 0x00003000;
inline constexpr u32 FIND_INDEX = 0x00004000;

inline constexpr i32 AVISTREAMREAD_CONVENIENT = -1;
inline constexpr u64 AVIGETFRAMEF_BESTDISPLAYFMT = 1;

inline constexpr u32 AVIFILEINFO_HASINDEX = 0x00000010;
inline constexpr u32 AVIFILEINFO_MUSTUSEINDEX = 0x00000020;
inline constexpr u32 AVIFILEINFO_ISINTERLEAVED = 0x00000100;
inline constexpr u32 AVIFILEINFO_TRUSTCKTYPE = 0x00000800;
inline constexpr u32 AVIFILEINFO_WASCAPTUREFILE = 0x00010000;
inline constexpr u32 AVIFILEINFO_COPYRIGHTED = 0x00020000;
inline constexpr u32 AVIFILECAPS_CANREAD = 0x00000001;
inline constexpr u32 AVIFILECAPS_CANWRITE = 0x00000002;
inline constexpr u32 AVIFILECAPS_ALLKEYFRAMES = 0x00000010;
inline constexpr u32 AVIFILECAPS_NOCOMPRESSION = 0x00000020;
inline constexpr u32 AVISTREAMINFO_DISABLED = 0x00000001;
inline constexpr u32 AVISTREAMINFO_FORMATCHANGES = 0x00010000;

inline constexpr u32 AVIIF_LIST = 0x00000001;
inline constexpr u32 AVIIF_KEYFRAME = 0x00000010;
inline constexpr u32 AVIIF_NOTIME = 0x00000100;

// mmioFOURCC
[[nodiscard]] constexpr u32 fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) |
           (static_cast<u32>(static_cast<u8>(d)) << 24);
}
inline constexpr u32 streamtypeVIDEO = fourcc('v', 'i', 'd', 's');
inline constexpr u32 streamtypeAUDIO = fourcc('a', 'u', 'd', 's');
inline constexpr u32 streamtypeMIDI = fourcc('m', 'i', 'd', 's');
inline constexpr u32 streamtypeTEXT = fourcc('t', 'x', 't', 's');

// OpenFile (winbase.h) modes AVIFileOpen takes.
inline constexpr u32 OF_READ = 0x0000;
inline constexpr u32 OF_WRITE = 0x0001;
inline constexpr u32 OF_READWRITE = 0x0002;
inline constexpr u32 OF_CREATE = 0x1000;

// wingdi.h
inline constexpr u32 BI_RGB = 0;
inline constexpr u32 BI_RLE8 = 1;
inline constexpr u32 BI_RLE4 = 2;
inline constexpr u32 BI_BITFIELDS = 3;

// Structure layouts (both bitnesses: every member is DWORD / WORD / RECT /
// TCHAR, so x86 and x64 agree).
inline constexpr u32 kAviFileInfoASize = 108;
inline constexpr u32 kAviFileInfoWSize = 172;
inline constexpr u32 kAviFileInfoTypeOffset = 44;
inline constexpr u32 kAviStreamInfoASize = 140;
inline constexpr u32 kAviStreamInfoWSize = 204;
inline constexpr u32 kAviStreamInfoCapsOffset = 12;
inline constexpr u32 kAviStreamInfoPriorityOffset = 16;
inline constexpr u32 kAviStreamInfoScaleOffset = 20;
inline constexpr u32 kAviStreamInfoRectOffset = 52;
inline constexpr u32 kAviStreamInfoEditCountOffset = 68;
inline constexpr u32 kAviStreamInfoNameOffset = 76;
inline constexpr u32 kAviNameChars = 64;
inline constexpr u32 kBitmapInfoHeaderSize = 40;

// GUIDs: DEFINE_AVIGUID(name, l, 0, 0) = {l, 0, 0, {C0,0,0,0,0,0,0,46}}.
inline constexpr u32 IID_IAVIFile_Data1 = 0x00020020;
inline constexpr u32 IID_IAVIStream_Data1 = 0x00020021;
inline constexpr u32 IID_IAVIStreaming_Data1 = 0x00020022;
inline constexpr u32 IID_IGetFrame_Data1 = 0x00020023;
inline constexpr u32 IID_IAVIEditStream_Data1 = 0x00020024;
inline constexpr u32 IID_IAVIPersistFile_Data1 = 0x00020025;
inline constexpr u32 CLSID_AVIFile_Data1 = 0x00020000;
/// HKCR\CLSID\{00020003-0000-0000-C000-000000000046}: avifil32's WAVE file
/// handler (HKCR\AVIFile\RIFFHandlers\WAVE and Extensions\WAV, measured).
inline constexpr u32 CLSID_WaveFile_Data1 = 0x00020003;
inline constexpr u32 IID_IUnknown_Data1 = 0x00000000;
inline constexpr u32 IID_IPersistFile_Data1 = 0x0000010B;

/// Build the 16 bytes of an OLE-style GUID {l-0000-0000-C000-000000000046}.
[[nodiscard]] constexpr std::array<u8, 16> ole_guid(u32 data1) noexcept {
    return {static_cast<u8>(data1), static_cast<u8>(data1 >> 8), static_cast<u8>(data1 >> 16),
            static_cast<u8>(data1 >> 24), 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
}

} // namespace vitreo::winapi::avifil32
