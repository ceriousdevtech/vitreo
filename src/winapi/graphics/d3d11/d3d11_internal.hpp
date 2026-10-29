#pragma once
/// @file d3d11_internal.hpp
/// @brief Shared surface between d3d11.cpp and the interfaces split out of it.
///
/// Private to the D3D11 frontend. This was one anonymous namespace at the top
/// of d3d11.cpp before the per-interface dispatchers moved out; it is here so
/// they can still see it, not to publish it.
///
/// Almost all of it is the numbers the D3D11 headers would have given a real
/// Windows build — bind flags, usages, CPU access flags, format-support bits,
/// query types, and the interface IIDs. Vitreo has no d3d11.h, so these are
/// transcribed, and every one of them is an ABI claim a guest can check.
///
/// The whole block was promoted in one step rather than symbol by symbol. Doing
/// it piecemeal on advapi32 cost a build round per discovery; the question worth
/// asking first is whether the entire anonymous namespace can move, and here it
/// could — 296 lines, no collisions, and every consumer already inside this DLL.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>
#include <unistd.h>
#include <mutex>
#include <set>
#include <string_view>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace vitreo::winapi {


inline constexpr usize STORAGE_SIZE = 0x40000;

/// A {FirstArraySlice, ArraySize} window over @p slices array slices, as a
/// Texture1DArray UAV names one. Measured (d3d11_uav_1d_array_probe): an
/// ArraySize of -1 means every slice from FirstArraySlice on, 0 is refused, and
/// so is any window that starts or ends past the last slice.
[[nodiscard]] inline bool uav_array_window_valid(u32 slices, u32 first,
                                                 u32 size) {
    if (first >= slices) return false;
    return size == 0xFFFFFFFFu || (size != 0 && size <= slices - first);
}

/// VITREO_D3D11_CENSUS: count every D3D11 method call and print the histogram
/// at exit. Diagnostic only; it answers "which API surface does this title
/// actually use", which failure-message surveys cannot.
[[nodiscard]] inline bool d3d11_census_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D11_CENSUS");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/// Read a 64-bit argument, and report the index of the argument after it.
///
/// A UINT64 parameter occupies ONE argument slot on x64 and TWO on x86, so every
/// parameter declared after one shifts by a slot between the two bitnesses.
/// Indexing them by hand reads a pointer out of the high half of a value on one
/// bitness only — a defect that passes every test run on the other, which is why
/// this is a helper and not an inline expression at each call site.
///
/// D3D11 has three such methods: ID3D11Device5::CreateFence,
/// ID3D11Fence::SetEventOnCompletion, and ID3D11DeviceContext4::Signal / Wait.
[[nodiscard]] inline u64 read_arg_u64(const Marshaler& m, bool is_64bit,
                                     usize index, usize& next_index) {
    if (is_64bit) {
        next_index = index + 1;
        return m.arg(index);
    }
    next_index = index + 2;
    return static_cast<u64>(m.arg32(index)) |
           (static_cast<u64>(m.arg32(index + 1)) << 32);
}

using GuidBytes = std::array<u8, 16>;

