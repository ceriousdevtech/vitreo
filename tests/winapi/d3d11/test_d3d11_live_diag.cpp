// SPDX-License-Identifier: LGPL-2.1-only
//
// Live-run diagnostics: the /tmp/vitreo-d3d11-caps marker.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_live_diag.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// /tmp/vitreo-d3d11-caps: every capability query a guest makes and every
// answer, in a fixed greppable form; the marker absent logs nothing and the
// answers are the same either way (log only).
TEST_F(D3D11LiveDiagTest, CapsMarkerLogsEveryQueryAndItsAnswer) {
    const std::string prefix = pid_prefix("d3d11-caps");
    live_diag::caps_marker().set_path(dir.write("caps", ""));

    // CheckFeatureSupport(D3D11_OPTIONS): the 56 bytes the guest got.
    ASSERT_EQ(device_call(33, {5, A6_ARG, 56}), d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckFeatureSupport feature=5 name=D3D11_OPTIONS "
                       "size=56 hr=0x00000000 data=" + hex_of(A6_ARG, 56));
    const std::string options_logged = log.lines[0];

    // FORMAT_SUPPORT2 for DXGI_FORMAT_R8G8B8A8_UNORM (28): InFormat, then the
    // mask, which the line repeats as format= / support=.
    log.lines.clear();
    write_words(A6_ARG, {28, 0});
    ASSERT_EQ(device_call(33, {3, A6_ARG, 8}), d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckFeatureSupport feature=3 name=FORMAT_SUPPORT2 "
                       "size=8 hr=0x00000000 format=28 support=" +
                  hex8(memory.read_u32(A6_ARG + 4).value()) +
                  " data=" + hex_of(A6_ARG, 8));

    // A refused query is logged with its HRESULT: an unknown feature, and a
    // wrong size for a known one.
    log.lines.clear();
    write_words(A6_ARG, {0x11111111u});
    ASSERT_EQ(device_call(33, {64, A6_ARG, 4}), d3d11::E_INVALIDARG);
    ASSERT_EQ(device_call(33, {5, A6_ARG, 8}), d3d11::E_INVALIDARG);
    ASSERT_EQ(log.lines.size(), 2u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckFeatureSupport feature=64 name=? size=4 "
                       "hr=0x80070057 data=11111111");
    EXPECT_EQ(log.lines[1],
              prefix + "CheckFeatureSupport feature=5 name=D3D11_OPTIONS "
                       "size=8 hr=0x80070057 data=" + hex_of(A6_ARG, 8));

    // CheckFormatSupport.
    log.lines.clear();
    ASSERT_EQ(device_call(29, {28, A6_OUT}), d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckFormatSupport format=28 hr=0x00000000 support=" +
                  hex8(memory.read_u32(A6_OUT).value()));

    // CheckMultisampleQualityLevels, and the Device2 form with its flags.
    log.lines.clear();
    ASSERT_EQ(device_call(30, {28, 4, A6_OUT}), d3d11::S_OK);
    const u32 levels = memory.read_u32(A6_OUT).value();
    ASSERT_EQ(device_call(53, {28, 4, 0, A6_OUT}), d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 2u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckMultisampleQualityLevels format=28 samples=4 "
                       "hr=0x00000000 levels=" + std::to_string(levels));
    EXPECT_EQ(log.lines[1],
              prefix + "CheckMultisampleQualityLevels1 format=28 samples=4 "
                       "flags=0x0 hr=0x00000000 levels=" +
                  std::to_string(memory.read_u32(A6_OUT).value()));

    // The marker gone: nothing is logged, and the answer is the same.
    live_diag::caps_marker().set_path(dir.missing("absent"));
    log.lines.clear();
    for (u32 i = 0; i < 56; i += 4) {
        ASSERT_TRUE(memory.write_u32(A6_ARG + i, 0xCDCDCDCDu).is_ok());
    }
    ASSERT_EQ(device_call(33, {5, A6_ARG, 56}), d3d11::S_OK);
    ASSERT_EQ(device_call(29, {28, A6_OUT}), d3d11::S_OK);
    ASSERT_EQ(device_call(30, {28, 4, A6_OUT}), d3d11::S_OK);
    EXPECT_TRUE(log.lines.empty());
    EXPECT_EQ(options_logged.substr(options_logged.find("data=") + 5),
              hex_of(A6_ARG, 56))
        << "the capability answer must not depend on the marker";
}

