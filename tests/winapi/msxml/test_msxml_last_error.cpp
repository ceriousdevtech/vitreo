// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// MSXML DOM methods leave the thread's last error at 0 on success, S_FALSE
// and failure alike (tools/oracle_probe/msxml_last_error_probe.expected,
// DOMDocument/30/60, x86 and x64). WiX dutil reads GetLastError() after DOM
// calls, so a stale ERROR_PATH_NOT_FOUND from an earlier failed FindFirstFile
// made VC_redist's bootstrapper abort with 0x80070003.

#include <gtest/gtest.h>

#include <winapi/com/msxml/msxml_com.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kStack = 0x30000000;
constexpr GuestAddr kOut = 0x30001000;
constexpr u32 kSFalse = 1;
// IXMLDOMDocument::get_documentElement / IXMLDOMNode::get_firstChild.
constexpr u32 kGetDocumentElement = 45;
constexpr u32 kGetFirstChild = 13;
// CLSID_DOMDocument (msxml.h).
const Guid kClsidDomDocument{0x2933BF90u, 0x7B36u, 0x11d2u,
                             {0xB2, 0x0E, 0x00, 0xC0, 0x4F, 0x98, 0x3E, 0x60}};
const Guid kIidUnknown{0x00000000u, 0x0000u, 0x0000u,
                       {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

class MsxmlLastErrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MsxmlManager::MSXML_SCRATCH_BASE,
                                       MsxmlManager::MSXML_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kStack - 0x1000, 0x3000,
                                       MemoryProtection::ReadWrite).is_ok());
        DllExports exports;
        register_msxml_thunks(exports, mgr_);
        ASSERT_TRUE(thunks_.register_dll("msxml6.dll", std::move(exports)).is_ok());
        mgr_.set_memory(&memory_);
        mgr_.set_thunk_registry(&thunks_);
        mgr_.set_scratch_base(MsxmlManager::MSXML_SCRATCH_BASE);
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rsp = kStack;
    }

    /// Run @p body with a 64-bit Marshaler whose first two arguments are set.
    template <typename F>
    u32 call(u64 arg0, u64 arg1, F&& body) {
        cpu_.gpr.rcx = arg0;
        cpu_.gpr.rdx = arg1;
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return body(m);
    }

    GuestMemory memory_;
    ThunkRegistry thunks_{0x70000000};
    MsxmlManager mgr_;
    cpu::CpuState cpu_{};
};

}  // namespace

TEST_F(MsxmlLastErrorTest, DomMethodsClearAStaleLastError) {
    const u32 created = call(0, 0, [&](Marshaler& m) {
        return mgr_.create_document(m, kClsidDomDocument, kIidUnknown, kOut);
    });
    ASSERT_EQ(created, 0u);
    const GuestAddr doc = memory_.read_u64(kOut).value();
    ASSERT_NE(doc, 0u);

    // An empty document has no element: S_FALSE, and still last error 0.
    set_last_error(3);  // ERROR_PATH_NOT_FOUND, as VC_redist's probe leaves it
    const u32 hr = call(doc, kOut, [&](Marshaler& m) {
        return mgr_.dispatch(MsxmlIface::Document, kGetDocumentElement, m);
    });
    EXPECT_EQ(hr, kSFalse);
    EXPECT_EQ(get_last_error(), 0u);

    // An IXMLDOMNode method on the same object, after another stale error.
    set_last_error(18);
    const u32 hr_child = call(doc, kOut, [&](Marshaler& m) {
        return mgr_.dispatch(MsxmlIface::Document, kGetFirstChild, m);
    });
    EXPECT_EQ(hr_child, kSFalse);
    EXPECT_EQ(get_last_error(), 0u);
}
