// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_rtl_memory_compression.cpp
/// @brief Extended ntdll export table, part 5 of 11: Rtl memory primitives and
///        the RTL compression API.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace ntdll;

// kernel32 exports RtlZeroMemory (and on x64 RtlMoveMemory) itself and
// forwards the rest to these, so they are shared (ntdll_ext.hpp).
void impl_RtlZeroMemory(Marshaler& m) {
    GuestAddr dest = m.arg_ptr(0);
    usize len = static_cast<usize>(m.arg(1));
    if (len > 0) {
        Byte* dp = m.memory().host_ptr_mut(dest, len);
        if (dp) {
            std::memset(dp, 0, len);
        } else {
            Byte zero[4096] = {};
            GuestAddr pos = dest;
            usize rem = len;
            while (rem > 0) {
                usize chunk = std::min(rem, sizeof(zero));
                (void)m.memory().write_bytes(pos, zero, chunk);
                pos += chunk;
                rem -= chunk;
            }
        }
    }
    m.set_return(0);
}

void impl_RtlMoveMemory(Marshaler& m) {
    GuestAddr dest = m.arg_ptr(0);
    GuestAddr src  = m.arg_ptr(1);
    usize len = static_cast<usize>(m.arg(2));
    if (len > 0) {
        Byte* dp = m.memory().host_ptr_mut(dest, len);
        const Byte* sp = m.memory().host_ptr(src, len);
        if (dp && sp) {
            std::memmove(dp, sp, len);
        } else {
            std::vector<Byte> buf(len);
            (void)m.memory().read_bytes(src, buf.data(), len);
            (void)m.memory().write_bytes(dest, buf.data(), len);
        }
    }
    m.set_return(0);
}