// D3D11CreateDevice and D3D11CreateDeviceAndSwapChain: one line per call with
// the levels asked for, the level chosen, the flags and the result.
TEST_F(D3D11LiveDiagTest, CapsMarkerLogsEveryDeviceCreation) {
    const std::string prefix = pid_prefix("d3d11-caps");
    live_diag::caps_marker().set_path(dir.write("caps", ""));

    // {11_1, 11_0}: the device runs 11_0.
    ASSERT_TRUE(memory.write_u32(LEVELS, d3d11::FEATURE_LEVEL_11_1).is_ok());
    ASSERT_TRUE(memory.write_u32(LEVELS + 4, d3d11::FEATURE_LEVEL_11_0).is_ok());
    prepare_create();
    set_arg(3, 0x20);  // D3D11_CREATE_DEVICE_BGRA_SUPPORT
    set_arg(4, LEVELS);
    set_arg(5, 2);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(m);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const u32 chosen = memory.read_u32(LEVEL_OUT).value();
    EXPECT_EQ(chosen, d3d11::FEATURE_LEVEL_11_0);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "D3D11CreateDevice driver_type=1 flags=0x20 "
                       "requested_fl=0xb100,0xb000 chosen_fl=0xb000 "
                       "hr=0x00000000");

    // No level array: the default list.
    log.lines.clear();
    prepare_create();
    d3d11.impl_D3D11CreateDevice(m);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "D3D11CreateDevice driver_type=1 flags=0x0 "
                       "requested_fl=default chosen_fl=0xb000 hr=0x00000000");

    // A level the runtime does not know: refused, nothing chosen.
    log.lines.clear();
    ASSERT_TRUE(memory.write_u32(LEVELS, 0x1234).is_ok());
    prepare_create();
    set_arg(4, LEVELS);
    set_arg(5, 1);
    d3d11.impl_D3D11CreateDevice(m);
    ASSERT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "D3D11CreateDevice driver_type=1 flags=0x0 "
                       "requested_fl=0x1234 chosen_fl=none hr=0x80070057");

    // The swap-chain form (no swap chain asked for: a device alone).
    log.lines.clear();
    ASSERT_TRUE(memory.write_u32(LEVELS, d3d11::FEATURE_LEVEL_10_1).is_ok());
    for (usize index = 0; index < 12; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(4, LEVELS);
    set_arg(5, 1);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(9, DEVICE_OUT);
    set_arg(10, LEVEL_OUT);
    set_arg(11, CONTEXT_OUT);
    d3d11.impl_D3D11CreateDeviceAndSwapChain(m);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "D3D11CreateDeviceAndSwapChain driver_type=1 flags=0x0 "
                       "requested_fl=0xa100 chosen_fl=0xa100 hr=0x00000000");
}