inline constexpr GuidBytes IID_IUNKNOWN{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
inline constexpr GuidBytes IID_ID3D11DEVICE{
    0xDB, 0x6D, 0x6F, 0xDB, 0x77, 0xAC, 0x88, 0x4E,
    0x82, 0x53, 0x81, 0x9D, 0xF9, 0xBB, 0xF1, 0x40};
inline constexpr GuidBytes IID_ID3D11DEVICE1{
    0x29, 0xFB, 0x4B, 0xA0, 0xEF, 0x08, 0xD6, 0x43,
    0xA4, 0x9C, 0xA9, 0xBD, 0xBD, 0xCB, 0xE6, 0x86};
inline constexpr GuidBytes IID_ID3D11DEVICE2{
    0xFA, 0xDF, 0x06, 0x9D, 0xE5, 0xD1, 0x07, 0x4D,
    0x83, 0xA8, 0x1B, 0xB1, 0x23, 0xF2, 0xF8, 0x41};
inline constexpr GuidBytes IID_ID3D11DEVICE3{
    0x37, 0x8C, 0x5C, 0xA0, 0xC6, 0xD2, 0x32, 0x47,
    0xB3, 0xA0, 0x9C, 0xE0, 0xB0, 0xDC, 0x9A, 0xE6};
inline constexpr GuidBytes IID_ID3D11DEVICE4{
    0x71, 0xAB, 0x92, 0x89, 0xE6, 0x02, 0x8D, 0x4B,
    0xBA, 0x48, 0xB0, 0x56, 0xDC, 0xDA, 0x42, 0xC4};
inline constexpr GuidBytes IID_ID3D11DEVICE5{
    0x02, 0xE2, 0xFD, 0x8F, 0xE7, 0xA0, 0xDF, 0x45,
    0x9E, 0x01, 0xE8, 0x37, 0x80, 0x1B, 0x5E, 0xA0};
inline constexpr GuidBytes IID_ID3D11DEVICECONTEXT{
    0x6C, 0xA9, 0xBF, 0xC0, 0x89, 0xE0, 0xFB, 0x44,
    0x8E, 0xAF, 0x26, 0xF8, 0x79, 0x61, 0x90, 0xDA};
inline constexpr GuidBytes IID_ID3D11DEVICECONTEXT1{
    0xAA, 0x6F, 0x2C, 0xBB, 0xFB, 0xB5, 0x82, 0x40,
    0x8E, 0x6B, 0x38, 0x8B, 0x8C, 0xFA, 0x90, 0xE1};
inline constexpr GuidBytes IID_ID3D11DEVICECONTEXT2{
    0x32, 0x5B, 0x0D, 0x42, 0x0C, 0xB9, 0xA4, 0x4D,
    0xBE, 0xF0, 0x35, 0x9F, 0x6A, 0x24, 0xA8, 0x3A};
inline constexpr GuidBytes IID_ID3D11DEVICECONTEXT3{
    0x1D, 0xC0, 0xE3, 0xB4, 0x9E, 0xE7, 0x37, 0x46,
    0x91, 0xB2, 0x51, 0x0E, 0x9F, 0x4C, 0x9B, 0x8F};
inline constexpr GuidBytes IID_ID3D11DEVICECONTEXT4{
    0xDA, 0x00, 0x76, 0x91, 0x8C, 0xF5, 0x33, 0x4C,
    0x98, 0xD8, 0x3E, 0x15, 0xB3, 0x90, 0xFA, 0x24};
inline constexpr GuidBytes IID_ID3D11DEVICECHILD{
    0xC8, 0xE5, 0x41, 0x18, 0xB0, 0x16, 0x9B, 0x48,
    0xBC, 0xC8, 0x44, 0xCF, 0xB0, 0xD5, 0xDE, 0xAE};
/// {61f21c45-3c0e-4a74-9cea-67100d9ad5e4} (d3d11.h ID3D11VideoContext).
inline constexpr GuidBytes IID_ID3D11VIDEOCONTEXT{
    0x45, 0x1C, 0xF2, 0x61, 0x0E, 0x3C, 0x74, 0x4A,
    0x9C, 0xEA, 0x67, 0x10, 0x0D, 0x9A, 0xD5, 0xE4};
/// {a7f026da-a5f8-4487-a564-15e34357651e} (d3d11_1.h ID3D11VideoContext1).
inline constexpr GuidBytes IID_ID3D11VIDEOCONTEXT1{
    0xDA, 0x26, 0xF0, 0xA7, 0xF8, 0xA5, 0x87, 0x44,
    0xA5, 0x64, 0x15, 0xE3, 0x43, 0x57, 0x65, 0x1E};
/// {c4e7374c-6243-4d1b-ae87-52b4f740e261} (d3d11_4.h ID3D11VideoContext2).
inline constexpr GuidBytes IID_ID3D11VIDEOCONTEXT2{
    0x4C, 0x37, 0xE7, 0xC4, 0x43, 0x62, 0x1B, 0x4D,
    0xAE, 0x87, 0x52, 0xB4, 0xF7, 0x40, 0xE2, 0x61};
/// {a9e2faa0-cb39-418f-a0b7-d8aad4de672e} (ID3D11VideoContext3: Windows SDK
/// 10.0.26100 d3d11_4.h; the mingw-w64 headers stop at VideoContext2).
inline constexpr GuidBytes IID_ID3D11VIDEOCONTEXT3{
    0xA0, 0xFA, 0xE2, 0xA9, 0x39, 0xCB, 0x8F, 0x41,
    0xA0, 0xB7, 0xD8, 0xAA, 0xD4, 0xDE, 0x67, 0x2E};
inline constexpr GuidBytes IID_ID3D11RESOURCE{
    0xF3, 0x63, 0x8E, 0xDC, 0x2B, 0xD1, 0x52, 0x49,
    0xB4, 0x7B, 0x5E, 0x45, 0x02, 0x6A, 0x86, 0x2D};
inline constexpr GuidBytes IID_ID3D11BUFFER{
    0x85, 0x0B, 0x57, 0x48, 0xEE, 0xD1, 0xCD, 0x4F,
    0xA2, 0x50, 0xEB, 0x35, 0x07, 0x22, 0xB0, 0x37};
/// {f8fb5c27-c6b3-4f75-a4c8-439af2ef564c} (d3d11.h ID3D11Texture1D).
inline constexpr GuidBytes IID_ID3D11TEXTURE1D{
    0x27, 0x5C, 0xFB, 0xF8, 0xB3, 0xC6, 0x75, 0x4F,
    0xA4, 0xC8, 0x43, 0x9A, 0xF2, 0xEF, 0x56, 0x4C};
inline constexpr GuidBytes IID_ID3D11TEXTURE2D{
    0xF2, 0xAA, 0x15, 0x6F, 0x08, 0xD2, 0x89, 0x4E,
    0x9A, 0xB4, 0x48, 0x95, 0x35, 0xD3, 0x4F, 0x9C};
inline constexpr GuidBytes IID_ID3D11TEXTURE3D{
    0x6E, 0x86, 0x7E, 0x03, 0x6D, 0xF5, 0x57, 0x43,
    0xA8, 0xAF, 0x9D, 0xAB, 0xBE, 0x6E, 0x25, 0x0E};
inline constexpr GuidBytes IID_ID3D11VIEW{
    0x16, 0x12, 0x9D, 0x83, 0x2E, 0xBB, 0x2B, 0x41,
    0xB7, 0xF4, 0xA9, 0xDB, 0xEB, 0xE0, 0x8E, 0xD1};
inline constexpr GuidBytes IID_ID3D11SHADERRESOURCEVIEW{
    0xE0, 0x6F, 0xE0, 0xB0, 0x92, 0x81, 0x1A, 0x4E,
    0xB1, 0xCA, 0x36, 0xD7, 0x41, 0x47, 0x10, 0xB2};
inline constexpr GuidBytes IID_ID3D11UNORDEREDACCESSVIEW{
    0x09, 0xF5, 0xAC, 0x28, 0x5C, 0x7F, 0xF6, 0x48,
    0x86, 0x11, 0xF3, 0x16, 0x01, 0x0A, 0x63, 0x80};
/// The `*1` object interfaces (d3d11_3.h). Each adds only GetDesc1.
inline constexpr GuidBytes IID_ID3D11TEXTURE2D1{
    0x51, 0x82, 0x21, 0x51, 0x33, 0x1E, 0x17, 0x46,
    0x9C, 0xCB, 0x4D, 0x3A, 0x43, 0x67, 0xE7, 0xBB};
inline constexpr GuidBytes IID_ID3D11TEXTURE3D1{
    0x83, 0x16, 0x71, 0x0C, 0x53, 0x28, 0x46, 0x48,
    0x9B, 0xB0, 0xF3, 0xE6, 0x06, 0x39, 0xE4, 0x6A};
inline constexpr GuidBytes IID_ID3D11SHADERRESOURCEVIEW1{
    0x87, 0x8B, 0x30, 0x91, 0x40, 0x90, 0x1D, 0x41,
    0x8C, 0x67, 0xC3, 0x92, 0x53, 0xCE, 0x38, 0x02};
inline constexpr GuidBytes IID_ID3D11RENDERTARGETVIEW1{
    0x23, 0x2E, 0xBE, 0xFF, 0x11, 0xF0, 0x8A, 0x41,
    0xAC, 0x56, 0x5C, 0xEE, 0xD7, 0xC5, 0xB9, 0x4B};
inline constexpr GuidBytes IID_ID3D11UNORDEREDACCESSVIEW1{
    0x53, 0x61, 0x3B, 0x7B, 0x86, 0xA8, 0x44, 0x45,
    0xAB, 0x37, 0x65, 0x37, 0xC8, 0x50, 0x04, 0x03};
inline constexpr GuidBytes IID_ID3D11QUERY1{
    0x66, 0x47, 0x1B, 0x63, 0xDC, 0x36, 0x1D, 0x46,
    0x8D, 0xB6, 0xC4, 0x7E, 0x13, 0xE6, 0x09, 0x16};

inline constexpr GuidBytes IID_ID3D11RENDERTARGETVIEW{
    0x67, 0xA0, 0xDB, 0xDF, 0x8D, 0x0B, 0x65, 0x48,
    0x87, 0x5B, 0xD7, 0xB4, 0x51, 0x6C, 0xC1, 0x64};
inline constexpr GuidBytes IID_ID3D11DEPTHSTENCILVIEW{
    0x2A, 0xC9, 0xDA, 0x9F, 0x76, 0x18, 0xC3, 0x48,
    0xAF, 0xAD, 0x25, 0xB9, 0x4F, 0x84, 0xA9, 0xB6};
inline constexpr GuidBytes IID_ID3D11ASYNCHRONOUS{
    0xCD, 0xD0, 0x35, 0x4B, 0x15, 0x1E, 0x58, 0x42,
    0x9C, 0x98, 0x1B, 0x13, 0x33, 0xF6, 0xDD, 0x3B};
inline constexpr GuidBytes IID_ID3D11QUERY{
    0x47, 0x07, 0xC0, 0xD6, 0xB7, 0x87, 0x5E, 0x42,
    0xB8, 0x4D, 0x44, 0xD1, 0x08, 0x56, 0x0A, 0xFD};
/// ID3D11CommandList {a24bc4d1-769e-43f7-8013-98ff566c18e2}
inline constexpr GuidBytes IID_ID3D11COMMANDLIST{
    0xD1, 0xC4, 0x4B, 0xA2, 0x9E, 0x76, 0xF7, 0x43,
    0x80, 0x13, 0x98, 0xFF, 0x56, 0x6C, 0x18, 0xE2};
/// ID3D11Fence {affde9d1-1df7-4bb7-8a34-0f46251dab80}
inline constexpr GuidBytes IID_ID3D11FENCE{
    0xD1, 0xE9, 0xFD, 0xAF, 0xF7, 0x1D, 0xB7, 0x4B,
    0x8A, 0x34, 0x0F, 0x46, 0x25, 0x1D, 0xAB, 0x80};
/// ID3D11Multithread {9b7e4e00-342c-4106-a19f-4f2704f689f0}. The SDK gives
/// ID3D10Multithread the very same IID (d3d10.h and d3d11_4.h), which is why
/// Windows answers both with one pointer.
inline constexpr GuidBytes IID_ID3D11MULTITHREAD{
    0x00, 0x4E, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
inline constexpr GuidBytes IID_ID3D10MULTITHREAD{
    0x00, 0x4E, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D11ClassLinkage {ddf57cba-9543-46e4-a12b-f207a0fe7fed}
inline constexpr GuidBytes IID_ID3D11CLASSLINKAGE{
    0xBA, 0x7C, 0xF5, 0xDD, 0x43, 0x95, 0xE4, 0x46,
    0xA1, 0x2B, 0xF2, 0x07, 0xA0, 0xFE, 0x7F, 0xED};
/// ID3D11ClassInstance {a6cd7faa-b0b7-4a2f-9436-8662a65797cb}
inline constexpr GuidBytes IID_ID3D11CLASSINSTANCE{
    0xAA, 0x7F, 0xCD, 0xA6, 0xB7, 0xB0, 0x2F, 0x4A,
    0x94, 0x36, 0x86, 0x62, 0xA6, 0x57, 0x97, 0xCB};
inline constexpr GuidBytes IID_ID3DUSERDEFINEDANNOTATION{
    0x8B, 0xAD, 0xDA, 0xB2, 0xD4, 0x03, 0xBF, 0x4D,
    0x95, 0xEB, 0x32, 0xAB, 0x4B, 0x63, 0xD0, 0xAB};
inline constexpr GuidBytes IID_IDXGIDEVICE{
    0xFA, 0x77, 0xEC, 0x54, 0x77, 0x13, 0xE6, 0x44,
    0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C};
inline constexpr GuidBytes IID_IDXGIDEVICE1{
    0x0F, 0x97, 0xDB, 0x77, 0x76, 0x62, 0xBA, 0x48,
    0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C};
inline constexpr GuidBytes IID_IDXGIDEVICE2{
    0x17, 0x86, 0x00, 0x05, 0xFD, 0xFB, 0x51, 0x40,
    0xA7, 0x90, 0x14, 0x48, 0x84, 0xB4, 0xF6, 0xA9};
inline constexpr GuidBytes IID_IDXGIOBJECT{
    0xB8, 0x2F, 0xC2, 0xAE, 0xF3, 0x76, 0x39, 0x46,
    0x9B, 0xE0, 0x28, 0xEB, 0x43, 0xA6, 0x7A, 0x2E};
inline constexpr GuidBytes IID_IDXGIDEVICESUBOBJECT{
    0x79, 0x03, 0x3E, 0x3D, 0xDE, 0xF9, 0x58, 0x4D,
    0xBB, 0x6C, 0x18, 0xD6, 0x29, 0x92, 0xF1, 0xA6};
inline constexpr GuidBytes IID_IDXGIRESOURCE{
    0xB4, 0x3A, 0x5F, 0x03, 0x2E, 0x48, 0x50, 0x4E,
    0xB4, 0x1F, 0x8A, 0x7F, 0x8B, 0xD8, 0x96, 0x0B};
// IDXGIResource1 {30961379-4609-4a41-998e-54fe567ee0c1} (dxgi1_2.h;
// tools/audit_guid_constants.py checks every constant like this one).
inline constexpr GuidBytes IID_IDXGIRESOURCE1{
    0x79, 0x13, 0x96, 0x30, 0x09, 0x46, 0x41, 0x4A,
    0x99, 0x8E, 0x54, 0xFE, 0x56, 0x7E, 0xE0, 0xC1};
/// IDXGISurface* (a 2D texture's surface facet), ID3DDeviceContextState, and
/// the D3D10 device IIDs CreateDeviceContextState may name.
inline constexpr GuidBytes IID_IDXGISURFACE{
    0x6C, 0xB5, 0xFC, 0xCA, 0xC3, 0x6A, 0x89, 0x48,
    0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC};
inline constexpr GuidBytes IID_IDXGISURFACE1{
    0x92, 0x30, 0xE6, 0x4A, 0x27, 0x63, 0x1B, 0x4C,
    0x80, 0xAE, 0xBF, 0xE1, 0x2E, 0xA3, 0x2B, 0x86};
inline constexpr GuidBytes IID_IDXGISURFACE2{
    0xDD, 0x96, 0xA4, 0xAB, 0x17, 0xB6, 0xB8, 0x4C,
    0xA8, 0x66, 0xBC, 0x44, 0xD7, 0xEB, 0x1F, 0xA2};
inline constexpr GuidBytes IID_ID3DDEVICECONTEXTSTATE{
    0x8A, 0x0D, 0x1E, 0x5C, 0x23, 0x7C, 0xF9, 0x48,
    0x8C, 0x59, 0xA9, 0x29, 0x58, 0xCE, 0xFF, 0x11};
inline constexpr GuidBytes IID_ID3D10DEVICE{
    0x0F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
inline constexpr GuidBytes IID_ID3D10DEVICE1{
    0x8F, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// The D3D10 interfaces a D3D11 object also answers (the D3D10 facets,
/// winapi/graphics/d3d10/d3d10_device.cpp): d3d10.h, d3d10_1.h, d3dcommon.h
/// and d3d10effect.h, checked by tools/audit_guid_constants.py.
/// ID3D10DeviceChild {9b7e4c00-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10DEVICECHILD{
    0x00, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Resource {9b7e4c01-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10RESOURCE{
    0x01, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Buffer {9b7e4c02-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10BUFFER{
    0x02, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Texture1D {9b7e4c03-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10TEXTURE1D{
    0x03, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Texture2D {9b7e4c04-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10TEXTURE2D{
    0x04, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Texture3D {9b7e4c05-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10TEXTURE3D{
    0x05, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10View {c902b03f-60a7-49ba-9936-2a3ab37a7e33}
inline constexpr GuidBytes IID_ID3D10VIEW{
    0x3F, 0xB0, 0x02, 0xC9, 0xA7, 0x60, 0xBA, 0x49,
    0x99, 0x36, 0x2A, 0x3A, 0xB3, 0x7A, 0x7E, 0x33};
/// ID3D10ShaderResourceView {9b7e4c07-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10SHADERRESOURCEVIEW{
    0x07, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10ShaderResourceView1 {9b7e4c87-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10SHADERRESOURCEVIEW1{
    0x87, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10RenderTargetView {9b7e4c08-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10RENDERTARGETVIEW{
    0x08, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10DepthStencilView {9b7e4c09-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10DEPTHSTENCILVIEW{
    0x09, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10BlendState {edad8d19-8a35-4d6d-8566-2ea276cde161}
inline constexpr GuidBytes IID_ID3D10BLENDSTATE{
    0x19, 0x8D, 0xAD, 0xED, 0x35, 0x8A, 0x6D, 0x4D,
    0x85, 0x66, 0x2E, 0xA2, 0x76, 0xCD, 0xE1, 0x61};
/// ID3D10BlendState1 {edad8d99-8a35-4d6d-8566-2ea276cde161}
inline constexpr GuidBytes IID_ID3D10BLENDSTATE1{
    0x99, 0x8D, 0xAD, 0xED, 0x35, 0x8A, 0x6D, 0x4D,
    0x85, 0x66, 0x2E, 0xA2, 0x76, 0xCD, 0xE1, 0x61};
/// ID3D10DepthStencilState {2b4b1cc8-a4ad-41f8-8322-ca86fc3ec675}
inline constexpr GuidBytes IID_ID3D10DEPTHSTENCILSTATE{
    0xC8, 0x1C, 0x4B, 0x2B, 0xAD, 0xA4, 0xF8, 0x41,
    0x83, 0x22, 0xCA, 0x86, 0xFC, 0x3E, 0xC6, 0x75};
/// ID3D10RasterizerState {a2a07292-89af-4345-be2e-c53d9fbb6e9f}
inline constexpr GuidBytes IID_ID3D10RASTERIZERSTATE{
    0x92, 0x72, 0xA0, 0xA2, 0xAF, 0x89, 0x45, 0x43,
    0xBE, 0x2E, 0xC5, 0x3D, 0x9F, 0xBB, 0x6E, 0x9F};
/// ID3D10SamplerState {9b7e4c0c-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10SAMPLERSTATE{
    0x0C, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10InputLayout {9b7e4c0b-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10INPUTLAYOUT{
    0x0B, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10VertexShader {9b7e4c0a-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10VERTEXSHADER{
    0x0A, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10GeometryShader {6316be88-54cd-4040-ab44-20461bc81f68}
inline constexpr GuidBytes IID_ID3D10GEOMETRYSHADER{
    0x88, 0xBE, 0x16, 0x63, 0xCD, 0x54, 0x40, 0x40,
    0xAB, 0x44, 0x20, 0x46, 0x1B, 0xC8, 0x1F, 0x68};
/// ID3D10PixelShader {4968b601-9d00-4cde-8346-8e7f675819b6}
inline constexpr GuidBytes IID_ID3D10PIXELSHADER{
    0x01, 0xB6, 0x68, 0x49, 0x00, 0x9D, 0xDE, 0x4C,
    0x83, 0x46, 0x8E, 0x7F, 0x67, 0x58, 0x19, 0xB6};
/// ID3D10Asynchronous {9b7e4c0d-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10ASYNCHRONOUS{
    0x0D, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Query {9b7e4c0e-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10QUERY{
    0x0E, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Predicate {9b7e4c10-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10PREDICATE{
    0x10, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Counter {9b7e4c11-342c-4106-a19f-4f2704f689f0}
inline constexpr GuidBytes IID_ID3D10COUNTER{
    0x11, 0x4C, 0x7E, 0x9B, 0x2C, 0x34, 0x06, 0x41,
    0xA1, 0x9F, 0x4F, 0x27, 0x04, 0xF6, 0x89, 0xF0};
/// ID3D10Blob {8ba5fb08-5195-40e2-ac58-0d989c3a0102}
inline constexpr GuidBytes IID_ID3D10BLOB{
    0x08, 0xFB, 0xA5, 0x8B, 0x95, 0x51, 0xE2, 0x40,
    0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02};
/// ID3D10StateBlock {0803425a-57f5-4dd6-9465-a87570834a08}
inline constexpr GuidBytes IID_ID3D10STATEBLOCK{
    0x5A, 0x42, 0x03, 0x08, 0xF5, 0x57, 0xD6, 0x4D,
    0x94, 0x65, 0xA8, 0x75, 0x70, 0x83, 0x4A, 0x08};

inline constexpr GuidBytes IID_IDXGIKEYEDMUTEX{
    0x89, 0x12, 0x8E, 0x9D, 0xB3, 0xD7, 0x5F, 0x46,
    0x81, 0x26, 0x25, 0x0E, 0x34, 0x9A, 0xF8, 0x5D};
inline constexpr GuidBytes IID_IDXGISWAPCHAIN{
    0xA0, 0x36, 0x0D, 0x31, 0xE7, 0xD2, 0x0A, 0x4C,
    0xAA, 0x04, 0x6A, 0x9D, 0x23, 0xB8, 0x88, 0x6A};
inline constexpr GuidBytes IID_IDXGISWAPCHAIN1{
    0xF7, 0x45, 0x0A, 0x79, 0x42, 0x0D, 0x76, 0x48,
    0x98, 0x3A, 0x0A, 0x55, 0xCF, 0xE6, 0xF4, 0xAA};
// dxgi1_3.h / dxgi1_4.h / dxgi1_5.h, checked by tools/audit_guid_constants.py.
inline constexpr GuidBytes IID_IDXGISWAPCHAIN2{
    0xC4, 0x2A, 0xBE, 0xA8, 0x9F, 0x19, 0x46, 0x49,
    0xB3, 0x31, 0x79, 0x59, 0x9F, 0xB9, 0x8D, 0xE7};
inline constexpr GuidBytes IID_IDXGISWAPCHAIN3{
    0xDB, 0x9B, 0xD9, 0x94, 0xF8, 0xF1, 0xB0, 0x4A,
    0xB2, 0x36, 0x7D, 0xA0, 0x17, 0x0E, 0xDA, 0xB1};
inline constexpr GuidBytes IID_IDXGISWAPCHAIN4{
    0x5A, 0x5D, 0x58, 0x3D, 0x4A, 0xBD, 0x9E, 0x48,
    0xB1, 0xF4, 0x3D, 0xBC, 0xB6, 0x45, 0x2F, 0xFB};
inline constexpr GuidBytes IID_IDXGISWAPCHAINMEDIA{
    0x0B, 0xB9, 0x95, 0xDD, 0x5F, 0xF0, 0x6A, 0x4F,
    0xBD, 0x65, 0x25, 0xBF, 0xB2, 0x64, 0xBD, 0x84};
inline constexpr GuidBytes IID_IDXGIDEVICE3{
    0x6C, 0x89, 0x07, 0x60, 0x44, 0x32, 0xFD, 0x4A,
    0xBF, 0x18, 0xA6, 0xD3, 0xBE, 0xDA, 0x50, 0x23};
inline constexpr GuidBytes IID_IDXGIDEVICE4{
    0x5F, 0xF9, 0xB4, 0x95, 0xDA, 0xD8, 0xA4, 0x4C,
    0x9E, 0xE6, 0x3B, 0x76, 0xD5, 0x96, 0x8A, 0x10};

inline constexpr u32 DXGI_ERROR_INVALID_CALL = 0x887A0001u;
inline constexpr u32 DXGI_USAGE_SHADER_INPUT = 0x10u;
inline constexpr u32 DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20u;
inline constexpr u32 DXGI_USAGE_BACK_BUFFER = 0x40u;
inline constexpr u32 D3D11_BIND_VERTEX_BUFFER = 0x1u;
inline constexpr u32 D3D11_BIND_INDEX_BUFFER = 0x2u;
inline constexpr u32 D3D11_BIND_CONSTANT_BUFFER = 0x4u;
inline constexpr u32 D3D11_BIND_SHADER_RESOURCE = 0x8u;
inline constexpr u32 D3D11_BIND_STREAM_OUTPUT = 0x10u;
inline constexpr u32 D3D11_BIND_RENDER_TARGET = 0x20u;
inline constexpr u32 D3D11_BIND_UNORDERED_ACCESS = 0x80u;
inline constexpr u32 D3D11_RESOURCE_MISC_GENERATE_MIPS = 0x1u;
inline constexpr u32 D3D11_RESOURCE_MISC_SHARED = 0x2u;
inline constexpr u32 D3D11_RESOURCE_MISC_TEXTURECUBE = 0x4u;
// Learn D3D11_RESOURCE_MISC_FLAG: the three buffer-shape flags. RAW_VIEWS
// enables a byte address buffer, STRUCTURED a structured buffer; the two
// describe different view shapes and are not combined on one buffer.
inline constexpr u32 D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS = 0x10u;
inline constexpr u32 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20u;
inline constexpr u32 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED = 0x40u;
inline constexpr u32 D3D11_RESOURCE_MISC_RESOURCE_CLAMP = 0x80u;
// Learn D3D11_RESOURCE_MISC_FLAG: SHARED_KEYEDMUTEX is 0x100, mutually
// exclusive with SHARED. 0x10 is DRAWINDIRECT_ARGS, not the mutex flag.
inline constexpr u32 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX = 0x100u;
// Learn: SHARED_NTHANDLE (0x800) pairs with SHARED or SHARED_KEYEDMUTEX and
// requires IDXGIResource1::CreateSharedHandle (GetSharedHandle fails).
inline constexpr u32 D3D11_RESOURCE_MISC_SHARED_NTHANDLE = 0x800u;
// Tiled resources (d3d11.h, d3d11_2.h; checked against the SDK by
// tools/audit_d3d11_query_constants.py).
inline constexpr u32 D3D11_RESOURCE_MISC_TILE_POOL = 0x20000u;
inline constexpr u32 D3D11_RESOURCE_MISC_TILED = 0x40000u;
inline constexpr u32 D3D11_2_TILED_RESOURCE_TILE_SIZE_IN_BYTES = 0x10000u;
inline constexpr u32 D3D11_PACKED_TILE = 0xFFFFFFFFu;
inline constexpr u32 D3D11_TILE_MAPPING_NO_OVERWRITE = 0x1u;
inline constexpr u32 D3D11_TILE_RANGE_NULL = 0x1u;
inline constexpr u32 D3D11_TILE_RANGE_SKIP = 0x2u;
inline constexpr u32 D3D11_TILE_RANGE_REUSE_SINGLE_TILE = 0x4u;
inline constexpr u32 D3D11_TILE_COPY_NO_OVERWRITE = 0x1u;
inline constexpr u32 D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE =
    0x2u;
inline constexpr u32 D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER =
    0x4u;
inline constexpr u32 D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE =
    0x1u;
inline constexpr u32 DXGI_SHARED_RESOURCE_READ = 0x80000000u;
inline constexpr u32 DXGI_SHARED_RESOURCE_WRITE = 0x1u;
inline constexpr u32 WAIT_TIMEOUT = 0x00000102u;
inline constexpr u32 INFINITE = 0xFFFFFFFFu;
inline constexpr u32 D3D11_BIND_DEPTH_STENCIL = 0x40u;
inline constexpr u32 D3D11_USAGE_DEFAULT = 0;
inline constexpr u32 D3D11_USAGE_IMMUTABLE = 1;
inline constexpr u32 D3D11_USAGE_DYNAMIC = 2;
inline constexpr u32 D3D11_USAGE_STAGING = 3;
inline constexpr u32 D3D11_CPU_ACCESS_WRITE = 0x10000u;
inline constexpr u32 D3D11_CPU_ACCESS_READ = 0x20000u;
inline constexpr u32 D3D11_MAP_READ = 1;
inline constexpr u32 D3D11_MAP_WRITE = 2;
inline constexpr u32 D3D11_MAP_READ_WRITE = 3;
inline constexpr u32 D3D11_MAP_WRITE_DISCARD = 4;
inline constexpr u32 D3D11_MAP_WRITE_NO_OVERWRITE = 5;
/// D3D11_MAP_FLAG (Learn): the only defined Map flag.
inline constexpr u32 D3D11_MAP_FLAG_DO_NOT_WAIT = 0x100000;
/// winerror.h DXGI_ERROR_WAS_STILL_DRAWING.
inline constexpr u32 kDxgiErrorWasStillDrawing = 0x887A000Au;
inline constexpr u32 D3D11_QUERY_TIMESTAMP = 2;
inline constexpr u32 D3D11_QUERY_TIMESTAMP_DISJOINT = 3;
// TIMESTAMP_DISJOINT Frequency: GPU timestamps are reported in nanoseconds
// of the host clock the backend stamps completed GPU work with.
inline constexpr u64 kTimestampFrequencyHz = 1000000000ull;

inline constexpr u32 D3D11_RESOURCE_DIMENSION_BUFFER = 1;
/// Bytes per pixel of a texture's **guest-visible staging layout**.
///
/// This is a D3D11 property, not a host one, and for packed depth+stencil the
/// two disagree. Measured on Windows 2026-09-22 with a 16-wide staging
/// surface: `D24_UNORM_S8_UINT` reports RowPitch 64 (four bytes per pixel, depth
/// and stencil packed together) and `D32_FLOAT_S8X24_UINT` reports 128 (eight).
/// The host maps both onto Metal's `Depth32Float_Stencil8`, whose separate
/// planes `format_table` reports as a five-byte footprint. Using the host number
/// for a staging row pitch would hand the guest a stride Windows never reports.
[[nodiscard]] inline u32 staging_bytes_per_pixel(u32 dxgi_format,
                                                u32 host_bytes_per_pixel) {
    switch (dxgi_format) {
    case 19:  // R32G8X24_TYPELESS
    case 20:  // D32_FLOAT_S8X24_UINT
    case 21:  // R32_FLOAT_X8X24_TYPELESS
    case 22:  // X32_TYPELESS_G8X24_UINT
        return 8;
    case 44:  // R24G8_TYPELESS
    case 45:  // D24_UNORM_S8_UINT
    case 46:  // R24_UNORM_X8_TYPELESS
    case 47:  // X24_TYPELESS_G8_UINT
        return 4;
    // The packed 4:2:2 video formats: a pixel is half of a host texel
    // (dxgi_pixels_per_texel). Measured (d3d11_video_formats_probe
    // [layout]): a 16-wide YUY2 maps RowPitch 32, Y210 / Y216 64.
    case 107:  // YUY2
        return 2;
    case 108:  // Y210
    case 109:  // Y216
        return 4;
    default:
        return host_bytes_per_pixel;
    }
}

/// DXGI_FORMAT_YUY2 / Y210 / Y216, packed 4:2:2 (Learn DXGI_FORMAT): a pair
/// of pixels is one texel of the resource's view format -- (Y0, U0, Y1, V0)
/// of 8 bits (R8G8B8A8) or 16 bits (R16G16B16A16) -- so the host texture is
/// that format at half the resource's width, and pixel x is host texel
/// x / 2. The width is even (Learn; measured, d3d11_video_formats_probe
/// [create] "size 7x4" E_INVALIDARG), and so must be every region edge: an
/// UpdateSubresource box or copy with an odd left or right edge, or an odd
/// DstX, writes nothing (measured [layout] "box1,1-5,3", "copy@1,1").
/// 1 for every other format.
[[nodiscard]] inline constexpr u32 dxgi_pixels_per_texel(u32 format) noexcept {
    return format == 107u || format == 108u || format == 109u ? 2u : 1u;
}

/// Whether @p dxgi_format carries depth *and* stencil in one guest-visible
/// pixel while the host keeps the two in separate planes. Such a staging
/// texture is creatable and mappable with the right pitch, but a GPU-to-staging
/// copy of one needs a two-plane readback and a repack that is not built.
[[nodiscard]] inline bool is_packed_depth_stencil(u32 dxgi_format) {
    switch (dxgi_format) {
    case 19: case 20: case 21: case 22:
    case 44: case 45: case 46: case 47:
        return true;
    default:
        return false;
    }
}

inline constexpr u32 D3D11_RESOURCE_DIMENSION_TEXTURE1D = 2;
inline constexpr u32 D3D11_RESOURCE_DIMENSION_TEXTURE2D = 3;
inline constexpr u32 D3D11_RESOURCE_DIMENSION_TEXTURE3D = 4;
inline constexpr u32 D3D11_SRV_DIMENSION_BUFFER = 1;
/// D3D11_TEX1D_SRV and D3D11_TEX1DARRAY_SRV have the same field order as their
/// 2D counterparts (MostDetailedMip, MipLevels[, FirstArraySlice, ArraySize]),
/// so the descriptor parsing is shared -- only the compatibility rule differs.
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE1D = 2;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE1DARRAY = 3;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE2D = 4;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE2DARRAY = 5;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE2DMS = 6;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY = 7;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURE3D = 8;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURECUBE = 9;
inline constexpr u32 D3D11_SRV_DIMENSION_TEXTURECUBEARRAY = 10;
inline constexpr u32 D3D11_SRV_DIMENSION_BUFFEREX = 11;
/// D3D11_TEX1D_RTV / D3D11_TEX1DARRAY_RTV share the field order of their 2D
/// counterparts, so only the compatibility rule differs.
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE1D = 2;
// Learn D3D11_RTV_DIMENSION: BUFFER is 1 (D3D11_BUFFER_RTV).
inline constexpr u32 D3D11_RTV_DIMENSION_BUFFER = 1;
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE1DARRAY = 3;
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE2D = 4;
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE2DARRAY = 5;
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE2DMS = 6;
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY = 7;
// Learn D3D11_RTV_DIMENSION: TEXTURE3D is 8. Its D3D11_TEX3D_RTV carries
// MipSlice, FirstWSlice and WSize — a window of W slices, not array slices.
inline constexpr u32 D3D11_RTV_DIMENSION_TEXTURE3D = 8;
// Learn D3D11_UAV_DIMENSION: BUFFER is 1, TEXTURE2D is 4. A buffer UAV
// names an element window (FirstElement/NumElements/Flags), not a mip.
inline constexpr u32 D3D11_UAV_DIMENSION_BUFFER = 1;
inline constexpr u32 D3D11_UAV_DIMENSION_TEXTURE1D = 2;
inline constexpr u32 D3D11_UAV_DIMENSION_TEXTURE1DARRAY = 3;
inline constexpr u32 D3D11_UAV_DIMENSION_TEXTURE2D = 4;
// Learn D3D11_UAV_DIMENSION: TEXTURE2DARRAY is 5. D3D11_TEX2D_ARRAY_UAV
// carries MipSlice, FirstArraySlice and ArraySize in the same slots as the
// 1D array form. A cubemap has no UAV dimension of its own; a guest views
// its six faces through this.
inline constexpr u32 D3D11_UAV_DIMENSION_TEXTURE2DARRAY = 5;
inline constexpr u32 D3D11_UAV_DIMENSION_TEXTURE3D = 8;
inline constexpr u32 D3D11_DSV_DIMENSION_TEXTURE2D = 3;
inline constexpr u32 D3D11_DSV_DIMENSION_TEXTURE2DARRAY = 4;
/// State-object limits: winerror.h D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS
/// and d3d11.h D3D11_REQ_*_OBJECT_COUNT_PER_DEVICE / D3D11_MAX_MAXANISOTROPY
/// (checked against the SDK headers by tools/audit_d3d11_query_constants.py).
inline constexpr u32 D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS = 0x887C0001u;
inline constexpr u32 D3D11_REQ_BLEND_OBJECT_COUNT_PER_DEVICE = 4096;
inline constexpr u32 D3D11_REQ_DEPTH_STENCIL_OBJECT_COUNT_PER_DEVICE = 4096;
inline constexpr u32 D3D11_REQ_RASTERIZER_OBJECT_COUNT_PER_DEVICE = 4096;
inline constexpr u32 D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE = 4096;
inline constexpr u32 D3D11_MAX_MAXANISOTROPY = 16;
/// The Windows SDK d3d11.h also defines D3D11_MIN_MAXANISOTROPY (0) and
/// D3D11_MIP_LOD_BIAS_MIN / MAX (-16.0f / 15.99f), which the mingw-w64 header
/// lacks; d3d11_a04_state_objects_probe measured each bound accepted and the
/// next value out refused (MaxAnisotropy 0 and 17, MipLODBias -16.0 / 15.99
/// and -16.5 / 16.0).
inline constexpr float D3D11_MIP_LOD_BIAS_MIN = -16.0f;
inline constexpr float D3D11_MIP_LOD_BIAS_MAX = 15.99f;

/// D3D11_QUERY (d3d11.h; checked against the SDK header by
/// tools/audit_d3d11_query_constants.py). The per-stream stream-output values
/// INTERLEAVE statistics and overflow predicates: 8 STATISTICS_STREAM0,
/// 9 OVERFLOW_PREDICATE_STREAM0, 10 STATISTICS_STREAM1, ... 15
/// OVERFLOW_PREDICATE_STREAM3.
inline constexpr u32 D3D11_QUERY_EVENT = 0;
inline constexpr u32 D3D11_QUERY_OCCLUSION = 1;
inline constexpr u32 D3D11_QUERY_PIPELINE_STATISTICS = 4;
inline constexpr u32 D3D11_QUERY_OCCLUSION_PREDICATE = 5;
inline constexpr u32 D3D11_QUERY_SO_STATISTICS = 6;
inline constexpr u32 D3D11_QUERY_SO_OVERFLOW_PREDICATE = 7;
inline constexpr u32 D3D11_QUERY_SO_STATISTICS_STREAM0 = 8;
inline constexpr u32 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 = 9;
inline constexpr u32 D3D11_QUERY_SO_STATISTICS_STREAM1 = 10;
inline constexpr u32 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1 = 11;
inline constexpr u32 D3D11_QUERY_SO_STATISTICS_STREAM2 = 12;
inline constexpr u32 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2 = 13;
inline constexpr u32 D3D11_QUERY_SO_STATISTICS_STREAM3 = 14;
inline constexpr u32 D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3 = 15;
/// One past the last D3D11_QUERY value. CreateQuery accepts every value below
/// it (D3D11.3 20.4.1: "the list of queries that must be supported");
/// anything else is E_INVALIDARG -- measured (d3d11_a10_queries_probe).
inline constexpr u32 D3D11_QUERY_COUNT = 16;
/// D3D11_QUERY_MISC_FLAG: the only defined bit. Learn: "only used with an
/// occlusion predicate", and its data "cannot be returned ... via GetData".
inline constexpr u32 D3D11_QUERY_MISC_PREDICATEHINT = 0x1;
/// D3D11_ASYNC_GETDATA_FLAG: the only defined bit.
inline constexpr u32 D3D11_ASYNC_GETDATA_DONOTFLUSH = 0x1;

[[nodiscard]] inline constexpr bool query_type_valid(u32 query_type) {
    return query_type < D3D11_QUERY_COUNT;
}

/// GetData's exact DataSize, and GetDataSize, per D3D11_QUERY (D3D11.3 20.3.1;
/// sizeof of each D3D11_QUERY_DATA_* measured in d3d11_a10_queries_probe):
/// BOOL 4, UINT64 8, TIMESTAMP_DISJOINT 16, PIPELINE_STATISTICS 88,
/// SO_STATISTICS 16. Zero for a value that is not a query type.
[[nodiscard]] inline constexpr u32 query_data_size(u32 query_type) {
    constexpr u32 kSizes[D3D11_QUERY_COUNT] = {
        4, 8, 8, 16, 88, 4, 16, 4, 16, 4, 16, 4, 16, 4, 16, 4};
    return query_type_valid(query_type) ? kSizes[query_type] : 0;
}

/// The D3D11_QUERY values that name a predicate: OCCLUSION_PREDICATE and
/// every SO_OVERFLOW_PREDICATE*. Such an object answers
/// QueryInterface(ID3D11Predicate) and can drive SetPredication whether
/// CreateQuery or CreatePredicate made it (measured).
[[nodiscard]] inline constexpr bool query_type_is_predicate(u32 query_type) {
    return query_type == D3D11_QUERY_OCCLUSION_PREDICATE ||
           (query_type >= D3D11_QUERY_SO_OVERFLOW_PREDICATE &&
            query_type < D3D11_QUERY_COUNT && (query_type & 1u) != 0);
}

/// ID3D11Device3::CreateQuery1: every type from PIPELINE_STATISTICS on is
/// limited to D3D11_CONTEXT_TYPE_ALL (0) and _3D (1); EVENT, OCCLUSION,
/// TIMESTAMP and TIMESTAMP_DISJOINT take any context type -- measured
/// (d3d11_a10_query_rules_probe [create1]).
[[nodiscard]] inline constexpr bool query_type_needs_3d_context(
    u32 query_type) {
    return query_type >= D3D11_QUERY_PIPELINE_STATISTICS &&
           query_type < D3D11_QUERY_COUNT;
}

/// For a stream-output query or predicate, the stream it counts (-1 for the
/// all-streams forms), or -2 when @p query_type is not a stream-output one.
[[nodiscard]] inline constexpr int stream_out_query_stream(u32 query_type) {
    if (query_type == D3D11_QUERY_SO_STATISTICS ||
        query_type == D3D11_QUERY_SO_OVERFLOW_PREDICATE) {
        return -1;
    }
    if (query_type >= D3D11_QUERY_SO_STATISTICS_STREAM0 &&
        query_type < D3D11_QUERY_COUNT) {
        return static_cast<int>(
            (query_type - D3D11_QUERY_SO_STATISTICS_STREAM0) / 2);
    }
    return -2;
}

/// D3D11_QUERY_DATA_SO_STATISTICS (and the predicate's BOOL) of a
/// stream-output query, from the per-stream {needed, written} primitive
/// totals of its bracket. SO_STATISTICS without a stream counts stream 0
/// (measured, a10 [so] and d3d11_gsso_probe [streams]: a four-stream GS
/// reports stream 0's numbers there); an overflow predicate without a
/// stream is TRUE when ANY stream overflowed (D3D11.3 20.4.10, measured).
struct StreamOutQueryCounts {
    u64 written{0};
    u64 needed{0};
    bool overflowed{false};
};
[[nodiscard]] inline StreamOutQueryCounts stream_out_query_counts(
    u32 query_type, const std::array<u64, 4>& needed,
    const std::array<u64, 4>& written) {
    const int stream = stream_out_query_stream(query_type);
    const usize counted = stream < 0 ? 0u : static_cast<usize>(stream);
    StreamOutQueryCounts out{};
    out.needed = needed[counted];
    out.written = written[counted];
    out.overflowed = out.needed > out.written;
    if (stream == -1) {
        for (usize k = 0; k < 4; ++k) out.overflowed |= needed[k] > written[k];
    }
    return out;
}

/// D3D11_PRIMITIVE_TOPOLOGY (d3dcommon.h D3D_PRIMITIVE_TOPOLOGY_*; checked by
/// tools/audit_d3d11_query_constants.py). N-control-point patch lists are
/// 32 + N, for N in 1..32.
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_POINTLIST = 1;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_LINELIST = 2;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP = 3;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ = 10;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ = 11;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ = 12;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ = 13;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST = 33;
inline constexpr u32 D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST = 64;

/// Primitives the input assembler makes of @p vertices vertices (one
/// instance) in @p topology: lists take whole primitives, strips share
/// vertices (D3D11.3 chapter 8, Input Assembler). @p known is false for a
/// topology that draws nothing on Windows (UNDEFINED or an unknown value).
[[nodiscard]] inline constexpr u64 primitives_in(u32 topology, u64 vertices,
                                                 bool& known) {
    known = true;
    switch (topology) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: return vertices;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: return vertices / 2;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
        return vertices >= 2 ? vertices - 1 : 0;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return vertices / 3;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
        return vertices >= 3 ? vertices - 2 : 0;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return vertices / 4;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
        return vertices >= 4 ? vertices - 3 : 0;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return vertices / 6;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
        return vertices >= 6 ? (vertices - 4) / 2 : 0;
    default:
        if (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
            return vertices /
                   (topology -
                    D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
        }
        known = false;
        return 0;
    }
}

/// Predicated operations on ID3D11DeviceContext[1..3], by vtable slot
/// (d3d11_3.h ID3D11DeviceContext3Vtbl): Learn's SetPredication list, less
/// its four tiled-resource operations -- UpdateTileMappings,
/// CopyTileMappings, CopyTiles and UpdateTiles all take effect under a
/// predicate that skips CopyResource (measured, d3d11_tiled_probe [data] P).
[[nodiscard]] inline constexpr bool predicated_context_slot(usize slot) {
    switch (slot) {
    case 12:   // DrawIndexed
    case 13:   // Draw
    case 20:   // DrawIndexedInstanced
    case 21:   // DrawInstanced
    case 38:   // DrawAuto
    case 39:   // DrawIndexedInstancedIndirect
    case 40:   // DrawInstancedIndirect
    case 41:   // Dispatch
    case 42:   // DispatchIndirect
    case 46:   // CopySubresourceRegion
    case 47:   // CopyResource
    case 48:   // UpdateSubresource
    case 49:   // CopyStructureCount
    case 50:   // ClearRenderTargetView
    case 51:   // ClearUnorderedAccessViewUint
    case 52:   // ClearUnorderedAccessViewFloat
    case 53:   // ClearDepthStencilView
    case 54:   // GenerateMips
    case 57:   // ResolveSubresource
    case 115:  // CopySubresourceRegion1
    case 116:  // UpdateSubresource1
    case 132:  // ClearView
        return true;
    default:
        return false;
    }
}

/// The D3D11_QUERY_DATA_PIPELINE_STATISTICS fields, in struct order: index i
/// is the UINT64 at byte offset 8 * i. d3d11.h (Windows Kits 10.0.26100 and
/// mingw-w64 alike) keeps D3D10's eight fields first and appends HS, DS and
/// CS -- NOT the order D3D11.3 20.4.7 lists them in, which put HS and DS
/// after VS here and wrote every count past VSInvocations into another
/// field. tools/audit_d3d11_query_constants.py checks this order against the
/// header, as graphics::STAT_* (pipeline_stats.hpp) must follow it.
enum class PipelineStatistic : u32 {
    IAVertices,
    IAPrimitives,
    VSInvocations,
    GSInvocations,
    GSPrimitives,
    CInvocations,
    CPrimitives,
    PSInvocations,
    HSInvocations,
    DSInvocations,
    CSInvocations,
    Count,
};
inline constexpr std::array<const char*,
                            static_cast<usize>(PipelineStatistic::Count)>
    PIPELINE_STATISTIC_NAMES{{"IAVertices", "IAPrimitives", "VSInvocations",
                              "GSInvocations", "GSPrimitives", "CInvocations",
                              "CPrimitives", "PSInvocations", "HSInvocations",
                              "DSInvocations", "CSInvocations"}};
static_assert(static_cast<u32>(PipelineStatistic::PSInvocations) ==
                  graphics::STAT_PS_INVOCATIONS &&
              static_cast<u32>(PipelineStatistic::HSInvocations) ==
                  graphics::STAT_HS_INVOCATIONS &&
              static_cast<u32>(PipelineStatistic::CPrimitives) ==
                  graphics::STAT_C_PRIMITIVES &&
              static_cast<u32>(PipelineStatistic::Count) ==
                  graphics::STATS_COUNTERS,
              "the GPU statistics record follows the struct order");
[[nodiscard]] inline constexpr u32 pipeline_statistic_bit(
    PipelineStatistic statistic) {
    return 1u << static_cast<u32>(statistic);
}

/// Where records laid out as @p record_registers (the output register each
/// vec4 slot holds) keep a stage's SV_Position and SV_ClipDistance components
/// -- the elements of @p outputs on @p stream -- for the PIPELINE_STATISTICS
/// clipper (graphics::StatsClipLayout; invalid without a position).
[[nodiscard]] graphics::StatsClipLayout stats_clip_layout(
    const std::vector<graphics::DxbcSignatureElement>& outputs,
    const std::vector<u32>& record_registers, u32 stream);

/// ID3D11Predicate {9eb576dd-9f77-4d86-81aa-8bab5fe490e2}. The interface adds
/// no methods to ID3D11Query, so one vtable serves both.
inline constexpr GuidBytes IID_ID3D11PREDICATE{
    0xDD, 0x76, 0xB5, 0x9E, 0x77, 0x9F, 0x86, 0x4D,
    0x81, 0xAA, 0x8B, 0xAB, 0x5F, 0xE4, 0x90, 0xE2};

inline constexpr u32 D3D11_FORMAT_SUPPORT_TEXTURE1D = 0x10u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_TEXTURE2D = 0x20u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_TEXTURE3D = 0x40u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_TEXTURECUBE = 0x80u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_SHADER_LOAD = 0x100u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_SHADER_SAMPLE = 0x200u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_SHADER_SAMPLE_COMPARISON = 0x400u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_MIP = 0x1000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_RENDER_TARGET = 0x4000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_BLENDABLE = 0x8000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_DEPTH_STENCIL = 0x10000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE = 0x40000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_DISPLAY = 0x80000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET = 0x200000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD = 0x400000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW =
    0x2000000u;
inline constexpr u32 D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD = 0x40u;
inline constexpr u32 D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE = 0x80u;

struct DxgiTypelessFamily {
    u32 typeless;
    std::array<u32, 6> typed;
    u8 typed_count;
};

inline constexpr DxgiTypelessFamily DXGI_TYPELESS_FAMILIES[] = {
    {1, {{2, 3, 4, 0, 0, 0}}, 3},
    {5, {{6, 7, 8, 0, 0, 0}}, 3},
    {9, {{10, 11, 12, 13, 14, 0}}, 5},
    {15, {{16, 17, 18, 0, 0, 0}}, 3},
    {19, {{20, 21, 22, 0, 0, 0}}, 3},
    {23, {{24, 25, 0, 0, 0, 0}}, 2},
    {27, {{28, 29, 30, 31, 32, 0}}, 5},
    {33, {{34, 35, 36, 37, 38, 0}}, 5},
    {39, {{40, 41, 42, 43, 0, 0}}, 4},
    {44, {{45, 46, 47, 0, 0, 0}}, 3},
    {48, {{49, 50, 51, 52, 0, 0}}, 4},
    {53, {{54, 55, 56, 57, 58, 59}}, 6},
    {60, {{61, 62, 63, 64, 65, 0}}, 5},
    {70, {{71, 72, 0, 0, 0, 0}}, 2},
    {73, {{74, 75, 0, 0, 0, 0}}, 2},
    {76, {{77, 78, 0, 0, 0, 0}}, 2},
    {79, {{80, 81, 0, 0, 0, 0}}, 2},
    {82, {{83, 84, 0, 0, 0, 0}}, 2},
    {90, {{87, 91, 0, 0, 0, 0}}, 2},
    {92, {{88, 93, 0, 0, 0, 0}}, 2},
    {94, {{95, 96, 0, 0, 0, 0}}, 2},
    {97, {{98, 99, 0, 0, 0, 0}}, 2},
};

[[nodiscard]] inline constexpr const DxgiTypelessFamily* dxgi_typeless_family(
    u32 format) noexcept {
    for (const auto& family : DXGI_TYPELESS_FAMILIES) {
        if (format == family.typeless) return &family;
        for (u8 i = 0; i < family.typed_count; ++i) {
            if (format == family.typed[i]) return &family;
        }
    }
    return nullptr;
}

[[nodiscard]] inline constexpr bool dxgi_format_is_typeless(
    u32 format) noexcept {
    const auto* family = dxgi_typeless_family(format);
    return family != nullptr && family->typeless == format;
}

/// DXGI_FORMAT_NV12 / P010 / P016 / 420_OPAQUE: planar 4:2:0 video formats
/// whose one subresource holds a luma plane and, below it at the same row
/// pitch, a half-height interleaved chroma plane (Learn DXGI_FORMAT: "Direct3D
/// 11 staging resources and initData parameters for this format use (rowPitch
/// * (height + (height / 2))) bytes" -- said of NV12 and of 420_OPAQUE, whose
/// layout is otherwise opaque: no CPU access, no views, only copies).
[[nodiscard]] inline constexpr bool dxgi_format_is_planar(u32 format) noexcept {
    return format == 103u || format == 104u || format == 105u ||
           format == 106u;
}

/// The DXGI video resource formats a view names by one of a few element
/// formats rather than by the resource's own (Learn DXGI_FORMAT): the planar
/// 4:2:0 ones and the packed AYUV, Y410, Y416, YUY2, Y210, Y216.
[[nodiscard]] inline constexpr bool dxgi_format_has_video_views(
    u32 format) noexcept {
    return dxgi_format_is_planar(format) ||
           (format >= 100u && format <= 102u) ||  // AYUV, Y410, Y416
           (format >= 107u && format <= 109u);    // YUY2, Y210, Y216
}

/// Rows of one subresource of @p format at level height @p height: both
/// planes of a planar format.
[[nodiscard]] inline constexpr u32 dxgi_subresource_rows(u32 format,
                                                         u32 height) noexcept {
    return dxgi_format_is_planar(format) ? height + height / 2u : height;
}

/// The plane a view of @p view format selects on a video @p resource
/// (dxgi_format_has_video_views), or -1 when the view format names no plane
/// of it. Learn DXGI_FORMAT:
///   NV12: luma views R8_UNORM / R8_UINT, chroma views R8G8_UNORM / _UINT;
///   P010 / P016: R16 and R16G16, and R32_UINT for chroma UAVs;
///   AYUV, YUY2: R8G8B8A8_UNORM / _UINT, and an R32_UINT UAV;
///   Y410: R10G10B10A2_UNORM / _UINT, and an R32_UINT UAV;
///   Y416, Y210, Y216: R16G16B16A16_UNORM / _UINT;
///   420_OPAQUE: none ("You cannot use shaders with this format").
/// Measured (d3d11_nv12_probe and d3d11_video_formats_probe [views]): every
/// other format, UNKNOWN, the resource's own format and a NULL description
/// are E_INVALIDARG, and a packed format's only plane is 0. YUY2's legacy
/// full-width R8G8_B8G8_UNORM SRV is refused separately
/// (dxgi_yuy2_rgb_view).
[[nodiscard]] inline constexpr int dxgi_video_view_plane(u32 resource,
                                                         u32 view,
                                                         bool uav) noexcept {
    switch (resource) {
    case 103u:  // NV12
        if (view == 61u || view == 62u) return 0;   // R8_UNORM, R8_UINT
        if (view == 49u || view == 50u) return 1;   // R8G8_UNORM, R8G8_UINT
        return -1;
    case 104u:  // P010
    case 105u:  // P016
        if (view == 56u || view == 57u) return 0;   // R16_UNORM, R16_UINT
        if (view == 35u || view == 36u) return 1;   // R16G16_UNORM, _UINT
        if (uav && view == 42u) return 1;           // R32_UINT
        return -1;
    case 100u:  // AYUV
    case 107u:  // YUY2
        if (view == 28u || view == 30u) return 0;   // R8G8B8A8_UNORM, _UINT
        if (uav && view == 42u) return 0;           // R32_UINT
        return -1;
    case 101u:  // Y410
        if (view == 24u || view == 25u) return 0;   // R10G10B10A2_UNORM, _UINT
        if (uav && view == 42u) return 0;           // R32_UINT
        return -1;
    case 102u:  // Y416
    case 108u:  // Y210
    case 109u:  // Y216
        if (view == 11u || view == 12u) return 0;   // R16G16B16A16_UNORM, _UINT
        return -1;
    default:
        return -1;
    }
}

/// Learn DXGI_FORMAT_YUY2: "A unique valid view format for this video
/// resource format is DXGI_FORMAT_R8G8_B8G8_UNORM", a full-width SRV that
/// rebuilds RGB per pixel before filtering (measured: an 8x4 YUY2 reads 8x4,
/// texel x = (Y0, x even ? U0 : V0, Y1, 1)). Metal has no texel format a view
/// of the half-width storage could take, so the view is refused as
/// unimplemented.
[[nodiscard]] inline constexpr bool dxgi_yuy2_rgb_view(u32 resource,
                                                       u32 view) noexcept {
    return resource == 107u && view == 68u;
}

[[nodiscard]] inline constexpr bool dxgi_typeless_view_compatible(
    u32 resource, u32 view) noexcept {
    if (resource == view) return true;
    const auto* family = dxgi_typeless_family(resource);
    if (family == nullptr || family->typeless != resource) return false;
    for (u8 i = 0; i < family->typed_count; ++i) {
        if (view == family->typed[i]) return true;
    }
    return false;
}

/// D3D11.3 functional spec §5.3.9.5 "Limitations on Typed UAVs": a Texture1D,
/// 2D or 3D created with one of the 32-bit TYPELESS formats R10G10B10A2,
/// R8G8B8A8, B8G8R8A8, B8G8R8X8, R16G16 or R32 may also be viewed by a UAV of
/// R32_UINT, R32_SINT or R32_FLOAT -- the one way a shader reads and writes a
/// packed texture as whole words (measured, d3d11_a03_views_probe "UAV R32
/// casting"). Neither a buffer nor a typed resource casts.
[[nodiscard]] inline constexpr bool dxgi_uav_r32_cast(u32 resource,
                                                      u32 view) noexcept {
    if (view != 41u && view != 42u && view != 43u) return false;
    switch (resource) {
    case 23u:  // R10G10B10A2_TYPELESS
    case 27u:  // R8G8B8A8_TYPELESS
    case 33u:  // R16G16_TYPELESS
    case 39u:  // R32_TYPELESS
    case 90u:  // B8G8R8A8_TYPELESS
    case 92u:  // B8G8R8X8_TYPELESS
        return true;
    default:
        return false;
    }
}

/// The formats a texture UAV may take over a resource of @p resource: the
/// resource's own, a typed member of its typeless family, or an R32 cast.
[[nodiscard]] inline constexpr bool dxgi_uav_view_compatible(
    u32 resource, u32 view) noexcept {
    return dxgi_typeless_view_compatible(resource, view) ||
           dxgi_uav_r32_cast(resource, view);
}

/// Learn CopyResource/CopySubresourceRegion: copy-compatible formats may be
/// identical or typed members of the same DXGI typeless group. The operation
/// copies/reinterprets bits; it does not perform a color-space conversion.
[[nodiscard]] inline constexpr bool dxgi_copy_formats_compatible(
    u32 destination, u32 source) noexcept {
    if (destination == source) return true;
    const auto* destination_family = dxgi_typeless_family(destination);
    const auto* source_family = dxgi_typeless_family(source);
    return destination_family != nullptr && source_family != nullptr &&
           destination_family->typeless == source_family->typeless;
}

/// Learn "Format Conversion using Direct3D 10.1": CopyResource and
/// CopySubresourceRegion also move bits between a block-compressed format and
/// an uncompressed one whose texel is the same size as one 4x4 block (and
/// between R9G9B9E5_SHAREDEXP and R32_UINT/SINT). One uncompressed texel is
/// one block: a 4x4 R32G32_UINT region is a 16x16 BC1 region. The classes:
///   32-bit:  R9G9B9E5_SHAREDEXP <-> R32_UINT, R32_SINT
///   64-bit:  BC1, BC4 <-> R16G16B16A16_UINT/SINT, R32G32_UINT/SINT
///   128-bit: BC2, BC3, BC5 <-> R32G32B32A32_UINT/SINT
/// BC6H and BC7 (D3D11 formats with 128-bit blocks) copy the same way on
/// Windows (a06 probe: R32G32B32A32_UINT 4x4 -> BC7 16x16). Each BC entry
/// includes its typeless and typed members.
struct DxgiReinterpretClass {
    u32 bits{0};          ///< element size; 0 = not a reinterpretable format
    bool packed{false};   ///< the block-compressed / shared-exponent side
};

[[nodiscard]] inline constexpr DxgiReinterpretClass
dxgi_reinterpret_copy_class(u32 format) noexcept {
    switch (format) {
    case 67:                                 // R9G9B9E5_SHAREDEXP
        return {32, true};
    case 42: case 43:                        // R32_UINT/SINT
        return {32, false};
    case 70: case 71: case 72:               // BC1
    case 79: case 80: case 81:               // BC4
        return {64, true};
    case 12: case 14: case 17: case 18:      // RGBA16 / RG32 UINT/SINT
        return {64, false};
    case 73: case 74: case 75:               // BC2
    case 76: case 77: case 78:               // BC3
    case 82: case 83: case 84:               // BC5
    case 94: case 95: case 96:               // BC6H
    case 97: case 98: case 99:               // BC7
        return {128, true};
    case 3: case 4:                          // RGBA32 UINT/SINT
        return {128, false};
    default:
        return {};
    }
}

/// True when the pair is one of the reinterpretation copies above: one side
/// from each half of the same size class.
[[nodiscard]] inline constexpr bool dxgi_reinterpret_copy_compatible(
    u32 destination, u32 source) noexcept {
    const DxgiReinterpretClass a = dxgi_reinterpret_copy_class(destination);
    const DxgiReinterpretClass b = dxgi_reinterpret_copy_class(source);
    return a.bits != 0 && a.bits == b.bits && a.packed != b.packed;
}

static_assert(dxgi_reinterpret_copy_compatible(17, 71));   // RG32_UINT, BC1
static_assert(dxgi_reinterpret_copy_compatible(98, 3));    // BC7, RGBA32_UINT
static_assert(dxgi_reinterpret_copy_compatible(42, 67));   // R32_UINT, RGB9E5
static_assert(!dxgi_reinterpret_copy_compatible(71, 3));   // 64 vs 128 bit
static_assert(!dxgi_reinterpret_copy_compatible(71, 80));  // BC1 vs BC4
static_assert(!dxgi_reinterpret_copy_compatible(17, 12));  // both plain

static_assert(dxgi_copy_formats_compatible(29, 28));
static_assert(dxgi_copy_formats_compatible(2, 3));
static_assert(!dxgi_copy_formats_compatible(28, 87));

/// Learn ResolveSubresource compatible-format scenarios. Typed resources must
/// match exactly; a typeless side may pair with the typed member named by
/// `resolve`, and two typeless resources require the same typeless family.
[[nodiscard]] inline constexpr bool dxgi_resolve_formats_compatible(
    u32 source, u32 destination, u32 resolve) noexcept {
    const bool source_typeless = dxgi_format_is_typeless(source);
    const bool destination_typeless = dxgi_format_is_typeless(destination);
    if (!source_typeless && !destination_typeless) {
        return source == destination && resolve == source;
    }
    const auto* source_family = dxgi_typeless_family(source);
    const auto* destination_family = dxgi_typeless_family(destination);
    if (source_family == nullptr || destination_family == nullptr ||
        source_family->typeless != destination_family->typeless ||
        dxgi_format_is_typeless(resolve)) {
        return false;
    }
    if (!dxgi_typeless_view_compatible(source_family->typeless, resolve)) {
        return false;
    }
    if (!source_typeless && source != resolve) return false;
    if (!destination_typeless && destination != resolve) return false;
    return true;
}

static_assert(dxgi_resolve_formats_compatible(27, 27, 29));
static_assert(dxgi_resolve_formats_compatible(27, 29, 29));
static_assert(!dxgi_resolve_formats_compatible(28, 28, 29));
static_assert(!dxgi_resolve_formats_compatible(27, 90, 28));

/// Learn D3D11_TEXTURE2D_DESC.MipLevels: 0 means a full mip chain.
[[nodiscard]] inline u32 full_mip_levels(u32 width, u32 height) {
    u32 extent = std::max(width, height);
    u32 levels = 1;
    while (extent > 1u) {
        extent >>= 1u;
        ++levels;
    }
    return levels;
}

[[nodiscard]] inline u32 full_mip_levels(u32 width, u32 height, u32 depth) {
    return full_mip_levels(std::max(width, depth), height);
}

inline constexpr std::array<const char*, d3d11::DEVICE_METHOD_COUNT>
    DEVICE_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "CreateBuffer",
        "CreateTexture1D", "CreateTexture2D", "CreateTexture3D",
        "CreateShaderResourceView", "CreateUnorderedAccessView",
        "CreateRenderTargetView", "CreateDepthStencilView",
        "CreateInputLayout", "CreateVertexShader", "CreateGeometryShader",
        "CreateGeometryShaderWithStreamOutput", "CreatePixelShader",
        "CreateHullShader", "CreateDomainShader", "CreateComputeShader",
        "CreateClassLinkage", "CreateBlendState", "CreateDepthStencilState",
        "CreateRasterizerState", "CreateSamplerState", "CreateQuery",
        "CreatePredicate", "CreateCounter", "CreateDeferredContext",
        "OpenSharedResource", "CheckFormatSupport",
        "CheckMultisampleQualityLevels", "CheckCounterInfo", "CheckCounter",
        "CheckFeatureSupport", "GetPrivateData", "SetPrivateData",
        "SetPrivateDataInterface", "GetFeatureLevel", "GetCreationFlags",
        "GetDeviceRemovedReason", "GetImmediateContext", "SetExceptionMode",
        "GetExceptionMode",
        "GetImmediateContext1", "CreateDeferredContext1",
        "CreateBlendState1", "CreateRasterizerState1",
        "CreateDeviceContextState", "OpenSharedResource1",
        "OpenSharedResourceByName",
        "GetImmediateContext2", "CreateDeferredContext2",
        "GetResourceTiling", "CheckMultisampleQualityLevels1",
        "CreateTexture2D1", "CreateTexture3D1", "CreateRasterizerState2",
        "CreateShaderResourceView1", "CreateUnorderedAccessView1",
        "CreateRenderTargetView1", "CreateQuery1",
        "GetImmediateContext3", "CreateDeferredContext3",
        "WriteToSubresource", "ReadFromSubresource",
        "RegisterDeviceRemovedEvent", "UnregisterDeviceRemoved",
        "OpenSharedFence", "CreateFence",
    }};

inline constexpr std::array<const char*, d3d11::QUERY_METHOD_COUNT>
    QUERY_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "GetDevice",
        "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
        "GetDataSize", "GetDesc", "GetDesc1",
    }};

inline constexpr std::array<const char*, d3d11::DXGI_DEVICE_METHOD_COUNT>
    DXGI_DEVICE_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "SetPrivateData",
        "SetPrivateDataInterface", "GetPrivateData", "GetParent",
        "GetAdapter", "CreateSurface", "QueryResourceResidency",
        "SetGPUThreadPriority", "GetGPUThreadPriority",
        "SetMaximumFrameLatency", "GetMaximumFrameLatency",
        "OfferResources", "ReclaimResources", "EnqueueSetEvent", "Trim",
        "OfferResources1", "ReclaimResources1",
    }};

inline constexpr std::array<const char*, d3d11::VIDEO_CONTEXT_METHOD_COUNT>
    VIDEO_CONTEXT_METHOD_NAMES{{
    "QueryInterface", "AddRef", "Release", "GetDevice", "GetPrivateData",
    "SetPrivateData", "SetPrivateDataInterface", "GetDecoderBuffer",
    "ReleaseDecoderBuffer", "DecoderBeginFrame", "DecoderEndFrame",
    "SubmitDecoderBuffers", "DecoderExtension",
    "VideoProcessorSetOutputTargetRect",
    "VideoProcessorSetOutputBackgroundColor",
    "VideoProcessorSetOutputColorSpace",
    "VideoProcessorSetOutputAlphaFillMode",
    "VideoProcessorSetOutputConstriction",
    "VideoProcessorSetOutputStereoMode", "VideoProcessorSetOutputExtension",
    "VideoProcessorGetOutputTargetRect",
    "VideoProcessorGetOutputBackgroundColor",
    "VideoProcessorGetOutputColorSpace",
    "VideoProcessorGetOutputAlphaFillMode",
    "VideoProcessorGetOutputConstriction",
    "VideoProcessorGetOutputStereoMode", "VideoProcessorGetOutputExtension",
    "VideoProcessorSetStreamFrameFormat",
    "VideoProcessorSetStreamColorSpace",
    "VideoProcessorSetStreamOutputRate",
    "VideoProcessorSetStreamSourceRect", "VideoProcessorSetStreamDestRect",
    "VideoProcessorSetStreamAlpha", "VideoProcessorSetStreamPalette",
    "VideoProcessorSetStreamPixelAspectRatio",
    "VideoProcessorSetStreamLumaKey", "VideoProcessorSetStreamStereoFormat",
    "VideoProcessorSetStreamAutoProcessingMode",
    "VideoProcessorSetStreamFilter", "VideoProcessorSetStreamExtension",
    "VideoProcessorGetStreamFrameFormat",
    "VideoProcessorGetStreamColorSpace",
    "VideoProcessorGetStreamOutputRate",
    "VideoProcessorGetStreamSourceRect", "VideoProcessorGetStreamDestRect",
    "VideoProcessorGetStreamAlpha", "VideoProcessorGetStreamPalette",
    "VideoProcessorGetStreamPixelAspectRatio",
    "VideoProcessorGetStreamLumaKey", "VideoProcessorGetStreamStereoFormat",
    "VideoProcessorGetStreamAutoProcessingMode",
    "VideoProcessorGetStreamFilter", "VideoProcessorGetStreamExtension",
    "VideoProcessorBlt", "NegotiateCryptoSessionKeyExchange",
    "EncryptionBlt", "DecryptionBlt", "StartSessionKeyRefresh",
    "FinishSessionKeyRefresh", "GetEncryptionBltKey",
    "NegotiateAuthenticatedChannelKeyExchange", "QueryAuthenticatedChannel",
    "ConfigureAuthenticatedChannel", "VideoProcessorSetStreamRotation",
    "VideoProcessorGetStreamRotation", "SubmitDecoderBuffers1",
    "GetDataForNewHardwareKey", "CheckCryptoSessionStatus",
    "DecoderEnableDownsampling", "DecoderUpdateDownsampling",
    "VideoProcessorSetOutputColorSpace1",
    "VideoProcessorSetOutputShaderUsage",
    "VideoProcessorGetOutputColorSpace1",
    "VideoProcessorGetOutputShaderUsage",
    "VideoProcessorSetStreamColorSpace1", "VideoProcessorSetStreamMirror",
    "VideoProcessorGetStreamColorSpace1", "VideoProcessorGetStreamMirror",
    "VideoProcessorGetBehaviorHints", "VideoProcessorSetOutputHDRMetaData",
    "VideoProcessorGetOutputHDRMetaData",
    "VideoProcessorSetStreamHDRMetaData",
    "VideoProcessorGetStreamHDRMetaData", "DecoderBeginFrame1",
    "SubmitDecoderBuffers2"
}};

inline constexpr std::array<const char*, d3d11::BUFFER_METHOD_COUNT>
    BUFFER_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "GetDevice",
        "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
        "GetType", "SetEvictionPriority", "GetEvictionPriority", "GetDesc",
    }};

inline constexpr std::array<const char*, d3d11::TEXTURE_2D_METHOD_COUNT>
    TEXTURE_2D_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "GetDevice",
        "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
        "GetType", "SetEvictionPriority", "GetEvictionPriority", "GetDesc",
        "GetDesc1",
    }};

inline constexpr std::array<const char*,
                     d3d11::SHADER_RESOURCE_VIEW_METHOD_COUNT>
    SHADER_RESOURCE_VIEW_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "GetDevice",
        "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
        "GetResource", "GetDesc", "GetDesc1",
    }};

inline constexpr std::array<const char*, d3d11::RENDER_TARGET_VIEW_METHOD_COUNT>
    RENDER_TARGET_VIEW_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "GetDevice",
        "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface",
        "GetResource", "GetDesc", "GetDesc1",
    }};

inline constexpr std::array<const char*, d3d11::SWAP_CHAIN_METHOD_COUNT>
    SWAP_CHAIN_METHOD_NAMES{{
        "QueryInterface", "AddRef", "Release", "SetPrivateData",
        "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetDevice",
        "Present", "GetBuffer", "SetFullscreenState", "GetFullscreenState",
        "GetDesc", "ResizeBuffers", "ResizeTarget", "GetContainingOutput",
        "GetFrameStatistics", "GetLastPresentCount",
        "GetDesc1", "GetFullscreenDesc", "GetHwnd", "GetCoreWindow",
        "Present1", "IsTemporaryMonoSupported", "GetRestrictToOutput",
        "SetBackgroundColor", "GetBackgroundColor", "SetRotation",
        "GetRotation",
        "SetSourceSize", "GetSourceSize", "SetMaximumFrameLatency",
        "GetMaximumFrameLatency", "GetFrameLatencyWaitableObject",
        "SetMatrixTransform", "GetMatrixTransform",
        "GetCurrentBackBufferIndex", "CheckColorSpaceSupport",
        "SetColorSpace1", "ResizeBuffers1", "SetHDRMetaData",
    }};

/// Marker-file line filter shared by every D3D11 diagnostic. The marker's
/// contents are a comma-separated set of substrings; an empty marker keeps
/// everything. Both diagnostics must honour it or one of them floods the disk
/// and starves the launch being diagnosed.
[[nodiscard]] inline const std::string& d3d11_diag_line_filter() {
    static const std::string filter = [] {
        std::string text;
        if (::access("/tmp/vitreo-d3d11-diag", F_OK) != 0) return text;
        if (std::FILE* file = std::fopen("/tmp/vitreo-d3d11-diag", "r")) {
            char buffer[256];
            if (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
                text = buffer;
                while (!text.empty() &&
                       (text.back() == '\n' || text.back() == '\r')) {
                    text.pop_back();
                }
            }
            (void)std::fclose(file);
        }
        return text;
    }();
    return filter;
}

[[nodiscard]] inline bool d3d11_diag_line_allowed(const char* message) {
    const std::string& filter = d3d11_diag_line_filter();
    if (filter.empty()) return true;
    const std::string_view text(message);
    std::string_view remaining(filter);
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const std::string_view token = remaining.substr(0, comma);
        if (!token.empty() && text.find(token) != std::string_view::npos) {
            return true;
        }
        if (comma == std::string_view::npos) break;
        remaining = remaining.substr(comma + 1);
    }
    return false;
}

/// Count @p format (a string literal) when it reports a skipped draw, a
/// REJECT, a failure or an UNIMPLEMENTED case -- from process start, switch
/// on or off -- and, with the switch on, print the tally every 5 s.
void d3d11_skip_stat(const char* format);
/// VITREO_D3D11_SKIP_STATS or the /tmp/vitreo-d3d11-skip-stats marker
/// (re-probed at most once per second). Cheap enough to call per draw.
bool d3d11_skip_stats_enabled() noexcept;
/// Print the skip tally if 5 s passed since the last report: the first one
/// gives every count since process start. The caller checks the switch.
void d3d11_skip_stat_report();
/// Skip events counted since process start (tests, diagnostics).
[[nodiscard]] u64 d3d11_skip_events() noexcept;

inline void d3d11_diag(const char* format, ...) {
    d3d11_skip_stat(format);
    // A guest launched by a store client is several re-execs removed from the
    // shell and does not reliably inherit environment variables, so an
    // env-only switch silently produces nothing for exactly the processes
    // worth diagnosing. A marker file always arrives: touch
    // /tmp/vitreo-d3d11-diag to enable, and the log lands beside it.
    static const bool marker_present =
        ::access("/tmp/vitreo-d3d11-diag", F_OK) == 0;
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D11_DIAG");
        const bool on = marker_present ||
                        (value != nullptr && value[0] != '\0' &&
                         std::strcmp(value, "0") != 0);
        // Every line is written and flushed from the calling thread: a
        // forgotten marker cost a fifth of the RHI thread in every later run
        // and nothing in the log said so.
        if (on) {
            std::fprintf(stderr,
                         "[vitreo] WARNING: D3D11 diagnostic trace is on (%s); "
                         "it logs and flushes every call and slows rendering\n",
                         marker_present ? "/tmp/vitreo-d3d11-diag exists"
                                        : "VITREO_D3D11_DIAG");
        }
        return on;
    }();
    if (!enabled) return;

    static std::FILE* const output = [] {
        const char* path = std::getenv("VITREO_D3D11_DIAG_FILE");
        if (path != nullptr && path[0] != '\0') {
            if (std::FILE* file = std::fopen(path, "a")) return file;
        }
        if (marker_present) {
            char path_buf[128];
            std::snprintf(path_buf, sizeof(path_buf),
                          "/tmp/vitreo-d3d11-diag-%d.log",
                          static_cast<int>(getpid()));
            if (std::FILE* file = std::fopen(path_buf, "a")) return file;
        }
        return stderr;
    }();

    char message[2048];
    va_list args;
    va_start(args, format);
    (void)std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (!d3d11_diag_line_allowed(message)) return;

    // A multi-process guest spreads its rendering over browser, GPU, and
    // renderer processes that all log here, so the pid is what makes the log
    // readable.
    // A plain mutex rather than flockfile: the stdio lock is a property of
    // the FILE object, and locking one a test has already invalidated aborts
    // the process ("mutex lock failed: Invalid argument"). A diagnostic must
    // never be able to take the program down.
    // No explicit lock at all: stdio calls are already thread-safe per FILE,
    // and every extra synchronisation primitive here is one more thing a
    // diagnostic can abort the process with. The whole line is formatted
    // first, so a single fprintf keeps it intact.
    if (output == nullptr) return;
    std::fprintf(output, "[d3d11-diag pid=%d] %s\n",
                 static_cast<int>(getpid()), message);
    std::fflush(output);
}

/// One `[vitreo] UNIMPLEMENTED d3d11!<method>: <case>` line per distinct
/// method and case, however often the guest hits it.
inline void d3d11_unimplemented_once(const char* method,
                                     const std::string& case_text) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::string key = std::string(method) + ": " + case_text;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!reported.insert(key).second) return;
    }
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED d3d11!%s\n", key.c_str());
}

[[nodiscard]] inline std::string method_export_name(const char* interface_name,
                                             usize slot) {
    char name[64];
    std::snprintf(name, sizeof(name), "_D3D11_%s_%03zu",
                  interface_name, slot);
    return name;
}

[[nodiscard]] inline bool guid_equal(const GuidBytes& left,
                              const GuidBytes& right) noexcept {
    return left == right;
}

// DXGI_SCALING / DXGI_ALPHA_MODE (dxgi1_2.h). Shared by the swap-chain code
// and D3D11CreateDeviceAndSwapChain, which stamps the legacy-chain defaults.
inline constexpr u32 kDxgiScalingStretch = 0;
inline constexpr u32 kDxgiScalingNone = 1;
inline constexpr u32 kDxgiAlphaUnspecified = 0;
inline constexpr u32 kDxgiAlphaPremultiplied = 1;
inline constexpr u32 kDxgiAlphaIgnore = 3;

} // namespace vitreo::winapi
