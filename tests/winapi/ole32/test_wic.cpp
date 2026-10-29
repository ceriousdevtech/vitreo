/// @file test_wic.cpp
/// @brief WIC imaging factory: BMP decode through decoder, frame and converter.

#include <gtest/gtest.h>

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/com/ole32/com_core.hpp>
#include <winapi/com/ole32/wic.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include "../../../tools/oracle_probe/wic_stream_probe_image.inc"
#include "../../../tools/oracle_probe/wic_indexed_probe_images.inc"
#include "../../../tools/oracle_probe/wic_indexed_matrix_images.inc"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
#include <winapi/com/ole32/wic_stream_contract.inc>
#include <winapi/com/ole32/wic_indexed_contract.inc>
#include "wic_indexed_oracle.inc"
#include "wic_indexed_matrix_oracle.inc"
#include "wic_jpeg_transport_oracle.inc"
#include "../../../tools/oracle_probe/wic_jpeg_probe_images.inc"

constexpr Guid kClsidWicImagingFactory{
    0xCACAF262u, 0x9370u, 0x4615u, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
constexpr Guid kIidImagingFactory{
    0xEC5EC8A9u, 0xC395u, 0x4314u, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
constexpr Guid kIidBitmapSource{
    0x00000120u, 0xA8F2u, 0x4877u, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr Guid kIidBitmapDecoder{
    0x9EDDE9E7u, 0x8DEEu, 0x47EAu, {0x99, 0xDF, 0xE6, 0xFA, 0xF2, 0xED, 0x44, 0xBF}};
constexpr Guid pixel_format(u8 index) {
    return Guid{0x6FDDC324u, 0x4E03u, 0x4BFEu,
                {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, index}};
}
constexpr u8 kPf24bppBGR = 0x0C;
constexpr u8 kPf32bppBGR = 0x0E;
constexpr u8 kPf32bppRGBA = 0x1F;
constexpr u8 kPf8bppIndexed = 0x04;

constexpr u32 kWincodecNotInitialized = 0x88982F0Cu;
constexpr u32 kWincodecComponentNotFound = 0x88982F50u;
constexpr u32 kWincodecFrameMissing = 0x88982F62u;
constexpr u32 kWincodecUnsupportedPixelFormat = 0x88982F80u;
constexpr u32 kWincodecInsufficientBuffer = 0x88982F8Cu;
constexpr u32 kHrFileNotFound = 0x80070002u;

/// A 2x2 24-bit bottom-up BMP: top row (10,20,30) (40,50,60) as R,G,B,
/// bottom row (70,80,90) (100,110,120).
std::vector<u8> make_bmp() {
    std::vector<u8> f;
    const auto u16le = [&](u16 v) { f.push_back(v & 0xFF); f.push_back(v >> 8); };
    const auto u32le = [&](u32 v) { for (int i = 0; i < 4; ++i) f.push_back((v >> (8 * i)) & 0xFF); };
    const u32 row_bytes = 8; // 2 px * 3 bytes, padded to 4
    f.push_back('B'); f.push_back('M');
    u32le(14 + 40 + row_bytes * 2); u16le(0); u16le(0); u32le(14 + 40);
    u32le(40); u32le(2); u32le(2); u16le(1); u16le(24); u32le(0);
    u32le(row_bytes * 2); u32le(2835); u32le(2835); u32le(0); u32le(0);
    // bottom row first, B,G,R order
    const u8 rows[2][8] = {{90, 80, 70, 120, 110, 100, 0, 0},
                           {30, 20, 10, 60, 50, 40, 0, 0}};
    for (const auto& row : rows) f.insert(f.end(), row, row + 8);
    return f;
}

} // namespace

class WicTest : public ::testing::Test {
protected:
    static constexpr GuestAddr DATA = 0x00100000;
    static constexpr GuestAddr STACK = 0x00200000;
    static constexpr GuestAddr COM_BASE = ComCoreManager::COM_SCRATCH_BASE;
    static constexpr GuestAddr CLSID_AT = DATA;
    static constexpr GuestAddr IID_AT = DATA + 0x20;
    static constexpr GuestAddr PATH_AT = DATA + 0x100;
    static constexpr GuestAddr GUID_AT = DATA + 0x200;
    static constexpr GuestAddr OUT_AT = DATA + 0x300;
    static constexpr GuestAddr BUF_AT = DATA + 0x1000;

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(DATA, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(COM_BASE, ComCoreManager::COM_SCRATCH_SIZE, MemoryProtection::ReadWrite).is_ok());
        cpu_.init_long_mode();
        cpu_.gpr.rsp = STACK + 0x400;
        ASSERT_TRUE(registry_.register_dll("ole32.dll", create_com_core_thunks(mgr_)).is_ok());
        mgr_.set_thunk_registry(&registry_);
        // CoCreateInstance needs an apartment (measured: CO_E_NOTINITIALIZED
        // otherwise, s7_ole_probe.expected).
        (void)mgr_.co_initialize();

        char temp_template[] = "/tmp/vitreo_wic_test_XXXXXX";
        char* temp_dir = ::mkdtemp(temp_template);
        ASSERT_NE(temp_dir, nullptr);
        root_ = temp_dir;
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root_)).is_ok());
        mgr_.set_vfs(&vfs_);

        const auto bmp = make_bmp();
        std::ofstream(root_ + "/splash.bmp", std::ios::binary)
            .write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        std::ofstream(root_ + "/notes.txt") << "not an image";
    }
    void TearDown() override { std::filesystem::remove_all(root_); }

    void write_guid(GuestAddr at, const Guid& g) {
        ASSERT_TRUE(memory_.write_bytes(at, reinterpret_cast<const Byte*>(&g), sizeof(g)).is_ok());
    }
    void write_wide(GuestAddr at, const std::string& s) {
        for (usize i = 0; i <= s.size(); ++i) {
            ASSERT_TRUE(memory_.write_u16(at + i * 2, i < s.size() ? static_cast<u16>(s[i]) : 0).is_ok());
        }
    }
    /// Win64 call: this + up to 3 register args, the rest at rsp+0x28.
    Marshaler call(std::initializer_list<u64> args) {
        usize i = 0;
        for (u64 v : args) {
            if(cpu_.is_32bit()){(void)memory_.write_u32(cpu_.gpr.rsp+4+i*4,static_cast<u32>(v));++i;continue;}
            switch (i) {
            case 0: cpu_.gpr.rcx = v; break;
            case 1: cpu_.gpr.rdx = v; break;
            case 2: cpu_.gpr.r8 = v; break;
            case 3: cpu_.gpr.r9 = v; break;
            default:
                (void)memory_.write_u64(cpu_.gpr.rsp + 0x28 + (i - 4) * 8, v);
                break;
            }
            ++i;
        }
        Marshaler m{memory_};
        m.attach_cpu(cpu_);
        return m;
    }
    u64 out64(GuestAddr at = OUT_AT) { return memory_.read_u64(at).value(); }

    GuestAddr create_factory() {
        write_guid(CLSID_AT, kClsidWicImagingFactory);
        write_guid(IID_AT, kIidImagingFactory);
        auto m = call({CLSID_AT, 0, 1, IID_AT, OUT_AT});
        EXPECT_EQ(mgr_.co_create_instance(m), com::S_OK);
        return static_cast<GuestAddr>(out64());
    }
    GuestAddr create_decoder(GuestAddr factory, const std::string& path) {
        write_wide(PATH_AT, path);
        auto m = call({factory, PATH_AT, 0, 0x80000000u, 0, OUT_AT});
        last_hr_ = wic_factory_create_decoder_from_filename(mgr_, m);
        return last_hr_ == com::S_OK ? static_cast<GuestAddr>(out64()) : 0;
    }

    GuestMemory memory_;
    cpu::CpuState cpu_;
    ComCoreManager mgr_;
    ThunkRegistry registry_{0x70000000};
    capsule::Vfs vfs_;
    std::string root_;
    u32 last_hr_{0};
};