// IDXGIAdapter::GetDesc / GetDesc1 / GetDesc2 / GetDesc3: the description the
// guest was given.
TEST_F(D3D11LiveDiagTest, CapsMarkerLogsTheAdapterDescriptions) {
    const std::string prefix = pid_prefix("d3d11-caps");
    live_diag::caps_marker().set_path(dir.write("caps", ""));
    const GuestAddr adapter = dxgi.create_default_adapter();
    ASSERT_NE(adapter, 0u);

    struct Call {
        const char* api;
        void (DxgiManager::*impl)(Marshaler&);
        u32 version;
    };
    const Call calls[] = {
        {"IDXGIAdapter::GetDesc", &DxgiManager::impl_GetDesc, 0},
        {"IDXGIAdapter1::GetDesc1", &DxgiManager::impl_GetDesc1, 1},
        {"IDXGIAdapter2::GetDesc2", &DxgiManager::impl_GetDesc2, 2},
        {"IDXGIAdapter4::GetDesc3", &DxgiManager::impl_GetDesc3, 2},
    };
    for (const Call& call : calls) {
        log.lines.clear();
        set_arg(0, adapter);
        set_arg(1, A6_DATA);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        (dxgi.*call.impl)(m);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << call.api;
        ASSERT_EQ(log.lines.size(), 1u) << call.api;

        // Rebuilt from the bytes the guest holds (x64 DXGI_ADAPTER_DESC).
        std::string description;
        for (u32 i = 0; i < 128; ++i) {
            const u16 c = memory.read_u16(A6_DATA + i * 2).value();
            if (c == 0) break;
            description.push_back(static_cast<char>(c));
        }
        char expected[512];
        std::snprintf(
            expected, sizeof(expected),
            "Adapter %s description=\"%s\" vendor=0x%04x device=0x%04x "
            "subsys=0x%08x revision=%u dedicated_video=%llu "
            "dedicated_system=%llu shared_system=%llu luid=%08x:%08x",
            call.api, description.c_str(),
            memory.read_u32(A6_DATA + 256).value(),
            memory.read_u32(A6_DATA + 260).value(),
            memory.read_u32(A6_DATA + 264).value(),
            memory.read_u32(A6_DATA + 268).value(),
            static_cast<unsigned long long>(
                memory.read_u64(A6_DATA + 272).value()),
            static_cast<unsigned long long>(
                memory.read_u64(A6_DATA + 280).value()),
            static_cast<unsigned long long>(
                memory.read_u64(A6_DATA + 288).value()),
            memory.read_u32(A6_DATA + 300).value(),
            memory.read_u32(A6_DATA + 296).value());
        std::string line = prefix + expected;
        if (call.version >= 1) {
            line += " flags=0x" + [&] {
                char text[16];
                std::snprintf(text, sizeof(text), "%x",
                              memory.read_u32(A6_DATA + 304).value());
                return std::string(text);
            }();
        }
        if (call.version >= 2) {
            line += " graphics_preemption=" +
                    std::to_string(memory.read_u32(A6_DATA + 308).value()) +
                    " compute_preemption=" +
                    std::to_string(memory.read_u32(A6_DATA + 312).value());
        }
        EXPECT_EQ(log.lines[0], line) << call.api;
    }
}

