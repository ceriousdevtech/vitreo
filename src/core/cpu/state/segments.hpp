// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"

namespace vitreo::cpu {

/// Segment selector indices
enum class SegIndex : u8 {
    ES = 0,
    CS = 1,
    SS = 2,
    DS = 3,
    FS = 4,
    GS = 5,
};

/// Segment descriptor (cached from GDT/LDT)
struct SegmentDescriptor {
    u64 base;           // Base address
    u32 limit;          // Segment limit
    
    // Access byte fields
    bool present;       // Segment present
    u8   dpl;           // Descriptor privilege level (0-3)
    bool is_system;     // System segment (false for code/data)
    bool is_code;       // Code segment (executable)
    bool conforming;    // Conforming code segment
    bool readable;      // Readable (code) / Writable (data)
    bool accessed;      // Accessed bit
    
    // Flags
    bool granularity;   // Limit granularity (false=byte, true=4KB)
    bool big;           // Default operation size (D/B bit)
    bool long_mode;     // Long mode (L bit, 64-bit code)
    
    constexpr SegmentDescriptor() noexcept
        : base(0), limit(0xFFFFF), present(true), dpl(0), is_system(false),
          is_code(false), conforming(false), readable(true), accessed(false),
          granularity(true), big(true), long_mode(false) {}
    
    /// Create a flat 32-bit code segment
    [[nodiscard]] static SegmentDescriptor flat_code32() noexcept {
        SegmentDescriptor desc;
        desc.base = 0;
        desc.limit = 0xFFFFF;
        desc.present = true;
        desc.dpl = 0;
        desc.is_code = true;
        desc.readable = true;
        desc.granularity = true;
        desc.big = true;
        return desc;
    }
    
    /// Create a flat 32-bit data segment
    [[nodiscard]] static SegmentDescriptor flat_data32() noexcept {
        SegmentDescriptor desc;
        desc.base = 0;
        desc.limit = 0xFFFFF;
        desc.present = true;
        desc.dpl = 0;
        desc.is_code = false;
        desc.readable = true;  // writable for data
        desc.granularity = true;
        desc.big = true;
        return desc;
    }
    
    /// Create a 64-bit code segment
    [[nodiscard]] static SegmentDescriptor code64() noexcept {
        SegmentDescriptor desc;
        desc.base = 0;
        desc.limit = 0;  // Ignored in long mode
        desc.present = true;
        desc.dpl = 0;
        desc.is_code = true;
        desc.readable = true;
        desc.long_mode = true;
        desc.big = false;  // Must be 0 for long mode
        return desc;
    }
    
    /// Create a 64-bit data segment
    [[nodiscard]] static SegmentDescriptor data64() noexcept {
        SegmentDescriptor desc;
        desc.base = 0;
        desc.limit = 0;  // Ignored in long mode
        desc.present = true;
        desc.dpl = 0;
        desc.is_code = false;
        desc.readable = true;  // writable for data
        desc.long_mode = false;  // Only set for code
        return desc;
    }
    
    /// Get the effective limit in bytes
    [[nodiscard]] u64 effective_limit() const noexcept {
        if (granularity) {
            return (static_cast<u64>(limit) << 12) | 0xFFF;
        }
        return limit;
    }
};

/// Segment registers with cached descriptors
struct SegmentRegs {
    // Selector values (visible part)
    u16 es, cs, ss, ds, fs, gs;
    
    // Cached descriptors (hidden part)
    SegmentDescriptor es_desc;
    SegmentDescriptor cs_desc;
    SegmentDescriptor ss_desc;
    SegmentDescriptor ds_desc;
    SegmentDescriptor fs_desc;
    SegmentDescriptor gs_desc;
    
    constexpr SegmentRegs() noexcept
        : es(0), cs(0), ss(0), ds(0), fs(0), gs(0) {}
    
    /// Get selector by index
    [[nodiscard]] u16 get(SegIndex idx) const noexcept {
        switch (idx) {
            case SegIndex::ES: return es;
            case SegIndex::CS: return cs;
            case SegIndex::SS: return ss;
            case SegIndex::DS: return ds;
            case SegIndex::FS: return fs;
            case SegIndex::GS: return gs;
            default: return 0;
        }
    }
    
    /// Set selector by index
    void set(SegIndex idx, u16 value) noexcept {
        switch (idx) {
            case SegIndex::ES: es = value; break;
            case SegIndex::CS: cs = value; break;
            case SegIndex::SS: ss = value; break;
            case SegIndex::DS: ds = value; break;
            case SegIndex::FS: fs = value; break;
            case SegIndex::GS: gs = value; break;
        }
    }
    
    /// Get descriptor by index
    [[nodiscard]] const SegmentDescriptor& get_desc(SegIndex idx) const noexcept {
        switch (idx) {
            case SegIndex::ES: return es_desc;
            case SegIndex::CS: return cs_desc;
            case SegIndex::SS: return ss_desc;
            case SegIndex::DS: return ds_desc;
            case SegIndex::FS: return fs_desc;
            case SegIndex::GS: return gs_desc;
            default: return ds_desc;  // Should not happen
        }
    }
    
    /// Set up flat 32-bit mode segments
    void setup_flat32() noexcept {
        cs = 0x08;
        cs_desc = SegmentDescriptor::flat_code32();
        ds = ss = es = 0x10;
        ds_desc = ss_desc = es_desc = SegmentDescriptor::flat_data32();
        fs = gs = 0;
        fs_desc = gs_desc = SegmentDescriptor::flat_data32();
    }
    
    /// Set up 64-bit long mode segments
    void setup_long_mode() noexcept {
        cs = 0x08;
        cs_desc = SegmentDescriptor::code64();
        ds = ss = es = 0x10;
        ds_desc = ss_desc = es_desc = SegmentDescriptor::data64();
        fs = gs = 0;
        fs_desc = gs_desc = SegmentDescriptor::data64();
    }
};

} // namespace vitreo::cpu