TEST_F(WicTest, FactoryDecodesABmpThroughDecoderFrameAndConverter) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    // The vtable is guest-visible and every slot is an ole32 thunk.
    const GuestAddr vtable = memory_.read_u64(factory).value();
    EXPECT_EQ(vtable, factory + 8);
    for (u32 slot = 0; slot < 29; ++slot) {
        const u64 method = memory_.read_u64(vtable + slot * 8).value();
        EXPECT_GE(method, 0x70000000ull) << "slot " << slot;
        EXPECT_LT(method, 0x80000000ull) << "slot " << slot;
    }

    const GuestAddr decoder = create_decoder(factory, "C:\\splash.bmp");
    ASSERT_NE(decoder, 0u) << std::hex << last_hr_;

    {
        auto m = call({decoder, OUT_AT});
        ASSERT_EQ(wic_decoder_get_frame_count(mgr_, m), com::S_OK);
        EXPECT_EQ(memory_.read_u32(OUT_AT).value(), 1u);
    }
    {
        auto m = call({decoder, 1, OUT_AT});
        EXPECT_EQ(wic_decoder_get_frame(mgr_, m), kWincodecFrameMissing)
            << "a BMP has exactly one frame";
    }
    auto m_frame = call({decoder, 0, OUT_AT});
    ASSERT_EQ(wic_decoder_get_frame(mgr_, m_frame), com::S_OK);
    const GuestAddr frame = static_cast<GuestAddr>(out64());
    ASSERT_NE(frame, 0u);

    // The frame reports the file's native format and size.
    {
        auto m = call({frame, GUID_AT});
        ASSERT_EQ(wic_source_get_pixel_format(mgr_, m), com::S_OK);
        Guid got{};
        ASSERT_TRUE(memory_.read_bytes(GUID_AT, reinterpret_cast<Byte*>(&got), sizeof(got)).is_ok());
        const Guid want = pixel_format(kPf24bppBGR);
        EXPECT_EQ(std::memcmp(&got, &want, sizeof(Guid)), 0);
    }
    {
        auto m = call({frame, OUT_AT, OUT_AT + 4});
        ASSERT_EQ(wic_source_get_size(mgr_, m), com::S_OK);
        EXPECT_EQ(memory_.read_u32(OUT_AT).value(), 2u);
        EXPECT_EQ(memory_.read_u32(OUT_AT + 4).value(), 2u);
    }
    // Native 24bppBGR pixels, top-down, BGR byte order.
    {
        auto m = call({frame, 0, 6, 12, BUF_AT});
        ASSERT_EQ(wic_source_copy_pixels(mgr_, m), com::S_OK);
        std::array<u8, 12> px{};
        ASSERT_TRUE(memory_.read_bytes(BUF_AT, reinterpret_cast<Byte*>(px.data()), px.size()).is_ok());
        const std::array<u8, 12> want{30, 20, 10, 60, 50, 40, 90, 80, 70, 120, 110, 100};
        EXPECT_EQ(px, want);
    }
    // Learn IWICBitmapSource::CopyPixels: a buffer shorter than the rows at
    // the given stride is refused whole, never partially written.
    {
        auto m = call({frame, 0, 6, 11, BUF_AT});
        EXPECT_EQ(wic_source_copy_pixels(mgr_, m), kWincodecInsufficientBuffer);
    }

    // The frame is also an IWICBitmapSource.
    {
        write_guid(IID_AT, kIidBitmapSource);
        auto m = call({frame, IID_AT, OUT_AT});
        ASSERT_EQ(wic_query_interface(mgr_, m), com::S_OK);
        EXPECT_EQ(out64(), frame);
        auto rel = call({frame});
        EXPECT_EQ(wic_release(mgr_, rel), 1u);
    }

    // Convert to 32bppBGR, the format an engine feeds to CreateDIBSection.
    auto m_conv = call({factory, OUT_AT});
    ASSERT_EQ(wic_factory_create_format_converter(mgr_, m_conv), com::S_OK);
    const GuestAddr converter = static_cast<GuestAddr>(out64());
    ASSERT_NE(converter, 0u);
    {
        // Learn: a converter has no pixels until Initialize.
        auto m = call({converter, 0, 8, 16, BUF_AT});
        EXPECT_EQ(wic_source_copy_pixels(mgr_, m), kWincodecNotInitialized);
    }
    {
        write_guid(GUID_AT, pixel_format(kPf8bppIndexed));
        auto m = call({converter, frame, GUID_AT, 0, 0, 0, 0});
        EXPECT_EQ(wic_converter_initialize(mgr_, m), kWincodecUnsupportedPixelFormat);
    }
    {
        write_guid(GUID_AT, pixel_format(kPf32bppBGR));
        auto m = call({converter, frame, GUID_AT, 0, 0, 0, 0});
        ASSERT_EQ(wic_converter_initialize(mgr_, m), com::S_OK);
    }
    {
        auto m = call({converter, GUID_AT});
        ASSERT_EQ(wic_source_get_pixel_format(mgr_, m), com::S_OK);
        Guid got{};
        ASSERT_TRUE(memory_.read_bytes(GUID_AT, reinterpret_cast<Byte*>(&got), sizeof(got)).is_ok());
        const Guid want = pixel_format(kPf32bppBGR);
        EXPECT_EQ(std::memcmp(&got, &want, sizeof(Guid)), 0);
    }
    {
        auto m = call({converter, 0, 8, 16, BUF_AT});
        ASSERT_EQ(wic_source_copy_pixels(mgr_, m), com::S_OK);
        std::array<u8, 16> px{};
        ASSERT_TRUE(memory_.read_bytes(BUF_AT, reinterpret_cast<Byte*>(px.data()), px.size()).is_ok());
        const std::array<u8, 16> want{30, 20, 10, 0xFF, 60, 50, 40, 0xFF,
                                      90, 80, 70, 0xFF, 120, 110, 100, 0xFF};
        EXPECT_EQ(px, want);
    }
    // A WICRect selects a sub-rectangle; a wider stride leaves padding alone.
    {
        ASSERT_TRUE(memory_.write_u32(DATA + 0x400, 1).is_ok()); // x
        ASSERT_TRUE(memory_.write_u32(DATA + 0x404, 1).is_ok()); // y
        ASSERT_TRUE(memory_.write_u32(DATA + 0x408, 1).is_ok()); // w
        ASSERT_TRUE(memory_.write_u32(DATA + 0x40C, 1).is_ok()); // h
        auto m = call({converter, DATA + 0x400, 16, 16, BUF_AT + 0x100});
        ASSERT_EQ(wic_source_copy_pixels(mgr_, m), com::S_OK);
        std::array<u8, 4> px{};
        ASSERT_TRUE(memory_.read_bytes(BUF_AT + 0x100, reinterpret_cast<Byte*>(px.data()), px.size()).is_ok());
        const std::array<u8, 4> want{120, 110, 100, 0xFF};
        EXPECT_EQ(px, want);
    }
    {
        write_guid(GUID_AT, pixel_format(kPf24bppBGR));
        write_guid(GUID_AT + 0x10, pixel_format(kPf32bppRGBA));
        auto m = call({converter, GUID_AT, GUID_AT + 0x10, OUT_AT});
        ASSERT_EQ(wic_converter_can_convert(mgr_, m), com::S_OK);
        EXPECT_EQ(memory_.read_u32(OUT_AT).value(), 1u);
    }
}

