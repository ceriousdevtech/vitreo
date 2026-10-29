// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_wasapi_capture_devices.cpp
/// @brief Capture endpoint, microphone permission and DirectSound/waveIn device-list tests.
/// Owns the Bitness/WasapiCaptureTest instantiation.

#include "com_test_common.hpp"
#include "com_test_wasapi_capture_fixture.hpp"

namespace {
class NullWaveOutput final : public windowing::WaveOutput {
public:
    u32 device_count() const noexcept override { return 0; }
    bool supports(const windowing::WaveOutputFormat&) const noexcept override { return false; }
    bool open(const windowing::WaveOutputFormat&, CompletionCallback) override { return false; }
    bool is_open() const noexcept override { return false; }
    void close() override {}
    bool submit(u64, std::span<const Byte>) override { return false; }
    void reset() override {}
    void set_paused(bool) override {}
    bool set_volume(float) override { return false; }
};
}  // namespace

INSTANTIATE_TEST_SUITE_P(Bitness, WasapiCaptureTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });

TEST_P(WasapiCaptureTest, CaptureEndpointsAreEnumeratedAndDescribed) {
    const GuestAddr e = enumerator();
    // eCapture: every host input, host order; eAll: render then capture;
    // a mask without DEVICE_STATE_ACTIVE lists none ("captures-agree=1").
    EXPECT_EQ(collection_ids(e, 1), (std::vector<std::u16string>{endpoint("uid-usb"),
                                                                  endpoint("uid-builtin")}));
    const auto all = collection_ids(e, 2);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0], u"Vitreo.CoreAudio.Render");
    EXPECT_TRUE(collection_ids(e, 1, 0x2).empty());
    // "GetDefaultAudioEndpoint(eCapture, role=0..2) hr=0 in-list=1": the host default.
    for (u32 role = 0; role < 3; ++role) {
        ASSERT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_get_default_audio_endpoint,
                         {e, 1, role, OUT}), com::S_OK);
        EXPECT_EQ(device_id(static_cast<GuestAddr>(ptr(OUT))), endpoint("uid-builtin"));
    }
    const GuestAddr device = static_cast<GuestAddr>(ptr(OUT));
    // "IMMEndpoint hr=0 flow=1 state=1 id-prefix-0.0.1=1".
    write_guid(IID_ADDR, kIidMMEndpoint);
    ASSERT_EQ(invoke(&ComCoreManager::mmdevice_query_interface, {device, IID_ADDR, OUT2}),
              com::S_OK);
    const GuestAddr mmendpoint = static_cast<GuestAddr>(ptr(OUT2));
    ASSERT_EQ(invoke(&ComCoreManager::mmendpoint_get_data_flow, {mmendpoint, OUT2}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT2).value(), 1u);
    ASSERT_EQ(invoke(&ComCoreManager::mmdevice_get_state, {device, OUT2}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT2).value(), 1u);
    EXPECT_EQ(device_id(device).substr(0, 18), u"{0.0.1.00000000}.{");
    // Property store: the friendly name, and "PKEY_AudioEngine_DeviceFormat
    // hr=0 vt=65 blob=40".
    ASSERT_EQ(invoke(&ComCoreManager::mmdevice_open_property_store, {device, 0, OUT2}), com::S_OK);
    const GuestAddr store = static_cast<GuestAddr>(ptr(OUT2));
    write_guid(C_KEY, kPkeyDeviceNames);
    ASSERT_TRUE(memory_.write_u32(C_KEY + 16, 14).is_ok());
    ASSERT_EQ(invoke(&ComCoreManager::property_store_get_value, {store, C_KEY, C_VALUE}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u16(C_VALUE).value(), 31u);  // VT_LPWSTR
    EXPECT_EQ(wstr(static_cast<GuestAddr>(ptr(C_VALUE + 8))), u"MacBook Pro Microphone");
    write_guid(C_KEY, kPkeyAudioEngine);
    ASSERT_TRUE(memory_.write_u32(C_KEY + 16, 0).is_ok());
    ASSERT_EQ(invoke(&ComCoreManager::property_store_get_value, {store, C_KEY, C_VALUE}),
              com::S_OK);
    EXPECT_EQ(memory_.read_u16(C_VALUE).value(), 65u);  // VT_BLOB
    EXPECT_EQ(memory_.read_u32(C_VALUE + 8).value(), 40u);
    const GuestAddr blob = static_cast<GuestAddr>(ptr(C_VALUE + (x64_ ? 16 : 12)));
    EXPECT_EQ(memory_.read_u16(blob).value(), 0xFFFEu);
    EXPECT_EQ(memory_.read_u16(blob + 2).value(), 2u);
    EXPECT_EQ(memory_.read_u32(blob + 4).value(), 48000u);
    // GetDevice: a capture id (any case) resolves; "(unknown capture-form id)
    // hr=80070490 out=null"; "(bogus) hr=80070057 out=null".
    std::u16string upper = endpoint("uid-usb");
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char16_t c) { return c >= u'a' && c <= u'z' ? char16_t(c - 32) : c; });
    put_wide(TEXT, upper);
    ASSERT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_get_device, {e, TEXT, OUT}), com::S_OK);
    EXPECT_EQ(device_id(static_cast<GuestAddr>(ptr(OUT))), endpoint("uid-usb"));
    put_wide(TEXT, u"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000000}");
    wptr(OUT, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_get_device, {e, TEXT, OUT}), kNotFound);
    EXPECT_EQ(ptr(OUT), 0u);
    put_wide(TEXT, u"bogus");
    wptr(OUT, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_get_device, {e, TEXT, OUT}), kEInvalidArg);
    EXPECT_EQ(ptr(OUT), 0u);
    // A device that leaves the host: its endpoint reads DEVICE_STATE_NOTPRESENT.
    capture_.list.pop_back();
    ASSERT_EQ(invoke(&ComCoreManager::mmdevice_get_state, {device, OUT2}), com::S_OK);
    EXPECT_EQ(memory_.read_u32(OUT2).value(), 4u);
    // No input device at all: the default is E_NOTFOUND (Windows "no render
    // endpoint" twin: "GetDefaultAudioEndpoint(eRender) hr=80070490").
    capture_.list.clear();
    capture_.default_uid.clear();
    wptr(OUT, 0x1234);
    EXPECT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_get_default_audio_endpoint,
                     {e, 1, 0, OUT}), kNotFound);
    EXPECT_EQ(ptr(OUT), 0u);
    EXPECT_TRUE(collection_ids(e, 1).empty());
}

