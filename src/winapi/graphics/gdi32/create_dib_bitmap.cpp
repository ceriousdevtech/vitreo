// SPDX-License-Identifier: LGPL-2.1-only
// Windows contract: Learn CreateDIBitmap; bitmap_create_probe{32,64}.expected.
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_internal.hpp"
#include "winapi/graphics/gdi32/bitmap_codec.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <limits>
#include <mutex>

namespace vitreo::winapi {
#include "create_dib_bitmap_contract.inc"

Handle GdiManager::create_dib_bitmap(Handle hdc, GuestAddr header, u32 flags,
    GuestAddr bits, GuestAddr info, u32 usage, u32* error) {
    namespace facts=create_dib_bitmap_contract;
    ApiLock lock(*this);
    if(error)*error=0;
    const auto fail=[&](u32 code) {if(error)*error=code;return Handle{0};};
    const auto unavailable=[&](std::once_flag& warning,const char* reason) {
        std::call_once(warning,[reason,flags,usage] {std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED gdi32!CreateDIBitmap: %s (flags=0x%x usage=%u)\n",reason,flags,usage);});
        return Handle{0};
    };
    const bool copy=(flags&facts::CBM_INIT)!=0 && bits!=0;
    // The Windows null-parameter matrix establishes this check's precedence.
    if(copy && info==0)return 0;
    if(header==0 || usage>facts::observed_max_usage)return fail(facts::ERROR_INVALID_PARAMETER);
    if(hdc!=0 && !m_dc_states.contains(hdc))return 0;
    if(!m_guest_memory) {
        static std::once_flag warning;
        return unavailable(warning,"guest memory is unavailable");
    }
    const auto size=m_guest_memory->read_u32(header);
    if(size.is_err()) {
        static std::once_flag warning;
        return unavailable(warning,"unreadable bitmap header");
    }
    const auto stock=[&] {return create_bitmap(0,0,facts::compatible_planes,
        facts::null_reference_bpp,0);};
    if(std::find(facts::legacy_fallback_sizes.begin(),facts::legacy_fallback_sizes.end(),size.value())!=facts::legacy_fallback_sizes.end())
        return stock();
    i32 width=0,height=0;
    u32 header_compression=facts::BI_RGB;
    if(size.value()==facts::SIZEOF_BITMAPCOREHEADER) {
        const auto w=m_guest_memory->read_u16(header+facts::OFFSETOF_BITMAPCOREHEADER_BCWIDTH);
        const auto h=m_guest_memory->read_u16(header+facts::OFFSETOF_BITMAPCOREHEADER_BCHEIGHT);
        if(w.is_err()||h.is_err()) {
            static std::once_flag warning;
            return unavailable(warning,"unreadable core bitmap dimensions");
        }
        width=w.value();height=h.value();
    } else if(size.value()>=facts::SIZEOF_BITMAPINFOHEADER) {
        const auto w=m_guest_memory->read_u32(header+facts::OFFSETOF_BITMAPINFOHEADER_BIWIDTH);
        const auto h=m_guest_memory->read_u32(header+facts::OFFSETOF_BITMAPINFOHEADER_BIHEIGHT);
        const auto compression=m_guest_memory->read_u32(header+facts::OFFSETOF_BITMAPINFOHEADER_BICOMPRESSION);
        if(w.is_err()||h.is_err()||compression.is_err()) {
            static std::once_flag warning;
            return unavailable(warning,"unreadable bitmap dimensions/compression");
        }
        width=std::bit_cast<i32>(w.value());height=std::bit_cast<i32>(h.value());
        header_compression=compression.value();
    } else {
        static std::once_flag warning;
        return unavailable(warning,"unmeasured legacy bitmap header size");
    }
    if(width==0||height==0)return stock();
    if(width<0)return fail(copy?0:facts::ERROR_INVALID_PARAMETER);
    const i64 absolute_height=height<0?-static_cast<i64>(height):height;
    if(absolute_height>std::numeric_limits<i32>::max()) {
        static std::once_flag warning;
        return unavailable(warning,"bitmap height exceeds representable surface size");
    }
    height=static_cast<i32>(absolute_height);
    if(!copy && (header_compression==facts::BI_JPEG||header_compression==facts::BI_PNG)) {
        static std::once_flag warning;
        return unavailable(warning,"JPEG/PNG bitmap creation");
    }
    u32 depth=facts::null_reference_bpp;
    if(hdc!=0) {
        const auto& dc=m_dc_states.at(hdc);
        depth=dc.memory_dc&&dc.bitmap?dc.bitmap->bpp:static_cast<u32>(get_device_caps(hdc,BITSPIXEL));
    }
    if(copy) {
        if(usage>facts::DIB_PAL_COLORS) {
            static std::once_flag warning;
            return unavailable(warning,"palette-index-only source usage");
        }
        if(usage==facts::DIB_PAL_COLORS && hdc==0) {
            static std::once_flag warning;
            return unavailable(warning,"logical palette source without a reference DC");
        }
        gdi::DibHeader source;
        if(!gdi::read_dib_header(*m_guest_memory,info,source)) {
            static std::once_flag warning;
            return unavailable(warning,"unreadable or unsupported source DIB header");
        }
        if(source.header_size!=facts::SIZEOF_BITMAPCOREHEADER &&
            source.header_size!=facts::SIZEOF_BITMAPINFOHEADER &&
            source.header_size!=facts::SIZEOF_BITMAPV4HEADER &&
            source.header_size!=facts::SIZEOF_BITMAPV5HEADER) {
            static std::once_flag warning;
            return unavailable(warning,"extended source DIB header size");
        }
        if(source.planes==0)return fail(facts::ERROR_INVALID_PARAMETER);
        if(source.compression!=facts::BI_RGB && source.compression!=facts::BI_BITFIELDS) {
            static std::once_flag warning;
            return unavailable(warning,"compressed RLE/JPEG/PNG or unknown source format");
        }
        if(!gdi::dib_is_decodable(source)||source.width<=0)return 0;
        if(source.compression==facts::BI_BITFIELDS) {
            u32 combined=0;
            for(const u32 mask:source.masks) {
                if(mask==0) {
                    static std::once_flag warning;
                    return unavailable(warning,"zero-channel bitfield masks");
                }
                if((combined&mask)!=0 || (source.bpp<std::numeric_limits<u32>::digits &&
                    (mask>>source.bpp)!=0)) {
                    static std::once_flag warning;
                    return unavailable(warning,"overlapping or out-of-depth bitfield masks");
                }
                const u32 channel=mask>>std::countr_zero(mask);
                if((channel&(channel+1u))!=0) {
                    static std::once_flag warning;
                    return unavailable(warning,"noncontiguous bitfield masks");
                }
                combined|=mask;
            }
        }
        const i64 source_height=source.height<0?-static_cast<i64>(source.height):source.height;
        if(source_height<height)return fail(facts::ERROR_INVALID_PARAMETER);
        if(source_height!=height) {
            static std::once_flag warning;
            return unavailable(warning,"source taller than destination");
        }
        constexpr u32 alignment_bits=sizeof(u32)*std::numeric_limits<u8>::digits;
        if(static_cast<u64>(source.width)*source.bpp>
                std::numeric_limits<u32>::max()-(alignment_bits-1u) ||
            static_cast<u64>(source.width)*sizeof(u32)>std::numeric_limits<i32>::max()) {
            static std::once_flag warning;
            return unavailable(warning,"source scan line exceeds codec resource bounds");
        }
        // The codec documents that its 8bpp inverse quantization does not
        // reproduce all Windows colours. Refuse that conversion honestly.
        if(depth==facts::indexed_destination_bpp) {
            static std::once_flag warning;
            return unavailable(warning,"8bpp destination colour quantization");
        }
        gdi::PixelFormat format;
        if(!gdi::dib_pixel_format(*m_guest_memory,source,usage,dc_logical_palette(hdc),format)) {
            static std::once_flag warning;
            return unavailable(warning,"source palette or bitfield mapping");
        }
    }
    u32 allocation_error=0;
    // Always allocate a DDB. CreateCompatibleBitmap would return a DIB
    // section when one is selected into the reference DC, which Windows'
    // CreateDIBitmap explicitly does not do (reference_* oracle cases).
    const Handle result=create_bitmap(width,height,facts::compatible_planes,depth,0,&allocation_error);
    if(result==0)return fail(allocation_error);
    if(copy && set_dib_bits(hdc,result,0,static_cast<u32>(height),bits,info,usage)!=height) {
        (void)delete_object(result);
        return 0;
    }
    return result;
}
} // namespace vitreo::winapi