void append_rtl_memory_compression_exports(NtdllExtManager& /*mgr*/,
                                           DllExports& exports, u16& ordinal) {
    // RtlCopyMemory (3 args, stdcall_bytes=12)
    // void RtlCopyMemory(VOID* dest, const VOID* src, SIZE_T length)
    exports.entries.push_back(ExportEntry{
        "RtlCopyMemory", ordinal++,
        [](Marshaler& m) {
            GuestAddr dest = m.arg_ptr(0);
            GuestAddr src  = m.arg_ptr(1);
            usize len = static_cast<usize>(m.arg(2));
            if (len > 0) {
                Byte* dp = m.memory().host_ptr_mut(dest, len);
                const Byte* sp = m.memory().host_ptr(src, len);
                if (dp && sp) {
                    std::memcpy(dp, sp, len);
                } else {
                    // Cross-region fallback
                    std::vector<Byte> buf(len);
                    (void)m.memory().read_bytes(src, buf.data(), len);
                    (void)m.memory().write_bytes(dest, buf.data(), len);
                }
            }
            m.set_return(0); // void return, STATUS_SUCCESS
        },
        12
    });

    // RtlZeroMemory (2 args, stdcall_bytes=8)
    // void RtlZeroMemory(VOID* dest, SIZE_T length)
    exports.entries.push_back(ExportEntry{
        "RtlZeroMemory", ordinal++,
        impl_RtlZeroMemory,
        8
    });

    // RtlMoveMemory (3 args, stdcall_bytes=12)
    // void RtlMoveMemory(VOID* dest, const VOID* src, SIZE_T length)
    exports.entries.push_back(ExportEntry{
        "RtlMoveMemory", ordinal++,
        impl_RtlMoveMemory,
        12
    });

    // RtlFillMemory (3 args, stdcall_bytes=12)
    // void RtlFillMemory(VOID* dest, SIZE_T length, UCHAR fill)
    exports.entries.push_back(ExportEntry{
        "RtlFillMemory", ordinal++,
        [](Marshaler& m) {
            GuestAddr dest = m.arg_ptr(0);
            usize len = static_cast<usize>(m.arg(1));
            u8 fill = static_cast<u8>(m.arg(2) & 0xFF);
            if (len > 0) {
                Byte* dp = m.memory().host_ptr_mut(dest, len);
                if (dp) {
                    std::memset(dp, fill, len);
                } else {
                    Byte buf[4096];
                    std::memset(buf, fill, sizeof(buf));
                    GuestAddr pos = dest;
                    usize rem = len;
                    while (rem > 0) {
                        usize chunk = std::min(rem, sizeof(buf));
                        (void)m.memory().write_bytes(pos, buf, chunk);
                        pos += chunk;
                        rem -= chunk;
                    }
                }
            }
            m.set_return(0);
        },
        12
    });

    // ------------------------------------------------------------------
    // RTL compression API
    //
    // NTSTATUS values (subset):
    //   STATUS_SUCCESS                = 0x00000000
    //   STATUS_INVALID_PARAMETER      = 0xC000000D
    //   STATUS_UNSUPPORTED_COMPRESSION = 0xC000025F
    //
    // COMPRESSION_FORMAT_NONE   = 0
    // COMPRESSION_FORMAT_LZNT1  = 2
    // COMPRESSION_FORMAT_XPRESS = 3
    // COMPRESSION_FORMAT_XPRESS_HUFF = 4
    //
    // PE loaders and .NET runtimes resolve these at startup; missing
    // exports turn into hard IAT failures.  We register the full surface,
    // implement COMPRESSION_FORMAT_NONE (raw memcpy) for completeness,
    // and return STATUS_UNSUPPORTED_COMPRESSION for the LZ-based formats
    // until a caller is observed to need them.
    // TODO(rtl-compress): implement an LZNT1 decoder when a caller hits it.
    // ------------------------------------------------------------------

    auto rtl_decompress_impl = [](Marshaler& m, bool /*has_workspace*/) {
        u16 format = static_cast<u16>(m.arg(0) & 0x000F);
        (void)m.arg_ptr(1); (void)m.arg(2); (void)m.arg_ptr(3); (void)m.arg(4); (void)m.arg_ptr(5);

        // Format 0 (NONE) is invalid for decompress; only LZNT1/XPRESS are valid.
        if (format == 0) {
            m.set_return(static_cast<u64>(0xC000000Du)); // STATUS_INVALID_PARAMETER
            return;
        }

        // STATUS_UNSUPPORTED_COMPRESSION until LZNT1/XPRESS decoders are implemented.
        m.set_return(static_cast<u64>(0xC000025Fu));
    };

    // RtlGetCompressionWorkSpaceSize (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "RtlGetCompressionWorkSpaceSize", ordinal++,
        [](Marshaler& m) {
            u16 format = static_cast<u16>(m.arg(0) & 0x000F);
            GuestAddr ws_size_ptr = m.arg_ptr(1);
            GuestAddr frag_ws_ptr = m.arg_ptr(2);

            u32 status;
            if (format == 0 /*NONE*/) {
                status = 0xC000000Du; // STATUS_INVALID_PARAMETER
            } else if (format == 2 /*LZNT1*/ || format == 3 /*XPRESS*/ ||
                       format == 4 /*XPRESS_HUFF*/) {
                constexpr u32 WORKSPACE_SIZE = 0x10000;
                if (ws_size_ptr != 0)  (void)m.write_u32(ws_size_ptr, WORKSPACE_SIZE);
                if (frag_ws_ptr != 0)  (void)m.write_u32(frag_ws_ptr, WORKSPACE_SIZE);
                status = 0;
            } else {
                status = 0xC000025Fu; // STATUS_UNSUPPORTED_COMPRESSION
            }
            m.set_return(static_cast<u64>(status));
        },
        12
    });

    // RtlDecompressBuffer (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "RtlDecompressBuffer", ordinal++,
        [rtl_decompress_impl](Marshaler& m) { rtl_decompress_impl(m, false); },
        24
    });

    // RtlDecompressBufferEx (7 args, stdcall_bytes=28)
    exports.entries.push_back(ExportEntry{
        "RtlDecompressBufferEx", ordinal++,
        [rtl_decompress_impl](Marshaler& m) { rtl_decompress_impl(m, true); },
        28
    });

    // RtlCompressBuffer (8 args, stdcall_bytes=32)
    exports.entries.push_back(ExportEntry{
        "RtlCompressBuffer", ordinal++,
        [](Marshaler& m) {
            u16 format = static_cast<u16>(m.arg(0) & 0x000F);
            GuestAddr uncomp_buf = m.arg_ptr(1);
            u32 uncomp_size = static_cast<u32>(m.arg(2));
            GuestAddr comp_buf = m.arg_ptr(3);
            u32 comp_size = static_cast<u32>(m.arg(4));
            // arg(5) = UncompressedChunkSize (unused for NONE)
            GuestAddr final_comp_ptr = m.arg_ptr(6);
            // arg(7) = workspace (unused)

            // Format 0 (NONE) is invalid for compress; only LZNT1/XPRESS are valid.
            if (format == 0) {
                m.set_return(static_cast<u64>(0xC000000Du)); // STATUS_INVALID_PARAMETER
                return;
            }

            u32 status = 0xC000025Fu;
            u32 bytes_written = 0;

            if (false /*format == 0 disabled above*/) {
                u32 copy_len = std::min(uncomp_size, comp_size);
                if (copy_len > 0 && uncomp_buf != 0 && comp_buf != 0) {
                    const Byte* src = m.memory().host_ptr(uncomp_buf, copy_len);
                    Byte* dst = m.memory().host_ptr_mut(comp_buf, copy_len);
                    if (src && dst) {
                        std::memcpy(dst, src, copy_len);
                    } else {
                        std::vector<Byte> buf(copy_len);
                        (void)m.memory().read_bytes(uncomp_buf, buf.data(), copy_len);
                        (void)m.memory().write_bytes(comp_buf, buf.data(), copy_len);
                    }
                }
                bytes_written = copy_len;
                status = 0;
            }

            if (final_comp_ptr != 0) {
                (void)m.write_u32(final_comp_ptr, bytes_written);
            }
            m.set_return(static_cast<u64>(status));
        },
        32
    });
}

} // namespace vitreo::winapi