// The marker is re-probed at most once a second: created after a probe it is
// not seen until the second is up, and removed it is not noticed either until
// then. (The injectable path is what the other tests use to skip the wait.)
TEST(D3D11LiveDiagMarker, IsProbedAtMostOncePerSecond) {
    TempDir dir;
    const std::string path = dir.missing("marker");
    live_diag::MarkerFile marker(path);
    EXPECT_FALSE(marker.present());  // probe 1: absent
    dir.write("marker", "ps_semantic=X max=2");
    EXPECT_FALSE(marker.present()) << "within the second: the cached answer";
    EXPECT_EQ(marker.generation(), 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(marker.present());
    EXPECT_EQ(marker.contents(), "ps_semantic=X max=2");
    const u64 generation = marker.generation();
    EXPECT_GT(generation, 0u);
    dir.remove("marker");
    EXPECT_TRUE(marker.present()) << "within the second: the cached answer";
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(marker.present());
    // Created again with other contents: a new generation.
    dir.write("marker", "max=9");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(marker.present());
    EXPECT_GT(marker.generation(), generation);
    EXPECT_EQ(marker.contents(), "max=9");
}

TEST(D3D11LiveDiagFormats, CapsLinesAreGreppable) {
    using namespace live_diag::caps;
    const std::array<u32, 2> levels{0xB100, 0xB000};
    EXPECT_EQ(format_create_device("D3D11CreateDevice", 1, 0x20, levels, true,
                                   0xB000, 0),
              "D3D11CreateDevice driver_type=1 flags=0x20 "
              "requested_fl=0xb100,0xb000 chosen_fl=0xb000 hr=0x00000000");
    EXPECT_EQ(format_create_device("D3D11CreateDevice", 0, 0, {}, false, 0,
                                   0x887A0004u),
              "D3D11CreateDevice driver_type=0 flags=0x0 requested_fl=default "
              "chosen_fl=none hr=0x887a0004");
    const std::array<u8, 6> data{1, 0, 0, 0, 0xAB, 0xCD};
    EXPECT_EQ(format_check_feature_support(5, 56, 0, true, data),
              "CheckFeatureSupport feature=5 name=D3D11_OPTIONS size=56 "
              "hr=0x00000000 data=01000000 abcd");
    EXPECT_EQ(format_check_feature_support(5, 56, 0x80070057u, false, {}),
              "CheckFeatureSupport feature=5 name=D3D11_OPTIONS size=56 "
              "hr=0x80070057 data=null");
    // d3d11.h (Windows Kits 10.0.26100.0), D3D11_FEATURE, by value.
    EXPECT_EQ(feature_name(0), "THREADING");
    EXPECT_EQ(feature_name(14), "D3D11_OPTIONS2");
    EXPECT_EQ(feature_name(19), "D3D11_OPTIONS5");
    EXPECT_EQ(feature_name(21), "D3D11_OPTIONS6");
    EXPECT_EQ(feature_name(22), "?");
    EXPECT_EQ(format_check_format_support(87, 0x80004005u, 0),
              "CheckFormatSupport format=87 hr=0x80004005 support=0x00000000");
    EXPECT_EQ(format_check_multisample("CheckMultisampleQualityLevels1", 28, 8,
                                       true, 1, 0, 0),
              "CheckMultisampleQualityLevels1 format=28 samples=8 flags=0x1 "
              "hr=0x00000000 levels=0");
}

// The caps log reads a 32-bit guest's stdcall stack arguments too: the same
// lines, with the level array and the out-parameters 32 bits wide.
TEST_F(D3D11ManagerX86Test, CapsMarkerLogsAStdcallGuestsQueries) {
    TempDir dir;
    CapturedLog log;
    live_diag::set_log_sink(&capture_line, &log);
    live_diag::caps_marker().set_path(dir.write("caps", ""));
    struct Restore {
        ~Restore() {
            live_diag::set_log_sink(nullptr, nullptr);
            live_diag::caps_marker().set_path("/tmp/vitreo-d3d11-caps");
        }
    } restore_on_exit;
    const std::string prefix =
        "[d3d11-caps] pid=" + std::to_string(::getpid()) + " ";

    constexpr GuestAddr LEVELS_AT = DATA_BASE + 0x300;
    constexpr GuestAddr DATA = DATA_BASE + 0x800;
    ASSERT_TRUE(memory.write_u32(LEVELS_AT, d3d11::FEATURE_LEVEL_11_0).is_ok());
    for (usize index = 0; index < 10; ++index) set_arg(index, 0);
    set_arg(1, d3d11::DRIVER_TYPE_HARDWARE);
    set_arg(4, static_cast<u32>(LEVELS_AT));
    set_arg(5, 1);
    set_arg(6, d3d11::SDK_VERSION);
    set_arg(7, static_cast<u32>(DEVICE_OUT));
    set_arg(8, static_cast<u32>(LEVEL_OUT));
    set_arg(9, static_cast<u32>(CONTEXT_OUT));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "D3D11CreateDevice driver_type=1 flags=0x0 "
                       "requested_fl=0xb000 chosen_fl=0xb000 hr=0x00000000");

    const u32 device = memory.read_u32(DEVICE_OUT).value();
    log.lines.clear();
    set_arg(0, device);
    set_arg(1, 5);  // D3D11_FEATURE_D3D11_OPTIONS
    set_arg(2, static_cast<u32>(DATA));
    set_arg(3, 56);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    std::string hex;
    for (u32 i = 0; i < 56; ++i) {
        if (i != 0 && i % 4 == 0) hex += ' ';
        char byte[4];
        std::snprintf(byte, sizeof(byte), "%02x",
                      memory.read_u8(DATA + i).value());
        hex += byte;
    }
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckFeatureSupport feature=5 name=D3D11_OPTIONS "
                       "size=56 hr=0x00000000 data=" + hex);

    log.lines.clear();
    set_arg(0, device);
    set_arg(1, 28);
    set_arg(2, static_cast<u32>(DATA));
    d3d11.impl_DeviceMethod(marshaler, 29);  // CheckFormatSupport
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    char support[16];
    std::snprintf(support, sizeof(support), "0x%08x",
                  memory.read_u32(DATA).value());
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckFormatSupport format=28 hr=0x00000000 support=" +
                  support);

    log.lines.clear();
    set_arg(0, device);
    set_arg(1, 28);
    set_arg(2, 4);
    set_arg(3, static_cast<u32>(DATA));
    d3d11.impl_DeviceMethod(marshaler, 30);  // CheckMultisampleQualityLevels
    ASSERT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::S_OK);
    ASSERT_EQ(log.lines.size(), 1u);
    EXPECT_EQ(log.lines[0],
              prefix + "CheckMultisampleQualityLevels format=28 samples=4 "
                       "hr=0x00000000 levels=" +
                  std::to_string(memory.read_u32(DATA).value()));
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