TEST_F(WicTest, DecoderReportsMissingFilesAndForeignContainersHonestly) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    EXPECT_EQ(create_decoder(factory, "C:\\absent.bmp"), 0u);
    EXPECT_EQ(last_hr_, kHrFileNotFound);
    // Learn: a file no installed codec claims is WINCODEC_ERR_COMPONENTNOTFOUND,
    // which is a failure the caller can distinguish from a missing file.
    EXPECT_EQ(create_decoder(factory, "C:\\notes.txt"), 0u);
    EXPECT_EQ(last_hr_, kWincodecComponentNotFound);
}

TEST_F(WicTest, ObjectsAnswerQueryInterfaceForTheirOwnInterfacesOnly) {
    const GuestAddr factory = create_factory();
    const GuestAddr decoder = create_decoder(factory, "C:\\splash.bmp");
    ASSERT_NE(decoder, 0u);
    write_guid(IID_AT, kIidBitmapDecoder);
    auto ok = call({decoder, IID_AT, OUT_AT});
    EXPECT_EQ(wic_query_interface(mgr_, ok), com::S_OK);
    EXPECT_EQ(out64(), decoder);
    write_guid(IID_AT, kIidImagingFactory);
    auto no = call({decoder, IID_AT, OUT_AT});
    EXPECT_EQ(wic_query_interface(mgr_, no), static_cast<u32>(0x80004002u));
    EXPECT_EQ(out64(), 0u);
    auto rel = call({decoder});
    EXPECT_EQ(wic_release(mgr_, rel), 1u);
    auto rel2 = call({decoder});
    EXPECT_EQ(wic_release(mgr_, rel2), 0u);
}

TEST_F(WicTest, ConvertBitmapSourceReturnsSameFormatSourceOrAConverter) {
    // Learn, WICConvertBitmapSource: a source already in dstFormat is handed
    // back with a reference added; otherwise a dstFormat converter wraps it.
    const GuestAddr factory = create_factory();
    const GuestAddr decoder = create_decoder(factory, "C:\\splash.bmp");
    ASSERT_NE(decoder, 0u);
    auto m_frame = call({decoder, 0, OUT_AT});
    ASSERT_EQ(wic_decoder_get_frame(mgr_, m_frame), com::S_OK);
    const GuestAddr frame = static_cast<GuestAddr>(out64());

    write_guid(GUID_AT, pixel_format(kPf24bppBGR));
    {
        auto m = call({GUID_AT, frame, OUT_AT});
        ASSERT_EQ(wic_convert_bitmap_source(mgr_, m), com::S_OK);
        EXPECT_EQ(out64(), frame);
        auto rel = call({frame});
        EXPECT_EQ(wic_release(mgr_, rel), 1u) << "the extra reference was added";
    }
    write_guid(GUID_AT, pixel_format(kPf32bppRGBA));
    {
        auto m = call({GUID_AT, frame, OUT_AT});
        ASSERT_EQ(wic_convert_bitmap_source(mgr_, m), com::S_OK);
        const GuestAddr converted = static_cast<GuestAddr>(out64());
        ASSERT_NE(converted, frame);
        auto px = call({converted, GUID_AT + 0x40});
        ASSERT_EQ(wic_source_get_pixel_format(mgr_, px), com::S_OK);
        Guid got{};
        ASSERT_TRUE(memory_.read_bytes(GUID_AT + 0x40, reinterpret_cast<Byte*>(&got), sizeof(got)).is_ok());
        const Guid want = pixel_format(kPf32bppRGBA);
        EXPECT_EQ(std::memcmp(&got, &want, sizeof(Guid)), 0);
    }
    write_guid(GUID_AT, pixel_format(kPf8bppIndexed));
    {
        (void)memory_.write_u64(OUT_AT, 0xBAD);
        auto m = call({GUID_AT, frame, OUT_AT});
        EXPECT_EQ(wic_convert_bitmap_source(mgr_, m), kWincodecUnsupportedPixelFormat);
        EXPECT_EQ(out64(), 0u);
    }
    {
        (void)memory_.write_u64(OUT_AT, 0xBAD);
        auto m = call({GUID_AT, 0, OUT_AT});
        EXPECT_EQ(wic_convert_bitmap_source(mgr_, m), 0x80070057u);
        EXPECT_EQ(out64(), 0u);
        auto m2 = call({GUID_AT, frame, 0});
        EXPECT_EQ(wic_convert_bitmap_source(mgr_, m2), 0x80070057u);
    }
}

