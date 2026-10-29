// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE/COFF header structures

#pragma once

#include "vitreo/types.hpp"

namespace vitreo::pe {

// ============================================================================
// DOS Header
// ============================================================================

/// DOS MZ header (64 bytes)
struct DosHeader {
    u16 e_magic;        ///< Magic number (MZ = 0x5A4D)
    u16 e_cblp;         ///< Bytes on last page of file
    u16 e_cp;           ///< Pages in file
    u16 e_crlc;         ///< Relocations
    u16 e_cparhdr;      ///< Size of header in paragraphs
    u16 e_minalloc;     ///< Minimum extra paragraphs needed
    u16 e_maxalloc;     ///< Maximum extra paragraphs needed
    u16 e_ss;           ///< Initial (relative) SS value
    u16 e_sp;           ///< Initial SP value
    u16 e_csum;         ///< Checksum
    u16 e_ip;           ///< Initial IP value
    u16 e_cs;           ///< Initial (relative) CS value
    u16 e_lfarlc;       ///< File address of relocation table
    u16 e_ovno;         ///< Overlay number
    u16 e_res[4];       ///< Reserved words
    u16 e_oemid;        ///< OEM identifier
    u16 e_oeminfo;      ///< OEM information
    u16 e_res2[10];     ///< Reserved words
    u32 e_lfanew;       ///< File address of new exe header
};

static_assert(sizeof(DosHeader) == 64, "DosHeader must be 64 bytes");

/// DOS magic number "MZ"
constexpr u16 DOS_MAGIC = 0x5A4D;

// ============================================================================
// PE Signature
// ============================================================================

/// PE signature "PE\0\0"
constexpr u32 PE_SIGNATURE = 0x00004550;

// ============================================================================
// COFF File Header
// ============================================================================

/// COFF file header (20 bytes)
struct CoffHeader {
    u16 Machine;              ///< Target machine type
    u16 NumberOfSections;     ///< Number of sections
    u32 TimeDateStamp;        ///< Time and date stamp
    u32 PointerToSymbolTable; ///< File offset of symbol table
    u32 NumberOfSymbols;      ///< Number of symbols
    u16 SizeOfOptionalHeader; ///< Size of optional header
    u16 Characteristics;      ///< Characteristics flags
};

static_assert(sizeof(CoffHeader) == 20, "CoffHeader must be 20 bytes");

/// Machine types
enum class MachineType : u16 {
    Unknown = 0x0,
    I386    = 0x014C,  ///< x86
    AMD64   = 0x8664,  ///< x64
    ARM64   = 0xAA64,  ///< ARM64
};

/// COFF characteristics flags
enum CoffCharacteristics : u16 {
    RELOCS_STRIPPED         = 0x0001,
    EXECUTABLE_IMAGE        = 0x0002,
    LINE_NUMS_STRIPPED      = 0x0004,
    LOCAL_SYMS_STRIPPED     = 0x0008,
    AGGRESSIVE_WS_TRIM      = 0x0010,
    LARGE_ADDRESS_AWARE     = 0x0020,
    BYTES_REVERSED_LO       = 0x0080,
    MACHINE_32BIT           = 0x0100,
    DEBUG_STRIPPED          = 0x0200,
    REMOVABLE_RUN_FROM_SWAP = 0x0400,
    NET_RUN_FROM_SWAP       = 0x0800,
    SYSTEM                  = 0x1000,
    DLL                     = 0x2000,
    UP_SYSTEM_ONLY          = 0x4000,
    BYTES_REVERSED_HI       = 0x8000,
};

// ============================================================================
// Optional Header
// ============================================================================

/// PE32 magic
constexpr u16 PE32_MAGIC = 0x10B;
/// PE32+ magic
constexpr u16 PE32PLUS_MAGIC = 0x20B;

/// Data directory entry
struct DataDirectory {
    u32 VirtualAddress;  ///< RVA of the data
    u32 Size;            ///< Size in bytes
};

static_assert(sizeof(DataDirectory) == 8, "DataDirectory must be 8 bytes");

/// Data directory indices
enum DataDirectoryIndex : u32 {
    DIR_EXPORT         = 0,
    DIR_IMPORT         = 1,
    DIR_RESOURCE       = 2,
    DIR_EXCEPTION      = 3,
    DIR_SECURITY       = 4,
    DIR_BASERELOC      = 5,
    DIR_DEBUG          = 6,
    DIR_ARCHITECTURE   = 7,
    DIR_GLOBALPTR      = 8,
    DIR_TLS            = 9,
    DIR_LOAD_CONFIG    = 10,
    DIR_BOUND_IMPORT   = 11,
    DIR_IAT            = 12,
    DIR_DELAY_IMPORT   = 13,
    DIR_CLR_RUNTIME    = 14,
    DIR_RESERVED       = 15,
    DIR_COUNT          = 16,
};

/// PE32 Optional Header (32-bit) - 96 bytes standard + data directories
struct OptionalHeader32 {
    // Standard fields (28 bytes)
    u16 Magic;
    u8  MajorLinkerVersion;
    u8  MinorLinkerVersion;
    u32 SizeOfCode;
    u32 SizeOfInitializedData;
    u32 SizeOfUninitializedData;
    u32 AddressOfEntryPoint;
    u32 BaseOfCode;
    u32 BaseOfData;
    
