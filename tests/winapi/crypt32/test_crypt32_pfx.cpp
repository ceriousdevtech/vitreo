// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_pfx.cpp
/// @brief crypt32!PFXIsPFXBlob replayed against real-Windows results.
///
/// Every expected value comes from tests/winapi/crypt32/oracle/
/// pfx_is_pfx_blob_probe{32,64}.expected (winoracle), via the generated
/// pfx_is_pfx_blob_oracle.inc.

#include "crypt32_api_harness.hpp"

#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::test::CryptApi;

namespace {

#include "pfx_is_pfx_blob_oracle.inc"

std::vector<u8> unhex(const char* hex) {
    std::vector<u8> out;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        out.push_back(static_cast<u8>(std::stoul(std::string(p, 2), nullptr, 16)));
    }
    return out;
}

const PfxOracleCase& oracle_case(const char* name) {
    for (const auto& c : kPfxOracleCases) {
        if (std::string(c.name) == name) return c;
    }
    ADD_FAILURE() << name;
    return kPfxOracleCases[0];
}

u32 child_exit(const char* mode) {
    for (const auto& c : kPfxOracleChildren) {
        if (std::string(c.mode) == mode) return c.exit_code;
    }
    ADD_FAILURE() << mode;
    return 0;
}

constexpr u32 kAccessViolation = 0xC0000005u;

// Outcome of one call in the probe's child-process encoding.
u32 outcome(CryptApi& api, GuestAddr blob) {
    api.cpu().pending_raise = {};
    const u64 ret = api.call("PFXIsPFXBlob", {blob});
    if (api.cpu().pending_raise.active) return api.cpu().pending_raise.code;
    return static_cast<u32>(ret);
}

// An unmapped guest address that the harness never maps.
GuestAddr unmapped(bool is64) { return is64 ? 0x380000000ull : 0x03800000ull; }

} // namespace

TEST(Crypt32Pfx, PFXIsPFXBlobMatchesWindowsCases) {
    for (const bool is64 : {false, true}) {
        CryptApi api(is64);
        for (const auto& c : kPfxOracleCases) {
            const std::vector<u8> bytes = unhex(c.hex);
            const bool null_pb = std::string(c.name) == "null_pb_cb0";
            const GuestAddr data = null_pb ? 0 : api.put(bytes.data(), bytes.size());
            const GuestAddr blob = api.put_blob(data, static_cast<u32>(bytes.size()));
            set_last_error(0xDEADBEEFu);
            api.cpu().pending_raise = {};
            const u64 ret = api.call("PFXIsPFXBlob", {blob});
            EXPECT_FALSE(api.cpu().pending_raise.active) << c.name;
            EXPECT_EQ(ret, c.ret) << c.name << (is64 ? " x64" : " x86");
            EXPECT_EQ(get_last_error(), c.last_error) << c.name;
        }
    }
}

TEST(Crypt32Pfx, PFXIsPFXBlobPointerFaultsMatchWindows) {
    for (const bool is64 : {false, true}) {
        CryptApi api(is64);
        // PFXIsPFXBlob(NULL) dereferences pPFX.
        EXPECT_EQ(outcome(api, 0), child_exit("null_blob"));
        EXPECT_EQ(child_exit("null_blob"), kAccessViolation);
        // pbData NULL with cbData 16: FALSE, no fault.
        EXPECT_EQ(outcome(api, api.put_blob(0, 16)), child_exit("null_pb_cb16"));
        // pbData on a released page: fault.
        EXPECT_EQ(outcome(api, api.put_blob(unmapped(is64), 16)),
                  child_exit("freed_pb_cb16"));
        // pPFX itself on a released page: fault.
        EXPECT_EQ(outcome(api, unmapped(is64)), child_exit("freed_blob"));

        // A real PFX ending exactly at the end of a readable page, with cbData
        // exact or running 64 bytes into an unreadable page: only the bytes
        // of the PFX element are read, so both are TRUE.
        const std::vector<u8> pfx = unhex(oracle_case("real_pfx").hex);
        const GuestAddr page = is64 ? 0x390000000ull : 0x03900000ull;
        ASSERT_TRUE(api.memory().map_region(page, 0x1000,
                                            MemoryProtection::ReadWrite).is_ok());
        const GuestAddr start = page + 0x1000 - pfx.size();
        ASSERT_TRUE(api.memory().write_bytes(start, pfx.data(), pfx.size()).is_ok());
        EXPECT_EQ(outcome(api, api.put_blob(start, static_cast<u32>(pfx.size()))),
                  child_exit("pfx_at_page_end"));
        EXPECT_EQ(outcome(api, api.put_blob(start, static_cast<u32>(pfx.size() + 64))),
                  child_exit("pfx_cb_past_page"));
    }
}
