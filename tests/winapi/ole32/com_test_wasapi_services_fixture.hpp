// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_wasapi_services_fixture.hpp
/// @brief WasapiServicesTest fixture, its GUID/HRESULT constants and helpers
/// (test_com_wasapi_services/sessions/volume.cpp and the capture fixture). Test-local.

#include "com_test_wasapi_contract_fixture.hpp"

// ---------------------------------------------------------------------------
// IAudioClient::GetService service objects (Learn: IAudioClient::GetService,
// IAudioClock(2), IAudioClockAdjustment, IAudioSessionControl(2),
// ISimpleAudioVolume, IChannelAudioVolume, IAudioStreamVolume,
// IAudioSessionEvents). IIDs from mingw-w64 audioclient.h / audiopolicy.h;
// what Learn leaves open from tools/oracle_probe/wasapi_services_probe
// {32,64}.expected (Windows). The parameter selects x64 (RCX/RDX/R8/R9,
// floats in XMM) or x86 (stack).
// ---------------------------------------------------------------------------
namespace {
constexpr u32 kAudclntWrongEndpointType = 0x88890003u;
constexpr u32 kAudclntPositionStalled = 0x08890003u;
constexpr u32 kAudclntNoSingleProcess = 0x0889000Du;
constexpr u32 kENoInterface = 0x80004002u;
constexpr u32 kRpcNullRef = 0x800706F4u;  // HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER)
constexpr u32 kNotFound = 0x80070490u;    // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
constexpr u32 kStreamFlagsCrossProcess = 0x00010000u;
constexpr u32 kStreamFlagsRateAdjust = 0x00100000u;
constexpr Guid kIidUnknown{0x00000000u, 0x0000u, 0x0000u, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr Guid kIidAudioClient{0x1cb9ad4cu, 0xdbfau, 0x4c32u, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
constexpr Guid kIidAudioClient2{0x726778cdu, 0xf60au, 0x4edau, {0x82, 0xde, 0xe4, 0x76, 0x10, 0xcd, 0x78, 0xaa}};
constexpr Guid kIidAudioCaptureClient{0xc8adbd64u, 0xe71eu, 0x48a0u, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
constexpr Guid kIidAudioClock{0xcd63314fu, 0x3fbau, 0x4a1bu, {0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7}};
constexpr Guid kIidAudioClock2{0x6f49ff73u, 0x6727u, 0x49acu, {0xa0, 0x08, 0xd9, 0x8c, 0xf5, 0xe7, 0x00, 0x48}};
constexpr Guid kIidAudioClockAdjustment{0xf6e4c0a0u, 0x46d9u, 0x4fb8u, {0xbe, 0x21, 0x57, 0xa3, 0xef, 0x2b, 0x62, 0x6c}};
constexpr Guid kIidSimpleAudioVolume{0x87ce5498u, 0x68d6u, 0x44e5u, {0x92, 0x15, 0x6d, 0xa4, 0x7e, 0xf8, 0x83, 0xd8}};
constexpr Guid kIidAudioStreamVolume{0x93014887u, 0x242du, 0x4068u, {0x8a, 0x15, 0xcf, 0x5e, 0x93, 0xb9, 0x0f, 0xe3}};
constexpr Guid kIidChannelAudioVolume{0x1c158861u, 0xb533u, 0x4b30u, {0xb1, 0xcf, 0xe8, 0x53, 0xe5, 0x1c, 0x59, 0xb8}};
constexpr Guid kIidAudioSessionControl{0xf4b1a599u, 0x7266u, 0x4319u, {0xa8, 0xca, 0xe7, 0x0a, 0xcb, 0x11, 0xe8, 0xcd}};
constexpr Guid kIidAudioSessionControl2{0xbfb7ff88u, 0x7239u, 0x4fc9u, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
constexpr Guid kIidAudioSessionManager{0xbfa971f1u, 0x4d5eu, 0x40bbu, {0x93, 0x5e, 0x96, 0x70, 0x39, 0xbf, 0xbe, 0xe4}};
constexpr Guid kIidAudioSessionManager2{0x77aa99a0u, 0x1bd6u, 0x484fu, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
constexpr Guid kIidMMDevice{0xd666063fu, 0x1587u, 0x4e43u, {0x81, 0xf1, 0xb9, 0x48, 0xe8, 0x07, 0x36, 0x3f}};
// Windows Kits 10.0.26100.0 um/audioclient.h and um/mfidl.h.
constexpr Guid kIidAudioClientDuckingControl{0xC789D381u, 0xA28Cu, 0x4168u, {0xB2, 0x8F, 0xD3, 0xA8, 0x37, 0x92, 0x4D, 0xC3}};
constexpr Guid kIidMFTrustedOutput{0xD19F8E95u, 0xB126u, 0x4446u, {0x89, 0x0C, 0x5D, 0xCB, 0x7A, 0xD7, 0x14, 0x53}};
// The probe's event contexts and private session GUIDs.
constexpr Guid kContextA{0x11111111u, 0x2222u, 0x3333u, {0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55}};
constexpr Guid kContextB{0xaaaaaaaau, 0xbbbbu, 0xccccu, {0xdd, 0xdd, 0xee, 0xee, 0xee, 0xee, 0xee, 0xee}};
constexpr Guid kPrivateSession{0x5e551012u, 0x1234u, 0x4abcu, {0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08}};

[[nodiscard]] inline bool same_guid(const Guid& a, const Guid& b) {
    return std::memcmp(&a, &b, sizeof(Guid)) == 0;
}
[[nodiscard]] inline u32 fbits(float value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
}  // namespace

class WasapiServicesTest : public WasapiContractTest,
                           public ::testing::WithParamInterface<bool> {
protected:
    static constexpr GuestAddr SINK = DATA_BASE + 0xC00;
    static constexpr GuestAddr SINK2 = DATA_BASE + 0xC20;
    static constexpr GuestAddr SINK_VTABLE = DATA_BASE + 0xC40;
    static constexpr GuestAddr SINK_FN = 0x00009000u;  // slot n at SINK_FN + n * 0x10
    static constexpr GuestAddr GUID_A = DATA_BASE + 0xE00;
    static constexpr GuestAddr GUID_B = DATA_BASE + 0xE20;
    static constexpr GuestAddr SESSION_GUID = DATA_BASE + 0xE40;
    static constexpr GuestAddr PARAM_GUID = DATA_BASE + 0xE60;
    static constexpr GuestAddr TEXT = DATA_BASE + 0xF00;
    static constexpr GuestAddr OUT = DATA_BASE + 0x1000;
    static constexpr GuestAddr OUT2 = DATA_BASE + 0x1100;
    static constexpr GuestAddr ARRAY = DATA_BASE + 0x1200;

    struct SinkCall {
        GuestAddr sink{0};
        u32 slot{0};
        std::vector<u64> args;
        u32 xmm1{0};
        bool context_null{true};
        Guid context{};
        std::u16string text;
        Guid param{};
        std::vector<float> volumes;
    };

    void SetUp() override {
        WasapiContractTest::SetUp();
        x64_ = GetParam();
        if (x64_) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        const GuestAddr stride = x64_ ? 8u : 4u;
        wptr(SINK, SINK_VTABLE);
        wptr(SINK2, SINK_VTABLE);
        for (u32 slot = 0; slot < 10; ++slot) wptr(SINK_VTABLE + slot * stride, SINK_FN + slot * 0x10u);
        write_guid(GUID_A, kContextA);
        write_guid(GUID_B, kContextB);
        mgr_.set_audio_activation_thread([this](GuestAddr entry, GuestAddr parameter) {
            launches_.emplace_back(entry, parameter);
            return true;
        });
        mgr_.set_guest_call([this](GuestAddr fn, const u64* args, usize count, bool is64) -> u64 {
            EXPECT_EQ(is64, x64_);
            if (fn < SINK_FN || fn >= SINK_FN + 0x100u) {
                ADD_FAILURE() << "unexpected guest call 0x" << std::hex << fn;
                return 0;
            }
            const u32 slot = static_cast<u32>((fn - SINK_FN) / 0x10u);
            auto& refs = sink_refs_[static_cast<GuestAddr>(args[0])];
            if (slot == 1) return ++refs;
            if (slot == 2) return --refs;
            SinkCall call;
            call.sink = static_cast<GuestAddr>(args[0]);
            call.slot = slot;
            call.args.assign(args, args + count);
            call.xmm1 = cpu_.sse.xmm[1].u32_[0];
            // The arguments live in guest memory only for the call: read now.
            const usize context_index = slot == 5 ? 3 : slot == 6 ? 4 : 2;
            if (slot != 8 && context_index < count) {
                const GuestAddr context = static_cast<GuestAddr>(args[context_index]);
                call.context_null = context == 0;
                if (context != 0) call.context = read_guid(context);
            }
            if (slot == 3 || slot == 4) call.text = wstr(static_cast<GuestAddr>(args[1]));
            if (slot == 7) call.param = read_guid(static_cast<GuestAddr>(args[1]));
            if (slot == 6) {
                for (u64 i = 0; i < args[1]; ++i) {
                    call.volumes.push_back(rf(static_cast<GuestAddr>(args[2] + i * 4u)));
                }
            }
            calls_.push_back(std::move(call));
            return 0;
        });
        modules_.set_main_module(0x400000u, "C:\\Games\\probe.exe", 0, 0);
        mgr_.set_module_manager(&modules_);
    }

    void wptr(GuestAddr address, u64 value) {
        if (x64_) ASSERT_TRUE(memory_.write_u64(address, value).is_ok());
        else ASSERT_TRUE(memory_.write_u32(address, static_cast<u32>(value)).is_ok());
    }
    [[nodiscard]] u64 ptr(GuestAddr address) {
        return x64_ ? memory_.read_u64(address).value_or(0xBAD)
                    : memory_.read_u32(address).value_or(0xBAD);
    }
    [[nodiscard]] u64 r64(GuestAddr address) { return memory_.read_u64(address).value_or(0xBAD); }
    [[nodiscard]] float rf(GuestAddr address) {
        const u32 bits = memory_.read_u32(address).value_or(0);
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void wf(GuestAddr address, float value) {
        ASSERT_TRUE(memory_.write_u32(address, fbits(value)).is_ok());
    }
    [[nodiscard]] Guid read_guid(GuestAddr address) {
        Guid guid{};
        EXPECT_TRUE(memory_.read_bytes(address, reinterpret_cast<Byte*>(&guid), sizeof(guid)).is_ok());
        return guid;
    }
    [[nodiscard]] std::u16string wstr(GuestAddr address) {
        std::u16string text;
        for (GuestAddr at = address;; at += 2) {
            const u16 unit = memory_.read_u16(at).value_or(0);
            if (unit == 0) break;
            text.push_back(static_cast<char16_t>(unit));
        }
        return text;
    }
    void put_wide(GuestAddr address, std::u16string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            ASSERT_TRUE(memory_.write_u16(address + static_cast<GuestAddr>(i * 2), text[i]).is_ok());
        }
        ASSERT_TRUE(memory_.write_u16(address + static_cast<GuestAddr>(text.size() * 2), 0).is_ok());
    }

    /// Lay out a call. `float_index` names a float argument: on x64 it
    /// travels in that XMM register and its GPR holds junk.
    void put_args(std::initializer_list<u64> values, int float_index) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        int index = 0;
        for (const u64 value : values) {
            if (!x64_) {
                (void)memory_.write_u32(frame + 4 + 4 * static_cast<GuestAddr>(index),
                                        static_cast<u32>(value));
            } else if (index < 4) {
                const u64 gpr = index == float_index ? 0xBADF00D0BADF00D0ull : value;
                if (index == float_index) cpu_.sse.xmm[index].u64_[0] = value;
                if (index == 0) cpu_.gpr.rcx = gpr;
                if (index == 1) cpu_.gpr.rdx = gpr;
                if (index == 2) cpu_.gpr.r8 = gpr;
                if (index == 3) cpu_.gpr.r9 = gpr;
            } else {
                (void)memory_.write_u64(frame + 0x28 + 8 * static_cast<GuestAddr>(index - 4), value);
            }
            ++index;
        }
    }
    template <typename Fn>
    u32 invoke(Fn fn, std::initializer_list<u64> values, int float_index = -1) {
        put_args(values, float_index);
        auto m = make_marshaler();
        return (mgr_.*fn)(m);
    }

    GuestAddr open_stream(u32 flags = 0, GuestAddr session = 0, bool float_mix = false,
                          u32 rate = 48000) {
        if (float_mix) write_format(3, 2, rate, 32);
        else write_format(1, 2, rate, 16);
        auto m = make_marshaler();
        const GuestAddr client = mgr_.create_audio_client(m);
        EXPECT_NE(client, 0u);
        const u32 hr = x64_
            ? invoke(&ComCoreManager::audio_client_initialize,
                     {client, 0, flags, 400000u, 0, FORMAT_ADDR, session})
            : invoke(&ComCoreManager::audio_client_initialize,
                     {client, 0, flags, 400000u, 0, 0, 0, FORMAT_ADDR, session});
        EXPECT_EQ(hr, com::S_OK);
        return client;
    }
    std::pair<u32, u64> service(GuestAddr client, const Guid& iid) {
        write_guid(IID_ADDR, iid);
        wptr(OUT, 0x1234);
        const u32 hr = invoke(&ComCoreManager::audio_client_get_service, {client, IID_ADDR, OUT});
        return {hr, ptr(OUT)};
    }
    GuestAddr must_service(GuestAddr client, const Guid& iid) {
        auto [hr, out] = service(client, iid);
        EXPECT_EQ(hr, com::S_OK);
        EXPECT_NE(out, 0u);
        return static_cast<GuestAddr>(out);
    }
    std::pair<u32, u64> qi(GuestAddr object, const Guid& iid) {
        write_guid(IID_ADDR, iid);
        wptr(OUT, 0x1234);
        const u32 hr = invoke(&ComCoreManager::audio_service_query_interface,
                              {object, IID_ADDR, OUT});
        return {hr, ptr(OUT)};
    }
    u32 client_refs(GuestAddr client) {
        (void)invoke(&ComCoreManager::audio_client_add_ref, {client});
        return invoke(&ComCoreManager::audio_client_release, {client});
    }
    /// Run the notification thread the manager asked for, as the runtime's
    /// guest thread would, and hand back what the sinks received.
    std::vector<SinkCall> drain() {
        std::vector<std::pair<GuestAddr, GuestAddr>> pending;
        pending.swap(launches_);
        for (const auto& [entry, parameter] : pending) {
            auto handler = registry_.resolve_handler(entry);
            EXPECT_TRUE(handler.is_ok());
            if (handler.is_err()) continue;
            put_args({parameter}, -1);
            auto m = make_marshaler();
            (*handler.value())(m);
        }
        std::vector<SinkCall> out;
        out.swap(calls_);
        return out;
    }
    /// Queue `frames` 16-bit stereo frames through the stream's render client.
    void submit(GuestAddr client, std::initializer_list<i16> samples) {
        const GuestAddr render = must_service(client, kIidAudioRenderClient);
        const u32 frames = static_cast<u32>(samples.size() / 2);
        ASSERT_EQ(invoke(&ComCoreManager::audio_render_client_get_buffer, {render, frames, OUT2}),
                  com::S_OK);
        const GuestAddr data = static_cast<GuestAddr>(ptr(OUT2));
        GuestAddr at = data;
        for (const i16 sample : samples) {
            ASSERT_TRUE(memory_.write_u16(at, static_cast<u16>(sample)).is_ok());
            at += 2;
        }
        ASSERT_EQ(invoke(&ComCoreManager::audio_render_client_release_buffer, {render, frames, 0}),
                  com::S_OK);
        (void)invoke(&ComCoreManager::audio_render_client_release, {render});
    }
    TestAudioPushVoice* voice_of(GuestAddr) { return renderer_.last_push; }

    bool x64_{false};
    ModuleManager modules_{registry_};
    std::vector<std::pair<GuestAddr, GuestAddr>> launches_;
    std::vector<SinkCall> calls_;
    std::unordered_map<GuestAddr, u32> sink_refs_;
};