TEST_P(WasapiCaptureTest, MicrophonePermissionGatesInitialize) {
    // Unmeasured on Windows: E_ACCESSDENIED, the result Learn documents for
    // capture without consent (ActivateAudioInterfaceAsync), and no stream.
    write_format(3, 2, 48000, 32);
    capture_.state = audio::CapturePermission::Denied;
    EXPECT_EQ(init(capture_client(), 0, 0, 0), kEAccessDenied);
    EXPECT_EQ(capture_.opens, 0);
    capture_.state = audio::CapturePermission::Undetermined;
    capture_.answer = audio::CapturePermission::Granted;
    EXPECT_EQ(init(capture_client(), 0, 0, 0), com::S_OK);
    EXPECT_EQ(capture_.requests, 1);
    EXPECT_EQ(capture_.opens, 1);
}

TEST_P(WasapiCaptureTest, ActivateAudioInterfaceAsyncReachesCaptureDevices) {
    constexpr GuestAddr HANDLER = DATA_BASE + 0x1300, VTABLE = DATA_BASE + 0x1340;
    const GuestAddr stride = x64_ ? 8u : 4u;
    wptr(HANDLER, VTABLE);
    wptr(VTABLE + stride, 0x100);
    wptr(VTABLE + 2 * stride, 0x200);
    wptr(VTABLE + 3 * stride, 0x300);
    u32 result = 0;
    u64 activated = 0;
    GuestAddr operation = 0;
    mgr_.set_guest_call([&](GuestAddr fn, const u64* args, usize, bool) -> u64 {
        if (fn == 0x100 || fn == 0x200) return 1;
        if (fn == 0x300) {
            put_args({operation, OUT2, OUT2 + 0x10}, -1);
            auto m = make_marshaler();
            EXPECT_EQ(mgr_.audio_activation_get_result(m), com::S_OK);
            result = memory_.read_u32(OUT2).value_or(1);
            activated = ptr(OUT2 + 0x10);
            return 0;
        }
        auto handler = registry_.resolve_handler(fn);
        if (handler.is_ok()) {
            put_args({args[0]}, -1);
            auto m = make_marshaler();
            (*handler.value())(m);
        }
        return cpu_.gpr.rax;
    });
    auto activate = [&](std::u16string_view path) {
        put_wide(TEXT, path);
        write_guid(IID_ADDR, kIidAudioClient);
        result = 0xFFFFFFFFu;
        activated = 0;
        EXPECT_EQ(invoke(&ComCoreManager::activate_audio_interface_async,
                         {TEXT, IID_ADDR, 0, HANDLER, OUT}), com::S_OK);
        operation = static_cast<GuestAddr>(ptr(OUT));
        (void)drain();
        return std::make_pair(result, activated != 0);
    };
    // "activate DEVINTERFACE_AUDIO_CAPTURE result=0 object=1" (either case),
    // "SWD capture path result=0 object=1", "capture-endpoint-id
    // result=80070002", "SWD capture endpoint with render class result=80070002".
    EXPECT_EQ(activate(u"{2EEF81BE-33FA-4800-9670-1CD474972C3F}"), std::make_pair(0u, true));
    EXPECT_EQ(activate(u"{2eef81be-33fa-4800-9670-1cd474972c3f}"), std::make_pair(0u, true));
    const std::u16string id = endpoint("uid-usb");
    EXPECT_EQ(activate(u"\\\\?\\SWD#MMDEVAPI#" + id + u"#{2eef81be-33fa-4800-9670-1cd474972c3f}"),
              std::make_pair(0u, true));
    EXPECT_EQ(activate(id), std::make_pair(kFileNotFound, false));
    EXPECT_EQ(activate(u"\\\\?\\SWD#MMDEVAPI#" + id + u"#{e6327cad-dcec-4949-ae8a-991e976a79d2}"),
              std::make_pair(kFileNotFound, false));
    // Learn (GetActivateResult): E_ACCESSDENIED when the user gives no consent.
    capture_.state = audio::CapturePermission::Denied;
    EXPECT_EQ(activate(u"{2EEF81BE-33FA-4800-9670-1CD474972C3F}"),
              std::make_pair(kEAccessDenied, false));
    // No input device: the endpoint is not found.
    capture_.state = audio::CapturePermission::Granted;
    capture_.list.clear();
    capture_.default_uid.clear();
    EXPECT_EQ(activate(u"{2EEF81BE-33FA-4800-9670-1CD474972C3F}"),
              std::make_pair(kNotFound, false));
}

