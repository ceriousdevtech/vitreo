// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_wasapi_contract_fixture.hpp
/// @brief WasapiContractTest fixture and the audclnt.h constants
/// (test_com_wasapi_contract.cpp and the services fixture). Test-local.

#include "com_test_common.hpp"
#include "com_test_filter_graph_fixture.hpp"
#include "com_test_audio_doubles.hpp"

// ---------------------------------------------------------------------------
// WASAPI shared-mode render contract (Learn: IAudioClient::Initialize,
// GetMixFormat, IsFormatSupported, SetEventHandle, Start, Stop, Reset;
// IAudioRenderClient::GetBuffer / ReleaseBuffer). HRESULTs are the
// audioclient.h AUDCLNT_ERR values (FACILITY_AUDCLNT 0x889).
// ---------------------------------------------------------------------------
namespace {
constexpr u32 kAudclntNotInitialized = 0x88890001u;
constexpr u32 kAudclntAlreadyInitialized = 0x88890002u;
constexpr u32 kAudclntNotStopped = 0x88890005u;
constexpr u32 kAudclntBufferTooLarge = 0x88890006u;
constexpr u32 kAudclntOutOfOrder = 0x88890007u;
constexpr u32 kAudclntUnsupportedFormat = 0x88890008u;
constexpr u32 kAudclntInvalidSize = 0x88890009u;
constexpr u32 kAudclntBufferOperationPending = 0x8889000Bu;
constexpr u32 kAudclntExclusiveModeNotAllowed = 0x8889000Eu;
constexpr u32 kAudclntEventHandleNotExpected = 0x88890011u;
constexpr u32 kAudclntEventHandleNotSet = 0x88890014u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kStreamFlagsLoopback = 0x00020000u;
constexpr u32 kStreamFlagsEventCallback = 0x00040000u;
constexpr u32 kStreamFlagsSrcDefaultQuality = 0x08000000u;
constexpr u32 kStreamFlagsAutoConvertPcm = 0x80000000u;
constexpr u32 kBufferFlagsSilent = 0x2u;
constexpr Guid kSubtypePcm{0x00000001u, 0x0000u, 0x0010u,
                           {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kSubtypeFloat{0x00000003u, 0x0000u, 0x0010u,
                             {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kSubtypeAc3{0x00000092u, 0x0000u, 0x0010u,
                           {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid kIidAudioRenderClient{0xF294ACFCu, 0x3146u, 0x4483u,
                                     {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
}  // namespace

class WasapiContractTest : public FilterGraphTest {
protected:
    static constexpr GuestAddr FORMAT_ADDR = DATA_BASE + 0x800;
    static constexpr GuestAddr PTR_OUT = DATA_BASE + 0x900;
    static constexpr GuestAddr SIZE_OUT = DATA_BASE + 0x904;

    void SetUp() override {
        FilterGraphTest::SetUp();
        mgr_.set_audio_renderer(&renderer_);
    }

    void write_format(u16 tag, u16 channels, u32 rate, u16 bits, u16 align = 0,
                      u16 extra = 0, u16 valid = 0, u32 mask = 0,
                      const Guid& sub = kSubtypePcm) {
        if (align == 0) align = static_cast<u16>(channels * (bits / 8u));
        ASSERT_TRUE(memory_.write_u16(FORMAT_ADDR, tag).is_ok());
        ASSERT_TRUE(memory_.write_u16(FORMAT_ADDR + 2, channels).is_ok());
        ASSERT_TRUE(memory_.write_u32(FORMAT_ADDR + 4, rate).is_ok());
        ASSERT_TRUE(memory_.write_u32(FORMAT_ADDR + 8, rate * align).is_ok());
        ASSERT_TRUE(memory_.write_u16(FORMAT_ADDR + 12, align).is_ok());
        ASSERT_TRUE(memory_.write_u16(FORMAT_ADDR + 14, bits).is_ok());
        ASSERT_TRUE(memory_.write_u16(FORMAT_ADDR + 16, extra).is_ok());
        ASSERT_TRUE(memory_.write_u16(FORMAT_ADDR + 18, valid).is_ok());
        ASSERT_TRUE(memory_.write_u32(FORMAT_ADDR + 20, mask).is_ok());
        write_guid(FORMAT_ADDR + 24, sub);
    }
    void write_extensible(u16 channels, u32 rate, u16 bits, u16 valid, u32 mask,
                          const Guid& sub) {
        write_format(0xFFFEu, channels, rate, bits, 0, 22u, valid, mask, sub);
    }

    [[nodiscard]] u32 new_client() {
        auto m = make_marshaler();
        return static_cast<u32>(mgr_.create_audio_client(m));
    }
    /// x86 stack: each REFERENCE_TIME takes two slots.
    [[nodiscard]] u32 initialize(u32 client, u32 mode, u32 flags, u32 duration,
                                 GuestAddr format = FORMAT_ADDR) {
        set_args({client, mode, flags, duration, 0, 0, 0,
                  static_cast<u32>(format), 0});
        auto m = make_marshaler();
        return mgr_.audio_client_initialize(m);
    }
    template <typename Fn>
    [[nodiscard]] u32 call(std::vector<u32> args, Fn fn) {
        set_args(args);
        auto m = make_marshaler();
        return (mgr_.*fn)(m);
    }

    TestAudioRenderer renderer_;
};