    // Windows-specific fields (68 bytes)
    u32 ImageBase;
    u32 SectionAlignment;
    u32 FileAlignment;
    u16 MajorOperatingSystemVersion;
    u16 MinorOperatingSystemVersion;
    u16 MajorImageVersion;
    u16 MinorImageVersion;
    u16 MajorSubsystemVersion;
    u16 MinorSubsystemVersion;
    u32 Win32VersionValue;
    u32 SizeOfImage;
    u32 SizeOfHeaders;
    u32 CheckSum;
    u16 Subsystem;
    u16 DllCharacteristics;
    u32 SizeOfStackReserve;
    u32 SizeOfStackCommit;
    u32 SizeOfHeapReserve;
    u32 SizeOfHeapCommit;
    u32 LoaderFlags;
    u32 NumberOfRvaAndSizes;
    
    // Data directories (variable, typically 16 * 8 = 128 bytes)
    DataDirectory DataDirectory[DIR_COUNT];
};

/// PE32+ Optional Header (64-bit) - 112 bytes standard + data directories
struct OptionalHeader64 {
    // Standard fields (24 bytes)
    u16 Magic;
    u8  MajorLinkerVersion;
    u8  MinorLinkerVersion;
    u32 SizeOfCode;
    u32 SizeOfInitializedData;
    u32 SizeOfUninitializedData;
    u32 AddressOfEntryPoint;
    u32 BaseOfCode;
    
    // Windows-specific fields (88 bytes)
    u64 ImageBase;
    u32 SectionAlignment;
    u32 FileAlignment;
    u16 MajorOperatingSystemVersion;
    u16 MinorOperatingSystemVersion;
    u16 MajorImageVersion;
    u16 MinorImageVersion;
    u16 MajorSubsystemVersion;
    u16 MinorSubsystemVersion;
    u32 Win32VersionValue;
    u32 SizeOfImage;
    u32 SizeOfHeaders;
    u32 CheckSum;
    u16 Subsystem;
    u16 DllCharacteristics;
    u64 SizeOfStackReserve;
    u64 SizeOfStackCommit;
    u64 SizeOfHeapReserve;
    u64 SizeOfHeapCommit;
    u32 LoaderFlags;
    u32 NumberOfRvaAndSizes;
    