TEST_P(WasapiCaptureTest, DirectSoundWaveInAndMMDeviceListTheSameDevices) {
    // One host device list behind all three APIs (measured on Windows):
    // DirectSound enumerates the primary driver and then each device, default
    // first, with its endpoint id as the module and its endpoint GUID; waveIn
    // ids are default first and DRV_QUERYFUNCTIONINSTANCEID is the endpoint
    // id; MMDevice lists host order with the same ids.
    capture_.list.push_back(audio::CaptureDevice{"uid-third", u"Line In", 48000, 2});
    const GuestAddr e = enumerator();
    const auto mm = collection_ids(e, 1);
    ASSERT_EQ(mm.size(), 3u);
    ASSERT_EQ(invoke(&ComCoreManager::mmdevice_enumerator_get_default_audio_endpoint,
                     {e, 1, 0, OUT}), com::S_OK);
    const std::u16string def = device_id(static_cast<GuestAddr>(ptr(OUT)));
    std::vector<std::u16string> expected_order{def};
    for (const auto& id : mm) {
        if (id != def) expected_order.push_back(id);
    }

    DirectSoundManager ds{nullptr};
    ds.set_memory(&memory_);
    ThunkRegistry ds_registry{0x78000000};
    ds.set_thunk_registry(&ds_registry);
    ASSERT_TRUE(ds_registry.register_dll("dsound.dll", create_dsound_thunks(ds)).is_ok());
    ds.set_audio_capture(&capture_);
    ds.note_guest_bitness(x64_);
    std::vector<std::u16string> ds_modules;
    std::vector<std::array<u8, 16>> ds_guids;
    ds.set_guest_call([&](GuestAddr, const u64* args, usize, bool) {
        ds_modules.push_back(wstr(static_cast<GuestAddr>(args[2])));
        std::array<u8, 16> g{};
        if (args[0] != 0) {
            EXPECT_TRUE(memory_.read_bytes(static_cast<GuestAddr>(args[0]),
                                           reinterpret_cast<Byte*>(g.data()), 16).is_ok());
        }
        ds_guids.push_back(g);
        return u64{1};
    });
    ASSERT_EQ(ds.enumerate(0x401000, 0, true, true, x64_), 0u);
    ASSERT_EQ(ds_modules.size(), 4u);
    EXPECT_EQ(ds_modules[0], u"");
    EXPECT_EQ(std::vector<std::u16string>(ds_modules.begin() + 1, ds_modules.end()),
              expected_order);
    for (usize i = 1; i < ds_guids.size(); ++i) {
        // The DirectSound GUID is the endpoint id's GUID.
        char text[40];
        u32 d1 = 0;
        u16 d2 = 0, d3 = 0;
        std::memcpy(&d1, ds_guids[i].data(), 4);
        std::memcpy(&d2, ds_guids[i].data() + 4, 2);
        std::memcpy(&d3, ds_guids[i].data() + 6, 2);
        std::snprintf(text, sizeof(text), "%08x-%04x-%04x", d1, d2, d3);
        EXPECT_EQ(ds_modules[i].substr(18, 18), std::u16string(text, text + 18));
    }

    capsule::Vfs vfs;
    NullWaveOutput wave;
    WinmmManager winmm{memory_, wave, vfs};
    winmm.set_audio_capture(&capture_);
    const auto exports = create_winmm_thunks(winmm);
    auto call = [&](std::string_view name, std::initializer_list<u64> args) {
        const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                     [&](const ExportEntry& x) { return x.name == name; });
        EXPECT_NE(it, exports.entries.end());
        put_args(args, -1);
        auto m = make_marshaler();
        it->handler(m);
        return static_cast<u32>(cpu_.gpr.rax);
    };
    ASSERT_EQ(call("waveInGetNumDevs", {}), 3u);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_EQ(call("waveInMessage", {i, 0x811, TEXT, 512}), 0u);
        EXPECT_EQ(wstr(TEXT), expected_order[i]);
    }
    ASSERT_EQ(call("waveInMessage", {0xFFFFFFFFu, 0x2015, OUT, OUT + 4}), 0u);
    EXPECT_EQ(memory_.read_u32(OUT).value(), 0u);  // the preferred device is id 0: the default
}