class WicStreamTest : public WicTest, public ::testing::WithParamInterface<bool> {
protected:
    static constexpr GuestAddr STREAM=DATA+0x3000,TABLE=STREAM+0x20;
    u64 cursor=0;u32 refs=1,read_error=0,seek_error=0,stat_error=0;std::vector<u8> bytes;
    std::function<void()> on_read;
    u64 fail_at=~u64(0);
    std::vector<std::pair<u64,u32>> reads;
    void SetUp() override {
      WicTest::SetUp();
      if(!GetParam()){cpu_.init_protected_mode();cpu_.gpr.rsp=STACK+0x400;}
      bytes.assign(std::begin(wic_stream_probe_image),std::end(wic_stream_probe_image));
      auto m=call({});ASSERT_TRUE(m.write_ptr(STREAM,TABLE).is_ok());
      for(u32 slot=0;slot<14;++slot)ASSERT_TRUE(m.write_ptr(TABLE+slot*(GetParam()?8:4),0x5000+slot).is_ok());
      mgr_.set_guest_call([this](GuestAddr function,const u64* args,usize count,bool is64)->u64{
        EXPECT_EQ(is64,GetParam());EXPECT_EQ(args[0],STREAM);
        if(function==0x5001)return ++refs;
        if(function==0x5002)return --refs;
        if(function==0x5005){if(seek_error)return seek_error;EXPECT_EQ(count,is64?4u:5u);const u64 move=is64?args[1]:args[1]|(args[2]<<32);const u32 origin=args[is64?2:3];cursor=origin==0?move:cursor+move;const auto out=args[is64?3:4];if(out)(void)memory_.write_u64(out,cursor);return 0;}
        if(function==0x5000+IStream_Stat){if(stat_error)return stat_error;(void)memory_.write_u64(args[1]+(is64?StatSizeOffset64:StatSizeOffset32),bytes.size());return 0;}
        if(function==0x5003){if(on_read){auto callback=std::move(on_read);on_read={};callback();}if(read_error)return read_error;if(cursor+args[2]>fail_at)return STG_E_READFAULT;reads.push_back({cursor,args[2]});const auto size=std::min<u64>(args[2],bytes.size()-std::min<u64>(cursor,bytes.size()));(void)memory_.write_bytes(args[1],bytes.data()+std::min<u64>(cursor,bytes.size()),size);(void)memory_.write_u32(args[3],size);cursor+=size;return size==args[2]?0:1;}
        ADD_FAILURE()<<"unexpected stream callback "<<function;return 0x80004001;
      });
    }
    GuestAddr output(){auto m=call({});return m.read_ptr(OUT_AT).value();}
    u32 create(GuestAddr factory,u32 mode=0,GuestAddr vendor=0,GuestAddr stream=STREAM,GuestAddr out=OUT_AT){auto m=call({factory,stream,vendor,mode,out});return wic_factory_create_decoder_from_stream(mgr_,m);}
    void release(GuestAddr object){auto m=call({object});EXPECT_EQ(wic_release(mgr_,m),0u);}
};
TEST_P(WicStreamTest, NativePngBothCacheModesAndFrameOwnStream) {
 for(u32 mode:{0u,1u}){
  const auto factory=create_factory();cursor=0;reads.clear();
  ASSERT_EQ(create(factory,mode),0u);const auto decoder=output();EXPECT_EQ(refs,2u);EXPECT_EQ(cursor,mode?72u:41u);
  for(auto [offset,size]:reads)EXPECT_FALSE(offset>=41&&offset<64)<<offset;
  auto framecall=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,framecall),0u);const auto frame=output();
  release(decoder);EXPECT_EQ(refs,2u);
  std::array<u8,32> pixels;pixels.fill(0x5a);ASSERT_TRUE(memory_.write_bytes(BUF_AT,pixels.data(),pixels.size()).is_ok());
  auto copy=call({frame,0,16,32,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,copy),0u);
  EXPECT_EQ(cursor,72u);
  ASSERT_TRUE(memory_.read_bytes(BUF_AT,pixels.data(),pixels.size()).is_ok());
  // Windows wic_stream_decode_probe{32,64}.expected, including untouched padding.
  const std::array<u8,32> expected={0,0,255,255,0,255,0,255,90,90,90,90,90,90,90,90,255,0,0,255,255,255,255,128,90,90,90,90,90,90,90,90};
  EXPECT_EQ(pixels,expected);
  auto new_converter=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_format_converter(mgr_,new_converter),0u);const auto converter=output();
  write_guid(GUID_AT,pixel_format(0x10));auto initialize=call({converter,frame,GUID_AT,0,0,0,0});ASSERT_EQ(wic_converter_initialize(mgr_,initialize),0u);
  auto premul=call({converter,0,8,16,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,premul),0u);
  const std::array<u8,16> expected_premul={0,0,255,255,0,255,0,255,255,0,0,255,128,128,128,128};std::array<u8,16> converted;
  ASSERT_TRUE(memory_.read_bytes(BUF_AT,converted.data(),converted.size()).is_ok());EXPECT_EQ(converted,expected_premul);release(converter);
  release(frame);EXPECT_EQ(refs,1u);release(factory);
 }
}
TEST_P(WicStreamTest, FactoryValidationVendorModesAndCursorOracle) {
 const auto factory=create_factory();
 for(u32 mode:{0u,1u,2u,3u,0x7fffffffu,0xffffffffu})for(u64 pos:{0u,7u,76u})for(u32 vendor=0;vendor<3;++vendor){
   cursor=pos;write_guid(GUID_AT,vendor==1?Guid{0xf0e749ca,0xedef,0x4589,{0xa7,0x3a,0xee,0xe,0x62,0x6a,0x2a,0x2b}}:Guid{});
   auto init=call({});ASSERT_TRUE(init.write_ptr(OUT_AT,0x12345678).is_ok());
   const u32 hr=create(factory,mode,vendor?GUID_AT:0);EXPECT_EQ(hr,pos==7?0x88982f50u:0u);
   if(pos==7){EXPECT_EQ(output(),0x12345678u);EXPECT_EQ(cursor,7u);EXPECT_EQ(refs,1u);}else{EXPECT_EQ(cursor,mode?72u:41u);release(output());EXPECT_EQ(refs,1u);}
 }
 for(u32 invalid=0;invalid<4;++invalid){auto init=call({});ASSERT_TRUE(init.write_ptr(OUT_AT,0x12345678).is_ok());cursor=0;read_error=invalid==2?0x8003001e:0;seek_error=invalid==3?0x80030019:0;
 EXPECT_EQ(create(factory,0,0,invalid==0?0:STREAM,invalid==1?0:OUT_AT),invalid<2?0x80070057u:invalid==2?0x8003001eu:0x80030019u);EXPECT_EQ(output(),0x12345678u);EXPECT_EQ(refs,1u);}
 release(factory);
}
TEST_P(WicStreamTest, PixelsAreDecodedEachCopyAndErrorsPropagateAtCopy) {
 const auto factory=create_factory();ASSERT_EQ(create(factory),0u);const auto decoder=output();
 // Windows oracle: GetFrame does not read the stream; CopyPixels does.
 read_error=0x8003001e;auto framecall=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,framecall),0u);const auto frame=output();
 auto badcopy=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,badcopy),0x8003001eu);
 read_error=0;auto copy=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(memory_.read_u32(BUF_AT).value(),0xffff0000u);
 read_error=0x8003001e;auto nextcopy=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,nextcopy),0x8003001eu);
 read_error=0;
 release(decoder);release(frame);release(factory);
}
TEST_P(WicStreamTest, TruncatedHeaderFailsBeforeDecoderAndShortPayloadIsDeferred) {
 const auto factory=create_factory();
 const auto complete=bytes;
 for(u32 mode:{0u,1u})for(u32 length:{0u,1u,2u,8u,24u,33u,41u,42u,64u,72u,76u}){
  bytes=complete;bytes.resize(length);cursor=0;auto init=call({});ASSERT_TRUE(init.write_ptr(OUT_AT,0x12345678).is_ok());
  const u32 hr=create(factory,mode);
  const u32 expected=length<8?0x88982f50u:length<=41?0x88982f07u:0u;
  ASSERT_EQ(hr,expected)<<mode<<","<<length;
  if(hr){EXPECT_EQ(output(),0x12345678u);EXPECT_EQ(cursor,0u);}else{EXPECT_EQ(cursor,mode&&length==76?72u:41u);release(output());}
 }
 release(factory);
}
TEST_P(WicStreamTest, GuestReadCanReenterGetFrameWithoutAFrontendLock) {
 const auto factory=create_factory();ASSERT_EQ(create(factory),0u);const auto decoder=output();auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();
 u32 callback_count=0;fail_at=72;
 on_read=[&]{++callback_count;auto nested=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,nested),0u);release(output());};
 auto copy=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(callback_count,1u);
 release(decoder);release(frame);release(factory);
}
TEST_P(WicStreamTest, UnsupportedNativeFormatsFailLoudlyAndPreserveOutput) {
 const auto factory=create_factory();bytes[25]=0;auto init=call({});ASSERT_TRUE(init.write_ptr(OUT_AT,0x12345678).is_ok());EXPECT_EQ(create(factory),0x80004001u);EXPECT_EQ(output(),0x12345678u);EXPECT_EQ(refs,1u);
 release(factory);
}
TEST_P(WicStreamTest, ObjectNamespacesRemainIsolatedAcrossRuntimeContexts) {
 const auto factory=create_factory();const auto decoder=create_decoder(factory,"C:\\splash.bmp");ASSERT_NE(decoder,0u);
 auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();
 GuestMemory other_memory;cpu::CpuState other_cpu;ComCoreManager other;ThunkRegistry other_registry{0x71000000};
 ASSERT_TRUE(other_memory.map_region(DATA,0x10000,MemoryProtection::ReadWrite).is_ok());ASSERT_TRUE(other_memory.map_region(STACK,0x10000,MemoryProtection::ReadWrite).is_ok());ASSERT_TRUE(other_memory.map_region(COM_BASE,0x10000,MemoryProtection::ReadWrite).is_ok());
 if(GetParam())other_cpu.init_long_mode();else other_cpu.init_protected_mode();other_cpu.gpr.rsp=STACK+0x400;
 ASSERT_TRUE(other_registry.register_dll("ole32.dll",create_com_core_thunks(other)).is_ok());other.set_thunk_registry(&other_registry);
 auto other_call=[&](std::initializer_list<u64> args){usize i=0;for(u64 v:args){if(!GetParam())(void)other_memory.write_u32(other_cpu.gpr.rsp+4+4*i,v);else if(i==0)other_cpu.gpr.rcx=v;else if(i==1)other_cpu.gpr.rdx=v;else if(i==2)other_cpu.gpr.r8=v;else if(i==3)other_cpu.gpr.r9=v;else(void)other_memory.write_u64(other_cpu.gpr.rsp+0x28+8*(i-4),v);++i;}Marshaler m{other_memory};m.attach_cpu(other_cpu);return m;};
 auto foreign_size=other_call({frame,OUT_AT,OUT_AT+8});EXPECT_EQ(wic_source_get_size(other,foreign_size),0x80070057u);
 auto foreign_ref=other_call({frame});EXPECT_EQ(wic_add_ref(other,foreign_ref),0u);auto foreign_release=other_call({frame});EXPECT_EQ(wic_release(other,foreign_release),0u);
 // B creates its own valid factory at the same guest address as A's factory.
 auto create=other_call({});ASSERT_EQ(wic_co_create_instance(other,create,kIidImagingFactory,0,OUT_AT),0u);const auto other_factory=create.read_ptr(OUT_AT).value();EXPECT_EQ(other_factory,factory);
 auto other_add=other_call({other_factory});EXPECT_EQ(wic_add_ref(other,other_add),2u);auto other_rel=other_call({other_factory});EXPECT_EQ(wic_release(other,other_rel),1u);
 auto size=call({frame,OUT_AT,OUT_AT+8});EXPECT_EQ(wic_source_get_size(mgr_,size),0u);EXPECT_EQ(memory_.read_u32(OUT_AT).value(),2u);
 auto pixels=call({frame,0,8,14,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,pixels),0u);EXPECT_EQ(memory_.read_u8(BUF_AT).value(),30u);
 release(frame);release(decoder);release(factory);
}
TEST_P(WicStreamTest, ReentrantPixelDecodeFailsExplicitlyWithoutMovingTheStream) {
 const auto factory=create_factory();ASSERT_EQ(create(factory),0u);const auto decoder=output();auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();
 u32 calls=0;
 on_read=[&]{++calls;const auto before=cursor;const auto reads_before=reads.size();auto nested=call({frame,0,8,16,BUF_AT+0x100});EXPECT_EQ(wic_source_copy_pixels(mgr_,nested),0x80004001u);EXPECT_EQ(cursor,before);EXPECT_EQ(reads.size(),reads_before);};
 auto copy=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(calls,1u);EXPECT_EQ(memory_.read_u32(BUF_AT).value(),0xffff0000u);
 // A failed transaction also relinquishes its claim.
 read_error=STG_E_READFAULT;auto fail=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,fail),STG_E_READFAULT);
 read_error=0;auto retry=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,retry),0u);
 release(decoder);release(frame);release(factory);
}
TEST_P(WicStreamTest, CrossThreadPixelOverlapFailsBeforeAnyGuestStreamCall) {
 const auto factory=create_factory();ASSERT_EQ(create(factory),0u);const auto decoder=output();auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();
 u32 overlap_status=0;
 on_read=[&]{
  const auto before=cursor;const auto reads_before=reads.size();
  std::thread overlap([&]{
   cpu::CpuState worker;if(GetParam())worker.init_long_mode();else worker.init_protected_mode();worker.gpr.rsp=STACK+0x800;
   const u64 args[]={frame,0,8,16,BUF_AT+0x100};
   for(usize i=0;i<5;++i){if(!GetParam())(void)memory_.write_u32(worker.gpr.rsp+4+4*i,args[i]);else if(i==0)worker.gpr.rcx=args[i];else if(i==1)worker.gpr.rdx=args[i];else if(i==2)worker.gpr.r8=args[i];else if(i==3)worker.gpr.r9=args[i];else(void)memory_.write_u64(worker.gpr.rsp+0x28,args[i]);}
   Marshaler m{memory_};m.attach_cpu(worker);overlap_status=wic_source_copy_pixels(mgr_,m);
  });
  overlap.join();EXPECT_EQ(cursor,before);EXPECT_EQ(reads.size(),reads_before);
 };
 auto copy=call({frame,0,8,16,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(overlap_status,0x80004001u);
 release(decoder);release(frame);release(factory);
}
TEST_P(WicStreamTest, RealPaletteCustomStateAndPredefinedTables) {
 const auto factory=create_factory();auto create=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_palette(mgr_,create),0u);const auto palette=output();
 const std::array<u32,2> colors={0xff000000,0xffffffff};ASSERT_TRUE(memory_.write_bytes(BUF_AT,reinterpret_cast<const u8*>(colors.data()),sizeof(colors)).is_ok());auto initialize=call({palette,BUF_AT,2});ASSERT_EQ(wic_palette_method(mgr_,initialize,palette_InitializeCustom),0u);
 auto query=call({palette,OUT_AT});ASSERT_EQ(wic_palette_method(mgr_,query,palette_IsBlackWhite),0u);EXPECT_EQ(memory_.read_u32(OUT_AT).value(),1u);
 auto invalid=call({palette,0,1});EXPECT_EQ(wic_palette_method(mgr_,invalid,palette_InitializeCustom),0x80070057u);auto count=call({palette,OUT_AT});EXPECT_EQ(wic_palette_method(mgr_,count,palette_GetColorCount),0u);EXPECT_EQ(memory_.read_u32(OUT_AT).value(),2u);
 auto predefined=call({palette,3,1});ASSERT_EQ(wic_palette_method(mgr_,predefined,palette_InitializePredefined),0u);auto new_count=call({palette,OUT_AT});EXPECT_EQ(wic_palette_method(mgr_,new_count,palette_GetColorCount),0u);EXPECT_EQ(memory_.read_u32(OUT_AT).value(),17u);
 release(palette);release(factory);
}
TEST_P(WicStreamTest, NativeIndexedPngHasRealIndicesPaletteAndConvertedPixels) {
 for(const auto& c:indexed_oracle){if(c.wide!=GetParam())continue;bytes=c.alpha?std::vector<u8>(std::begin(wic_indexed_alpha),std::end(wic_indexed_alpha)):std::vector<u8>(std::begin(wic_indexed_packed),std::end(wic_indexed_packed));cursor=0;
  const auto factory=create_factory();ASSERT_EQ(create(factory),0u);const auto decoder=output();auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();auto format=call({frame,GUID_AT});ASSERT_EQ(wic_source_get_pixel_format(mgr_,format),0u);EXPECT_EQ(memory_.read_u8(GUID_AT+15).value(),c.alpha?GUID_WICPixelFormat8bppIndexed.Data4[7]:GUID_WICPixelFormat2bppIndexed.Data4[7]);
  std::array<u8,80> result;result.fill(0x5a);(void)memory_.write_bytes(BUF_AT,result.data(),32);auto native=call({frame,0,8,32,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,native),0u);std::array<u8,32> actual_native{};(void)memory_.read_bytes(BUF_AT,actual_native.data(),32);EXPECT_EQ(actual_native,c.native);
  const i32 rectangle[]={2,0,2,2};(void)memory_.write_bytes(PATH_AT,reinterpret_cast<const u8*>(rectangle),16);result.fill(0xa5);(void)memory_.write_bytes(BUF_AT,result.data(),8);auto roi=call({frame,PATH_AT,4,8,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,roi),0u);std::array<u8,8> actual_roi{};(void)memory_.read_bytes(BUF_AT,actual_roi.data(),8);EXPECT_EQ(actual_roi,c.region);
  auto new_palette=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_palette(mgr_,new_palette),0u);const auto palette=output();auto copy_palette=call({frame,palette});ASSERT_EQ(wic_source_copy_palette(mgr_,copy_palette),0u);auto colors=call({palette,4,BUF_AT,OUT_AT});ASSERT_EQ(wic_palette_method(mgr_,colors,palette_GetColors),0u);std::array<u32,4> actual_colors{};(void)memory_.read_bytes(BUF_AT,reinterpret_cast<u8*>(actual_colors.data()),16);EXPECT_EQ(actual_colors,c.colors);
  auto new_converter=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_format_converter(mgr_,new_converter),0u);const auto converter=output();write_guid(GUID_AT,pixel_format(0x10));auto initialize=call({converter,frame,GUID_AT,0,0,0,0});ASSERT_EQ(wic_converter_initialize(mgr_,initialize),0u);release(frame);release(decoder);
  result.fill(0x3c);(void)memory_.write_bytes(BUF_AT,result.data(),result.size());auto convert=call({converter,0,20,80,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,convert),0u);(void)memory_.read_bytes(BUF_AT,result.data(),result.size());EXPECT_EQ(result,c.converted);release(converter);release(palette);release(factory);
 }
}
TEST_P(WicStreamTest, IndexedPaletteIsCachedWhilePixelsRetainLiveStreamErrors) {
 // wic_indexed_cache{32,64}.expected: both metadata options cache PLTE/tRNS;
 // CopyPalette performs no source read, while CopyPixels propagates a later read failure.
 for (u32 mode : {0u, 1u}) {
  bytes.assign(std::begin(wic_indexed_alpha), std::end(wic_indexed_alpha));cursor=0;read_error=0;
  const auto factory=create_factory();ASSERT_EQ(create(factory,mode),0u);const auto decoder=output();
  auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();
  auto make=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_palette(mgr_,make),0u);const auto palette=output();
  for(usize i=4;i+4<bytes.size();++i)if(std::memcmp(bytes.data()+i,"PLTE",4)==0||std::memcmp(bytes.data()+i,"tRNS",4)==0)bytes[i+4]^=0xff;
  const auto before=cursor;reads.clear();read_error=STG_E_READFAULT;
  auto copy=call({frame,palette});ASSERT_EQ(wic_source_copy_palette(mgr_,copy),0u);EXPECT_EQ(cursor,before);EXPECT_TRUE(reads.empty());
  auto colors=call({palette,4,BUF_AT,OUT_AT});ASSERT_EQ(wic_palette_method(mgr_,colors,palette_GetColors),0u);
  std::array<u32,4> actual{};(void)memory_.read_bytes(BUF_AT,reinterpret_cast<u8*>(actual.data()),sizeof(actual));
  for(const auto& c:indexed_oracle)if(c.wide==GetParam()&&c.alpha)EXPECT_EQ(actual,c.colors);
  auto pixels=call({frame,0,16,64,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,pixels),STG_E_READFAULT);
  read_error=0;release(palette);release(frame);release(decoder);release(factory);EXPECT_EQ(refs,1u);
 }
}
TEST_P(WicStreamTest, AllIndexedDepthsFiltersAdam7PalettesAndCacheModesMatchWindows) {
 for(const auto& c:indexed_matrix_oracle){if(c.wide!=GetParam())continue;SCOPED_TRACE(::testing::Message()<<GetParam()<<","<<c.id<<","<<c.cache);const auto& input=indexed_matrix_images[c.id];bytes.assign(input.bytes,input.bytes+input.size);cursor=0;
  const auto factory=create_factory();ASSERT_EQ(create(factory,c.cache),0u);EXPECT_EQ(cursor,c.cursor);const auto decoder=output();auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();EXPECT_EQ(cursor,c.cursor);auto fmt=call({frame,GUID_AT});ASSERT_EQ(wic_source_get_pixel_format(mgr_,fmt),0u);EXPECT_EQ(memory_.read_u8(GUID_AT+15).value(),c.format);
  std::array<u8,200> buffer;buffer.fill(0x5a);(void)memory_.write_bytes(BUF_AT,buffer.data(),80);auto copy=call({frame,0,16,80,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(cursor,c.copy_cursor);std::array<u8,80> actual{};(void)memory_.read_bytes(BUF_AT,actual.data(),80);EXPECT_EQ(actual,c.native);
  const i32 full[]={0,0,9,5},rows[]={0,1,9,3},roi[]={1,1,7,3};
  for(u32 variant=0;variant<3;++variant){const auto* rectangle=variant==0?full:variant==1?rows:roi;(void)memory_.write_bytes(PATH_AT,reinterpret_cast<const u8*>(rectangle),16);buffer.fill(variant==2?0xa5:0x5a);(void)memory_.write_bytes(BUF_AT,buffer.data(),80);auto read=call({frame,PATH_AT,variant==2?8u:16u,variant==2?24u:80u,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,read),0u);if(variant==2){std::array<u8,24> actual_roi{};(void)memory_.read_bytes(BUF_AT,actual_roi.data(),24);EXPECT_EQ(actual_roi,c.roi);}else{(void)memory_.read_bytes(BUF_AT,actual.data(),80);EXPECT_EQ(actual,variant==0?c.full:c.rows);}}
  auto new_palette=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_palette(mgr_,new_palette),0u);const auto palette=output();auto palette_copy=call({frame,palette});ASSERT_EQ(wic_source_copy_palette(mgr_,palette_copy),0u);auto count=call({palette,OUT_AT});ASSERT_EQ(wic_palette_method(mgr_,count,palette_GetColorCount),0u);EXPECT_EQ(memory_.read_u32(OUT_AT).value(),c.count);auto colors=call({palette,7,BUF_AT,OUT_AT});ASSERT_EQ(wic_palette_method(mgr_,colors,palette_GetColors),0u);std::array<u32,7> actual_colors{};(void)memory_.read_bytes(BUF_AT,reinterpret_cast<u8*>(actual_colors.data()),c.count*sizeof(u32));EXPECT_EQ(actual_colors,c.colors);
  for(u32 premul=0;premul<2;++premul){auto converter_new=call({factory,OUT_AT});ASSERT_EQ(wic_factory_create_format_converter(mgr_,converter_new),0u);const auto converter=output();write_guid(GUID_AT,pixel_format(premul?0x10:0x0f));auto initialize=call({converter,frame,GUID_AT,0,0,0,0});ASSERT_EQ(wic_converter_initialize(mgr_,initialize),0u);buffer.fill(0x3c);(void)memory_.write_bytes(BUF_AT,buffer.data(),buffer.size());auto read=call({converter,0,40,200,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,read),0u);(void)memory_.read_bytes(BUF_AT,buffer.data(),buffer.size());EXPECT_EQ(buffer,premul?c.premul:c.bgra);release(converter);}
  release(palette);release(frame);release(decoder);release(factory);EXPECT_EQ(refs,1u);
 }
}
TEST_P(WicStreamTest, JpegTransportCursorFailuresAndLargeBufferedInputMatchWindows) {
 for(const auto& c:jpeg_transport_cases){if(c.wide!=GetParam())continue;SCOPED_TRACE(::testing::Message()<<c.image<<","<<c.cache<<","<<c.action);
  const bool gray=std::strcmp(c.image,"gray")==0,exif=std::strcmp(c.image,"exif")==0;
  const u8* data=gray?wic_jpeg_gray:exif?wic_jpeg_exif:wic_jpeg_rgb;usize size=gray?sizeof(wic_jpeg_gray):exif?sizeof(wic_jpeg_exif):sizeof(wic_jpeg_rgb);
  if(c.action==3)size=10;else if(c.action==4)size-=2;
  bytes.assign(data,data+size);
  if(c.action==6||c.action==7){std::vector<u8> padded(bytes.begin(),bytes.begin()+2);for(u32 block=0;block<3;++block){padded.insert(padded.end(),{0xff,0xfe,0xff,0xff});padded.insert(padded.end(),65533,65);}padded.insert(padded.end(),bytes.begin()+2,bytes.end());bytes=std::move(padded);}
  if(c.action>=8){for(usize at=0;at+12<bytes.size();++at)if(!std::memcmp(bytes.data()+at,"JFIF\0",5)){bytes[at+7]=c.action==9?2:c.action==10?0:1;bytes[at+8]=0;bytes[at+9]=c.action==11?0:c.action==10?2:72;bytes[at+10]=0;bytes[at+11]=c.action==10?1:144;break;}}
  cursor=c.action==1?bytes.size():c.action==2?7:0;read_error=0;const auto factory=create_factory();auto init=call({});(void)init.write_ptr(OUT_AT,0x12345678);
  ASSERT_EQ(create(factory,c.cache),c.hr);EXPECT_EQ(cursor,c.cursor);
  if(c.hr){EXPECT_EQ(output(),0x12345678u);}else{const auto decoder=output();if(c.action==5||c.action==7)read_error=STG_E_READFAULT;auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();auto resolution=call({frame,OUT_AT,OUT_AT+8});ASSERT_EQ(wic_source_get_resolution(mgr_,resolution),0u);double dx=0,dy=0;(void)memory_.read_bytes(OUT_AT,reinterpret_cast<u8*>(&dx),8);(void)memory_.read_bytes(OUT_AT+8,reinterpret_cast<u8*>(&dy),8);EXPECT_NEAR(dx,c.dpi_x,0.00005);EXPECT_NEAR(dy,c.dpi_y,0.00005);auto copy=call({frame,0,24,128,BUF_AT});EXPECT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(cursor,c.cursor);read_error=0;release(frame);release(decoder);}
  EXPECT_EQ(refs,c.finalrefs);release(factory);
 }
}
TEST_P(WicStreamTest, JpegNativeMetadataBufferedSourceAndRealPixels) {
 struct Input{const u8*data;usize size;bool gray;double dpi;};
 const Input inputs[]={{wic_jpeg_rgb,sizeof(wic_jpeg_rgb),false,96},{wic_jpeg_gray,sizeof(wic_jpeg_gray),true,96},{wic_jpeg_exif,sizeof(wic_jpeg_exif),false,72}};
 for(const auto& input:inputs)for(u32 mode:{0u,1u}){
  bytes.assign(input.data,input.data+input.size);cursor=0;read_error=0;const auto factory=create_factory();ASSERT_EQ(create(factory,mode),0u);const auto decoder=output();EXPECT_EQ(cursor,input.size);
  read_error=STG_E_READFAULT;auto get=call({decoder,0,OUT_AT});ASSERT_EQ(wic_decoder_get_frame(mgr_,get),0u);const auto frame=output();release(decoder);
  auto fmt=call({frame,GUID_AT});ASSERT_EQ(wic_source_get_pixel_format(mgr_,fmt),0u);EXPECT_EQ(memory_.read_u8(GUID_AT+15).value(),input.gray?8u:12u);
  auto resolution=call({frame,OUT_AT,OUT_AT+8});ASSERT_EQ(wic_source_get_resolution(mgr_,resolution),0u);double dpi=0;(void)memory_.read_bytes(OUT_AT,reinterpret_cast<u8*>(&dpi),sizeof(dpi));EXPECT_EQ(dpi,input.dpi);
  std::array<u8,128> data;data.fill(0x5a);(void)memory_.write_bytes(BUF_AT,data.data(),data.size());auto copy=call({frame,0,24,128,BUF_AT});ASSERT_EQ(wic_source_copy_pixels(mgr_,copy),0u);EXPECT_EQ(cursor,input.size);(void)memory_.read_bytes(BUF_AT,data.data(),data.size());
  // Native captures specify exact row footprints/padding; lossy JPEG pixel
  // values are the actual ImageIO algorithm, not claimed Windows-byte-equal.
  const usize rowbytes=input.gray?4:12;for(usize y=0;y<4;++y)for(usize x=rowbytes;x<24;++x)EXPECT_EQ(data[y*24+x],0x5a);
  EXPECT_NE(data[0],data[3*24]);read_error=0;release(frame);release(factory);EXPECT_EQ(refs,1u);
 }
}
INSTANTIATE_TEST_SUITE_P(BothGuestWidths,WicStreamTest,::testing::Bool());