    // Data directories
    DataDirectory DataDirectory[DIR_COUNT];
};

/// Subsystem types
enum class Subsystem : u16 {
    Unknown                 = 0,
    Native                  = 1,
    WindowsGui              = 2,
    WindowsCui              = 3,
    Os2Cui                  = 5,
    PosixCui                = 7,
    NativeWindows           = 8,
    WindowsCeGui            = 9,
    EfiApplication          = 10,
    EfiBootServiceDriver    = 11,
    EfiRuntimeDriver        = 12,
    EfiRom                  = 13,
    Xbox                    = 14,
    WindowsBootApplication  = 16,
};

/// DLL characteristics flags
enum DllCharacteristics : u16 {
    HIGH_ENTROPY_VA       = 0x0020,
    DYNAMIC_BASE          = 0x0040,
    FORCE_INTEGRITY       = 0x0080,
    NX_COMPAT             = 0x0100,
    NO_ISOLATION          = 0x0200,
    NO_SEH                = 0x0400,
    NO_BIND               = 0x0800,
    APPCONTAINER          = 0x1000,
    WDM_DRIVER            = 0x2000,
    GUARD_CF              = 0x4000,
    TERMINAL_SERVER_AWARE = 0x8000,
};

// ============================================================================
// Section Header
// ============================================================================

/// Section header (40 bytes)
struct SectionHeader {
    char Name[8];              ///< Section name (null-padded, not null-terminated if 8 chars)
    u32 VirtualSize;           ///< Virtual size (size when loaded)
    u32 VirtualAddress;        ///< RVA (relative virtual address)
    u32 SizeOfRawData;         ///< Size of raw data on disk
    u32 PointerToRawData;      ///< File offset to raw data
    u32 PointerToRelocations;  ///< File offset to relocations
    u32 PointerToLinenumbers;  ///< File offset to line numbers
    u16 NumberOfRelocations;   ///< Number of relocations
    u16 NumberOfLinenumbers;   ///< Number of line numbers
    u32 Characteristics;       ///< Section flags
};

static_assert(sizeof(SectionHeader) == 40, "SectionHeader must be 40 bytes");

/// Section characteristics flags
enum SectionCharacteristics : u32 {
    SCN_TYPE_NO_PAD            = 0x00000008,
    SCN_CNT_CODE               = 0x00000020,
    SCN_CNT_INITIALIZED_DATA   = 0x00000040,
    SCN_CNT_UNINITIALIZED_DATA = 0x00000080,
    SCN_LNK_OTHER              = 0x00000100,
    SCN_LNK_INFO               = 0x00000200,
    SCN_LNK_REMOVE             = 0x00000800,
    SCN_LNK_COMDAT             = 0x00001000,
    SCN_GPREL                  = 0x00008000,
    SCN_MEM_PURGEABLE          = 0x00020000,
    SCN_MEM_16BIT              = 0x00020000,
    SCN_MEM_LOCKED             = 0x00040000,
    SCN_MEM_PRELOAD            = 0x00080000,
    SCN_ALIGN_1BYTES           = 0x00100000,
    SCN_ALIGN_2BYTES           = 0x00200000,
    SCN_ALIGN_4BYTES           = 0x00300000,
    SCN_ALIGN_8BYTES           = 0x00400000,
    SCN_ALIGN_16BYTES          = 0x00500000,
    SCN_ALIGN_32BYTES          = 0x00600000,
    SCN_ALIGN_64BYTES          = 0x00700000,
    SCN_ALIGN_128BYTES         = 0x00800000,
    SCN_ALIGN_256BYTES         = 0x00900000,
    SCN_ALIGN_512BYTES         = 0x00A00000,
    SCN_ALIGN_1024BYTES        = 0x00B00000,
    SCN_ALIGN_2048BYTES        = 0x00C00000,
    SCN_ALIGN_4096BYTES        = 0x00D00000,
    SCN_ALIGN_8192BYTES        = 0x00E00000,
    SCN_LNK_NRELOC_OVFL        = 0x01000000,
    SCN_MEM_DISCARDABLE        = 0x02000000,
    SCN_MEM_NOT_CACHED         = 0x04000000,
    SCN_MEM_NOT_PAGED          = 0x08000000,
    SCN_MEM_SHARED             = 0x10000000,
    SCN_MEM_EXECUTE            = 0x20000000,
    SCN_MEM_READ               = 0x40000000,
    SCN_MEM_WRITE              = 0x80000000,
};

// ============================================================================
// Import Directory
// ============================================================================

/// Import directory entry (20 bytes)
struct ImportDescriptor {
    union {
        u32 Characteristics;        ///< 0 for terminating null import descriptor
        u32 OriginalFirstThunk;     ///< RVA to original unbound IAT (INT)
    };
    u32 TimeDateStamp;              ///< Time/date stamp
    u32 ForwarderChain;             ///< -1 if no forwarders
    u32 Name;                       ///< RVA of module name
    u32 FirstThunk;                 ///< RVA to IAT
};

static_assert(sizeof(ImportDescriptor) == 20, "ImportDescriptor must be 20 bytes");

/// Import by name entry
struct ImportByName {
    u16 Hint;       ///< Index into export name table
    char Name[1];   ///< Variable-length null-terminated name
};

// ============================================================================
// Export Directory
// ============================================================================

/// Export directory table
struct ExportDirectory {
    u32 Characteristics;
    u32 TimeDateStamp;
    u16 MajorVersion;
    u16 MinorVersion;
    u32 Name;                   ///< RVA of DLL name
    u32 Base;                   ///< Ordinal base
    u32 NumberOfFunctions;      ///< Number of entries in EAT
    u32 NumberOfNames;          ///< Number of name pointers
    u32 AddressOfFunctions;     ///< RVA of EAT
    u32 AddressOfNames;         ///< RVA of name pointer table
    u32 AddressOfNameOrdinals;  ///< RVA of ordinal table
};

static_assert(sizeof(ExportDirectory) == 40, "ExportDirectory must be 40 bytes");

// ============================================================================
// Base Relocation
// ============================================================================

/// Base relocation block header
struct BaseRelocationBlock {
    u32 VirtualAddress;  ///< Page RVA
    u32 SizeOfBlock;     ///< Size including header
    // Followed by variable number of u16 entries
};

static_assert(sizeof(BaseRelocationBlock) == 8, "BaseRelocationBlock must be 8 bytes");

/// Base relocation types (top 4 bits of each entry)
enum RelocationType : u16 {
    RELOC_ABSOLUTE       = 0,   ///< Padding, skip
    RELOC_HIGH           = 1,   ///< Add high 16 bits of delta
    RELOC_LOW            = 2,   ///< Add low 16 bits of delta
    RELOC_HIGHLOW        = 3,   ///< Add full 32-bit delta (x86)
    RELOC_HIGHADJ        = 4,   ///< Add high 16 bits with adjustment
    RELOC_DIR64          = 10,  ///< Add full 64-bit delta (x64)
};

} // namespace vitreo::pe
