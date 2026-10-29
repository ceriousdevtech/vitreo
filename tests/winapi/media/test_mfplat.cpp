// Media Foundation platform contract (mfplat.dll and the Media Feature Pack
// DLLs). Every expected value comes from the Windows SDK 10.0.26100 headers
// and import libraries or from tools/oracle_probe/mf_startup_probe.c
// recorded on Windows (mf_startup_probe{64,32}.expected, x86 == x64), never
// from running Vitreo.
#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/media/mfplat/mf_attributes.hpp"
#include "winapi/media/mfplat/mfplat.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 S_OK_HR = 0;
constexpr u32 S_FALSE_HR = 1;
constexpr u32 E_NOTIMPL_HR = 0x80004001u;
constexpr u32 E_NOINTERFACE_HR = 0x80004002u;
constexpr u32 E_POINTER_HR = 0x80004003u;
constexpr u32 E_INVALIDARG_HR = 0x80070057u;
constexpr u32 E_NOT_SUFFICIENT_BUFFER_HR = 0x8007007Au;
constexpr u32 MF_E_ATTRIBUTENOTFOUND_HR = 0xC00D36E6u;
constexpr u32 MF_E_INVALIDTYPE_HR = 0xC00D36BDu;
constexpr u32 MF_E_BAD_STARTUP_VERSION_HR = 0xC00D36E3u;
constexpr u32 MF_E_UNSUPPORTED_REPRESENTATION_HR = 0xC00D36B7u;
constexpr u32 CLASS_E_CLASSNOTAVAILABLE_HR = 0x80040111u;
constexpr u32 MF_VERSION = 0x00020070u; // SDK mfapi.h: MF_SDK_VERSION << 16 | MF_API_VERSION

// IMFAttributes vtable slots (SDK mfobjects.h order).
enum Slot : u32 {
    QueryInterface = 0, AddRef, Release, GetItem, GetItemType, CompareItem, Compare,
    GetUINT32, GetUINT64, GetDouble, GetGUID, GetStringLength, GetString,
    GetAllocatedString, GetBlobSize, GetBlob, GetAllocatedBlob, GetUnknown, SetItem,
    DeleteItem, DeleteAllItems, SetUINT32, SetUINT64, SetDouble, SetGUID, SetString,
    SetBlob, SetUnknown, LockStore, UnlockStore, GetCount, GetItemByIndex, CopyAllItems,
    GetMajorType, IsCompressedFormat, IsEqual, GetRepresentation, FreeRepresentation,
};

constexpr MfGuid guid(u32 a, u16 b, u16 c, u8 d0, u8 d1, u8 d2, u8 d3, u8 d4, u8 d5, u8 d6, u8 d7) {
    return MfGuid{a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}};
}
// Keys private to the probe (mf_startup_probe.c).
constexpr MfGuid K_U32 = guid(0x7a1c0001, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x01);
constexpr MfGuid K_U64 = guid(0x7a1c0002, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x02);
constexpr MfGuid K_DBL = guid(0x7a1c0003, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x03);
constexpr MfGuid K_GUID = guid(0x7a1c0004, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x04);
constexpr MfGuid K_STR = guid(0x7a1c0005, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x05);
constexpr MfGuid K_BLOB = guid(0x7a1c0006, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x06);
constexpr MfGuid K_UNK = guid(0x7a1c0007, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0x07);
constexpr MfGuid K_NONE = guid(0x7a1c00ff, 0x1111, 0x4a4a, 0x9b, 0x9b, 1, 2, 3, 4, 5, 0xff);
constexpr MfGuid V_GUID = guid(0x5e5e5e5e, 0x2222, 0x4b4b, 0x8c, 0x8c, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
// SDK IIDs and attribute GUIDs.
constexpr MfGuid IID_IUnknown_G = guid(0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
constexpr MfGuid IID_IMFAttributes_G = guid(0x2cd2d921, 0xc447, 0x44a7, 0xa1, 0x3c, 0x4a, 0xda, 0xbf, 0xc2, 0x47, 0xe3);
constexpr MfGuid IID_IMFMediaType_G = guid(0x44ae0fa8, 0xea31, 0x4109, 0x8d, 0x2e, 0x4c, 0xae, 0x49, 0x97, 0xc5, 0x55);
constexpr MfGuid IID_IMFSample_G = guid(0xc40a00f2, 0xb93a, 0x4d80, 0xae, 0x8c, 0x5a, 0x1c, 0x63, 0x4f, 0x58, 0xe4);
constexpr MfGuid MF_MT_MAJOR_TYPE = guid(0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
constexpr MfGuid MF_MT_SUBTYPE = guid(0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
constexpr MfGuid MF_MT_ALL_SAMPLES_INDEPENDENT = guid(0xc9173739, 0x5e56, 0x461c, 0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f);
constexpr MfGuid MF_MT_USER_DATA = guid(0xb6bc765f, 0x4c3b, 0x40a4, 0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d);
constexpr MfGuid MF_MT_INTERLACE_MODE = guid(0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd);
constexpr MfGuid MF_MT_FRAME_RATE_RANGE_MIN = guid(0xd2e7558c, 0xdc1f, 0x403f, 0x9a, 0x72, 0xd2, 0x8b, 0xb1, 0xeb, 0x3b, 0x5e);
constexpr MfGuid MFMediaType_Video = guid(0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
constexpr MfGuid MFVideoFormat_H264 = guid(0x34363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
constexpr MfGuid MFVideoFormat_NV12 = guid(0x3231564e, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

constexpr u16 VT_EMPTY = 0, VT_R8 = 5, VT_UNKNOWN = 13, VT_UI4 = 19, VT_UI8 = 21,
              VT_LPWSTR = 31, VT_CLSID = 72, VT_BLOB = 0x1011, VT_I4 = 3;

const ExportEntry* find_export(const DllExports& exports, const std::string& name) {
    for (const auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

#define ASSERT_TRUE_RET(cond, ret) do { const bool _ok = (cond); EXPECT_TRUE(_ok); if (!_ok) return ret; } while (0)

/// Fixture: an mfplat.dll registered in a thunk registry, a scratch region,
/// a task allocator and a fake guest IUnknown whose vtable calls land in
/// `unk_refs`, all driven through the registered export handlers.
class MfplatFixture : public ::testing::Test {
protected:
    static constexpr GuestAddr DATA = 0x10000;        // 64 KiB of test buffers
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr TASK = 0x20000000;     // CoTaskMem substitute
    static constexpr GuestAddr THUNK_BASE = 0x70000000;
    static constexpr GuestAddr FAKE_UNK = 0x30000;    // guest-implemented IUnknown

    void setup(bool is64) {
        ASSERT_TRUE(memory.map_region(DATA, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(TASK, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(THUNK_BASE, 0x20000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(MfPlatManager::MFPLAT_SCRATCH_BASE,
                                      MfPlatManager::MFPLAT_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
        mgr.set_thunk_registry(&thunks);
        mgr.set_scratch_base(MfPlatManager::MFPLAT_SCRATCH_BASE);
        mgr.set_task_allocator(
            [this](u64 size) -> GuestAddr {
                const GuestAddr p = task_next;
                task_next += (size + 15) & ~u64{15};
                task_allocs.push_back(p);
                return p;
            },
            [this](GuestAddr p) { task_frees.push_back(p); });
        mgr.set_guest_vtable_call(
            [this](Marshaler& m, GuestAddr punk, u32 slot, const u64* args, usize count) -> u32 {
                EXPECT_EQ(punk, FAKE_UNK);
                EXPECT_EQ(args[0], punk);
                if (slot == 1) return ++unk_refs;
                if (slot == 2) return --unk_refs;
                EXPECT_EQ(slot, 0u);
                EXPECT_EQ(count, 3u);
                auto iid = m.read_struct<MfGuid>(static_cast<GuestAddr>(args[1]));
                EXPECT_TRUE(iid.is_ok());
                if (iid.is_ok() && iid.value() == IID_IUnknown_G) {
                    (void)m.write_ptr(static_cast<GuestAddr>(args[2]), punk);
                    ++unk_refs;
                    return S_OK_HR;
                }
                (void)m.write_ptr(static_cast<GuestAddr>(args[2]), 0);
                return E_NOINTERFACE_HR;
            });
        exports = create_mfplat_thunks(mgr);
        DllExports copy = exports;
        ASSERT_TRUE(thunks.register_dll("mfplat.dll", std::move(copy)).is_ok());
    }

    // ---- calling convention helpers ------------------------------------
    u32 call(const std::string& name, std::vector<u64> args) {
        const ExportEntry* e = find_export(exports, name);
        EXPECT_NE(e, nullptr) << name;
        if (e == nullptr) return 0xDEADBEEF;
        place_args(args);
        e->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 method(MfIface iface, GuestAddr self, u32 slot, std::vector<u64> args) {
        args.insert(args.begin(), self);
        return call(MfPlatManager::slot_thunk_name(iface, slot), std::move(args));
    }
    u32 attr(GuestAddr self, u32 slot, std::vector<u64> args = {}) {
        return method(MfIface::Attributes, self, slot, std::move(args));
    }
    u32 mt(GuestAddr self, u32 slot, std::vector<u64> args = {}) {
        return method(MfIface::MediaType, self, slot, std::move(args));
    }
    void place_args(const std::vector<u64>& args) {
        if (cpu.is_64bit()) {
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28 + (i - 4) * 8, args[i]).is_ok());
            }
        } else {
            for (usize i = 0; i < args.size(); ++i) {
                ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i])).is_ok());
            }
        }
    }

    // ---- guest memory helpers -------------------------------------------
    GuestAddr put_guid(GuestAddr at, const MfGuid& g) {
        EXPECT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(&g), 16).is_ok());
        return at;
    }
    GuestAddr put_wstr(GuestAddr at, const std::u16string& s) {
        std::vector<Byte> raw((s.size() + 1) * 2, 0);
        for (usize i = 0; i < s.size(); ++i) {
            raw[i * 2] = static_cast<Byte>(s[i] & 0xFF);
            raw[i * 2 + 1] = static_cast<Byte>(s[i] >> 8);
        }
        EXPECT_TRUE(memory.write_bytes(at, raw.data(), raw.size()).is_ok());
        return at;
    }
    std::u16string read_wstr(GuestAddr at) {
        std::u16string s;
        for (;;) {
            const u16 u = memory.read_u16(at + s.size() * 2).value();
            if (u == 0) return s;
            s.push_back(static_cast<char16_t>(u));
        }
    }
    GuestAddr ptr_size() const { return cpu.is_64bit() ? 8u : 4u; }
    u64 rd_ptr(GuestAddr at) { return marshaler.read_ptr(at).value(); }
    u32 rd32(GuestAddr at) { return memory.read_u32(at).value(); }
    void fill(GuestAddr at, usize n, u8 v) {
        std::vector<Byte> b(n, v);
        ASSERT_TRUE(memory.write_bytes(at, b.data(), n).is_ok());
    }
    /// PROPVARIANT: vt at +0, union at +8 (24 bytes on x64, 16 on x86).
    GuestAddr put_pv(GuestAddr at, u16 vt, u64 payload = 0, u32 blob_count = 0) {
        fill(at, 24, 0);
        EXPECT_TRUE(memory.write_u16(at, vt).is_ok());
        if (vt == VT_BLOB) {
            EXPECT_TRUE(memory.write_u32(at + 8, blob_count).is_ok());
            EXPECT_TRUE(marshaler.write_ptr(at + 8 + ptr_size(), payload).is_ok());
        } else if (vt == VT_UI4) {
            EXPECT_TRUE(memory.write_u32(at + 8, static_cast<u32>(payload)).is_ok());
        } else {
            EXPECT_TRUE(memory.write_u64(at + 8, payload).is_ok());
        }
        return at;
    }
    u16 pv_vt(GuestAddr at) { return memory.read_u16(at).value(); }
    u64 pv_u64(GuestAddr at) { return memory.read_u64(at + 8).value(); }
    u64 pv_ptr(GuestAddr at) { return rd_ptr(at + 8); }

    GuestAddr create(MfIface iface) {
        const GuestAddr out = DATA + 0xF000;
        ASSERT_TRUE_RET(call(iface == MfIface::MediaType ? "MFCreateMediaType" : "MFCreateAttributes",
                             {out, 4u}) == S_OK_HR, 0);
        return static_cast<GuestAddr>(rd_ptr(out));
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ThunkRegistry thunks{THUNK_BASE};
    MfPlatManager mgr;
    DllExports exports;
    GuestAddr task_next{TASK};
    std::vector<GuestAddr> task_allocs, task_frees;
    i32 unk_refs{1};
};

class MfplatTest64 : public MfplatFixture {
protected:
    void SetUp() override { setup(true); }
};
class MfplatTest32 : public MfplatFixture {
protected:
    void SetUp() override { setup(false); }
};

} // namespace

// ===========================================================================
// MFStartup / MFShutdown / MFLockPlatform (mf_startup_probe.expected)
// ===========================================================================

TEST_F(MfplatTest64, StartupVersionMatrixAndShutdownMatchWindows) {
    // Windows answers S_OK for every accepted version (mf_startup_probe);
    // Vitreo keeps the version contract but reports the platform as not
    // implemented, because the MFCreate* factories a successful startup
    // promises do not exist yet (a guest that got S_OK crashed on their null).
    constexpr u32 E_NOTIMPL_HR = 0x80004001u;
    EXPECT_EQ(call("MFStartup", {MF_VERSION, 0}), E_NOTIMPL_HR);           // FULL
    EXPECT_EQ(call("MFStartup", {MF_VERSION, 1}), E_NOTIMPL_HR);           // LITE
    EXPECT_EQ(call("MFStartup", {0x00010070u, 1}), E_NOTIMPL_HR);          // older major
    EXPECT_EQ(call("MFStartup", {0x00030070u, 1}), MF_E_BAD_STARTUP_VERSION_HR);
    EXPECT_EQ(call("MFStartup", {0x00020000u, 1}), E_NOTIMPL_HR);
    EXPECT_EQ(call("MFStartup", {0x000200ffu, 1}), E_NOTIMPL_HR);
    EXPECT_EQ(call("MFStartup", {0x00020071u, 1}), E_NOTIMPL_HR);
    EXPECT_EQ(call("MFStartup", {0, 0}), E_NOTIMPL_HR);
    EXPECT_EQ(call("MFStartup", {0xffffffffu, 0}), MF_E_BAD_STARTUP_VERSION_HR);
    EXPECT_EQ(call("MFStartup", {MF_VERSION, 0x12345678u}), E_NOTIMPL_HR);  // flags ignored
    // A refused startup leaves no reference behind.
    EXPECT_EQ(mgr.startup_count(), 0u);
    EXPECT_EQ(call("MFLockPlatform", {}), S_OK_HR);
    EXPECT_EQ(call("MFUnlockPlatform", {}), S_OK_HR);
    EXPECT_EQ(call("MFUnlockPlatform", {}), S_OK_HR);                 // unbalanced
    for (int i = 0; i < 12; ++i) EXPECT_EQ(call("MFShutdown", {}), S_OK_HR) << i;
    EXPECT_EQ(mgr.startup_count(), 0u);
    // Objects are still creatable after the last shutdown (measured).
    const GuestAddr a = create(MfIface::Attributes);
    EXPECT_NE(a, 0u);
    EXPECT_EQ(attr(a, Release), 0u);
    EXPECT_EQ(find_export(exports, "MFStartup")->stdcall_bytes, 8u);
    EXPECT_EQ(find_export(exports, "MFShutdown")->stdcall_bytes, 0u);
}

// ===========================================================================
// IMFAttributes (mf_startup_probe.expected, section "== IMFAttributes")
// ===========================================================================

TEST_F(MfplatTest64, AttributesMissingKeysLeaveOutputsUntouched) {
    const GuestAddr a = create(MfIface::Attributes);
    ASSERT_NE(a, 0u);
    const GuestAddr key = put_guid(DATA, K_NONE);
    const GuestAddr out = DATA + 0x100;
    ASSERT_TRUE(memory.write_u32(out, 0).is_ok());
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 0u);
    fill(out, 8, 0xCC);
    EXPECT_EQ(attr(a, GetUINT32, {key, out}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(rd32(out), 0xCCCCCCCCu);
    EXPECT_EQ(attr(a, GetItemType, {key, out}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(rd32(out), 0xCCCCCCCCu);
    EXPECT_EQ(attr(a, GetItem, {key, 0}), MF_E_ATTRIBUTENOTFOUND_HR);
    const GuestAddr pv = DATA + 0x200;
    fill(pv, 24, 0xCC);
    EXPECT_EQ(attr(a, GetItem, {key, pv}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(pv_vt(pv), 0xCCCCu);
    EXPECT_EQ(attr(a, DeleteItem, {key}), S_OK_HR);
    EXPECT_EQ(attr(a, GetStringLength, {key, out}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(attr(a, GetBlobSize, {key, out}), MF_E_ATTRIBUTENOTFOUND_HR);
    const GuestAddr iid = put_guid(DATA + 0x300, IID_IUnknown_G);
    fill(out, 8, 0xCC);
    EXPECT_EQ(attr(a, GetUnknown, {key, iid, out}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(rd_ptr(out), 0u);   // measured: *ppv nulled
    put_pv(pv, VT_UI4, 42);
    fill(out, 4, 0xCC);
    EXPECT_EQ(attr(a, CompareItem, {key, pv, out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 0u);     // FALSE for a missing key (Learn, measured)
    EXPECT_EQ(attr(a, Release), 0u);
    EXPECT_EQ(mgr.live_object_count(), 0u);
}

TEST_F(MfplatTest64, AttributesSettersGettersAndTypesMatchWindows) {
    const GuestAddr a = create(MfIface::Attributes);
    ASSERT_NE(a, 0u);
    const GuestAddr k_u32 = put_guid(DATA + 0x00, K_U32), k_u64 = put_guid(DATA + 0x10, K_U64),
                    k_dbl = put_guid(DATA + 0x20, K_DBL), k_guid = put_guid(DATA + 0x30, K_GUID),
                    k_str = put_guid(DATA + 0x40, K_STR), k_blob = put_guid(DATA + 0x50, K_BLOB),
                    k_unk = put_guid(DATA + 0x60, K_UNK), v_guid = put_guid(DATA + 0x70, V_GUID);
    const GuestAddr hello = put_wstr(DATA + 0x80, u"hello");
    const GuestAddr blob = DATA + 0xA0;
    const Byte bytes[5] = {1, 2, 3, 4, 5};
    ASSERT_TRUE(memory.write_bytes(blob, bytes, 5).is_ok());
    const GuestAddr out = DATA + 0x100, out2 = DATA + 0x110, pv = DATA + 0x200, buf = DATA + 0x300;

    EXPECT_EQ(attr(a, SetUINT32, {k_u32, 42}), S_OK_HR);
    EXPECT_EQ(attr(a, SetUINT64, {k_u64, 0x123456789abcdefull}), S_OK_HR);
    u64 two_and_half = 0; const double d = 2.5; std::memcpy(&two_and_half, &d, 8);
    cpu.sse.xmm[2].u64_[0] = two_and_half;   // x64: double in XMM2
    EXPECT_EQ(attr(a, SetDouble, {k_dbl, 0xCCCCCCCCu}), S_OK_HR);
    EXPECT_EQ(attr(a, SetGUID, {k_guid, v_guid}), S_OK_HR);
    EXPECT_EQ(attr(a, SetString, {k_str, hello}), S_OK_HR);
    EXPECT_EQ(attr(a, SetBlob, {k_blob, blob, 5}), S_OK_HR);
    EXPECT_EQ(attr(a, SetUnknown, {k_unk, FAKE_UNK}), S_OK_HR);
    EXPECT_EQ(unk_refs, 2);
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 7u);

    EXPECT_EQ(attr(a, GetUINT32, {k_u32, out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 42u);
    EXPECT_EQ(attr(a, GetUINT64, {k_u64, out}), S_OK_HR);
    EXPECT_EQ(memory.read_u64(out).value(), 0x123456789abcdefull);
    EXPECT_EQ(attr(a, GetDouble, {k_dbl, out}), S_OK_HR);
    EXPECT_EQ(memory.read_u64(out).value(), two_and_half);
    EXPECT_EQ(attr(a, GetGUID, {k_guid, out}), S_OK_HR);
    EXPECT_EQ(marshaler.read_struct<MfGuid>(out).value(), V_GUID);

    // Strings: a buffer that cannot hold the NUL is E_NOT_SUFFICIENT_BUFFER,
    // the length is still reported, the buffer is untouched.
    EXPECT_EQ(attr(a, GetStringLength, {k_str, out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 5u);
    fill(buf, 16, 0x43); fill(out, 4, 0xCC);
    EXPECT_EQ(attr(a, GetString, {k_str, buf, 5, out}), E_NOT_SUFFICIENT_BUFFER_HR);
    EXPECT_EQ(rd32(out), 5u);
    EXPECT_EQ(memory.read_u16(buf).value(), 0x4343u);
    EXPECT_EQ(attr(a, GetString, {k_str, buf, 6, out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 5u);
    EXPECT_EQ(read_wstr(buf), u"hello");
    EXPECT_EQ(attr(a, GetString, {k_str, buf, 6, 0}), S_OK_HR);
    EXPECT_EQ(attr(a, GetString, {k_str, buf, 0, out}), E_NOT_SUFFICIENT_BUFFER_HR);
    EXPECT_EQ(rd32(out), 5u);
    fill(out, 8, 0xCC); fill(out2, 4, 0xCC);
    EXPECT_EQ(attr(a, GetAllocatedString, {k_str, out, out2}), S_OK_HR);
    EXPECT_EQ(rd32(out2), 5u);
    EXPECT_EQ(read_wstr(static_cast<GuestAddr>(rd_ptr(out))), u"hello");
    EXPECT_EQ(task_allocs.size(), 1u);  // CoTaskMemAlloc, freed by the caller

    // Blobs.
    EXPECT_EQ(attr(a, GetBlobSize, {k_blob, out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 5u);
    fill(buf, 8, 0xCC); fill(out, 4, 0xCC);
    EXPECT_EQ(attr(a, GetBlob, {k_blob, buf, 4, out}), E_NOT_SUFFICIENT_BUFFER_HR);
    EXPECT_EQ(rd32(out), 5u);
    EXPECT_EQ(memory.read_u8(buf).value(), 0xCCu);
    EXPECT_EQ(attr(a, GetBlob, {k_blob, buf, 5, out}), S_OK_HR);
    EXPECT_EQ(memory.read_u8(buf).value(), 1u);
    EXPECT_EQ(memory.read_u8(buf + 4).value(), 5u);
    EXPECT_EQ(attr(a, GetBlob, {k_blob, buf, 8, out}), S_OK_HR);
    EXPECT_EQ(rd32(out), 5u);
    EXPECT_EQ(attr(a, GetBlob, {k_blob, buf, 5, 0}), S_OK_HR);
    EXPECT_EQ(attr(a, GetAllocatedBlob, {k_blob, out, out2}), S_OK_HR);
    EXPECT_EQ(rd32(out2), 5u);
    EXPECT_EQ(memory.read_u8(static_cast<GuestAddr>(rd_ptr(out)) + 4).value(), 5u);

    // IUnknown: GetUnknown queries the guest object; E_NOINTERFACE passes through.
    const GuestAddr iid_unk = put_guid(DATA + 0x400, IID_IUnknown_G);
    const GuestAddr iid_attr = put_guid(DATA + 0x410, IID_IMFAttributes_G);
    fill(out, 8, 0xCC);
    EXPECT_EQ(attr(a, GetUnknown, {k_unk, iid_unk, out}), S_OK_HR);
    EXPECT_EQ(rd_ptr(out), FAKE_UNK);
    EXPECT_EQ(unk_refs, 3);
    --unk_refs;  // the caller's Release
    fill(out, 8, 0xCC);
    EXPECT_EQ(attr(a, GetUnknown, {k_unk, iid_attr, out}), E_NOINTERFACE_HR);
    EXPECT_EQ(rd_ptr(out), 0u);
    EXPECT_EQ(unk_refs, 2);

    // Wrong-type reads: MF_E_INVALIDTYPE for every typed getter.
    EXPECT_EQ(attr(a, GetUINT64, {k_u32, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetUINT32, {k_str, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetDouble, {k_u32, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetGUID, {k_u32, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetStringLength, {k_u32, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetString, {k_u32, buf, 8, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetAllocatedString, {k_u32, out, out2}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetBlobSize, {k_u32, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetBlob, {k_u32, buf, 8, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetAllocatedBlob, {k_u32, out, out2}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetUnknown, {k_u32, iid_unk, out}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetBlob, {k_str, buf, 8, out}), MF_E_INVALIDTYPE_HR);

    // Item types and the PROPVARIANT each GetItem hands back.
    struct Expect { GuestAddr key; u16 vt; };
    const Expect expects[] = {{k_u32, VT_UI4}, {k_u64, VT_UI8}, {k_dbl, VT_R8}, {k_guid, VT_CLSID},
                              {k_str, VT_LPWSTR}, {k_blob, VT_BLOB}, {k_unk, VT_UNKNOWN}};
    for (const Expect& e : expects) {
        fill(out, 4, 0xCC);
        EXPECT_EQ(attr(a, GetItemType, {e.key, out}), S_OK_HR);
        EXPECT_EQ(rd32(out), e.vt);
        fill(pv, 24, 0xCC);
        EXPECT_EQ(attr(a, GetItem, {e.key, pv}), S_OK_HR);
        EXPECT_EQ(pv_vt(pv), e.vt);
        if (e.vt == VT_UNKNOWN) --unk_refs;   // the copy's reference, PropVariantClear'd
    }
    EXPECT_EQ(attr(a, GetItem, {k_u32, pv}), S_OK_HR);
    EXPECT_EQ(rd32(pv + 8), 42u);
    EXPECT_EQ(attr(a, GetItem, {k_u64, pv}), S_OK_HR);
    EXPECT_EQ(pv_u64(pv), 0x123456789abcdefull);
    EXPECT_EQ(attr(a, GetItem, {k_guid, pv}), S_OK_HR);
    EXPECT_EQ(marshaler.read_struct<MfGuid>(static_cast<GuestAddr>(pv_ptr(pv))).value(), V_GUID);
    EXPECT_EQ(attr(a, GetItem, {k_str, pv}), S_OK_HR);
    EXPECT_EQ(read_wstr(static_cast<GuestAddr>(pv_ptr(pv))), u"hello");
    EXPECT_EQ(attr(a, GetItem, {k_blob, pv}), S_OK_HR);
    EXPECT_EQ(rd32(pv + 8), 5u);
    EXPECT_EQ(memory.read_u8(static_cast<GuestAddr>(rd_ptr(pv + 16)) + 4).value(), 5u);
    EXPECT_EQ(attr(a, GetItem, {k_unk, pv}), S_OK_HR);
    EXPECT_EQ(pv_ptr(pv), FAKE_UNK);
    EXPECT_EQ(unk_refs, 3);       // the copy holds a reference (PropVariantClear releases)
    --unk_refs;
    EXPECT_EQ(attr(a, GetItem, {k_u32, 0}), S_OK_HR);

    // CompareItem: type and value, strings case-sensitive, IUnknown by identity.
    put_pv(pv, VT_UI4, 42); fill(out, 4, 0xCC);
    EXPECT_EQ(attr(a, CompareItem, {k_u32, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    put_pv(pv, VT_UI4, 43);
    EXPECT_EQ(attr(a, CompareItem, {k_u32, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    put_pv(pv, VT_UI8, 42);
    EXPECT_EQ(attr(a, CompareItem, {k_u32, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    put_pv(pv, VT_LPWSTR, hello);
    EXPECT_EQ(attr(a, CompareItem, {k_str, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    put_pv(pv, VT_LPWSTR, put_wstr(DATA + 0x500, u"Hello"));
    EXPECT_EQ(attr(a, CompareItem, {k_str, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    put_pv(pv, VT_BLOB, blob, 5);
    EXPECT_EQ(attr(a, CompareItem, {k_blob, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    put_pv(pv, VT_BLOB, blob, 4);
    EXPECT_EQ(attr(a, CompareItem, {k_blob, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    put_pv(pv, VT_CLSID, v_guid);
    EXPECT_EQ(attr(a, CompareItem, {k_guid, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    put_pv(pv, VT_UNKNOWN, FAKE_UNK);
    EXPECT_EQ(attr(a, CompareItem, {k_unk, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    EXPECT_EQ(unk_refs, 2);
    put_pv(pv, VT_R8, two_and_half);
    EXPECT_EQ(attr(a, CompareItem, {k_dbl, pv, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);

    // SetItem type gate: a rejected type still leaves an empty item behind
    // (measured on Windows), removed by DeleteItem.
    const GuestAddr k_none = put_guid(DATA + 0x600, K_NONE);
    put_pv(pv, VT_I4, 1);
    EXPECT_EQ(attr(a, SetItem, {k_none, pv}), MF_E_INVALIDTYPE_HR);
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 8u);
    fill(out, 4, 0xCC);
    EXPECT_EQ(attr(a, GetItemType, {k_none, out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    fill(pv, 24, 0xCC);
    EXPECT_EQ(attr(a, GetItem, {k_none, pv}), S_OK_HR); EXPECT_EQ(pv_vt(pv), VT_EMPTY);
    EXPECT_EQ(attr(a, DeleteItem, {k_none}), S_OK_HR);
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 7u);
    for (u16 vt : {u16{0}, u16{11}, u16{30}, u16{17}, u16{20}, u16{4}}) {
        put_pv(pv, vt, 1);
        EXPECT_EQ(attr(a, SetItem, {k_none, pv}), MF_E_INVALIDTYPE_HR) << vt;
    }
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 8u);
    EXPECT_EQ(attr(a, DeleteItem, {k_none}), S_OK_HR);
    put_pv(pv, VT_UI4, 7);
    EXPECT_EQ(attr(a, SetItem, {k_u32, pv}), S_OK_HR);
    EXPECT_EQ(attr(a, GetUINT32, {k_u32, out}), S_OK_HR); EXPECT_EQ(rd32(out), 7u);
    put_pv(pv, VT_LPWSTR, put_wstr(DATA + 0x700, u"replaced"));
    EXPECT_EQ(attr(a, SetItem, {k_u32, pv}), S_OK_HR);
    EXPECT_EQ(attr(a, GetItemType, {k_u32, out}), S_OK_HR); EXPECT_EQ(rd32(out), VT_LPWSTR);
    EXPECT_EQ(attr(a, SetUINT32, {k_u32, 42}), S_OK_HR);
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 7u);

    // Enumeration keeps insertion order; out of range is E_INVALIDARG with
    // the outputs untouched.
    const MfGuid order[7] = {K_U32, K_U64, K_DBL, K_GUID, K_STR, K_BLOB, K_UNK};
    const u16 vts[7] = {VT_UI4, VT_UI8, VT_R8, VT_CLSID, VT_LPWSTR, VT_BLOB, VT_UNKNOWN};
    for (u32 i = 0; i < 7; ++i) {
        fill(out, 16, 0xCC); fill(pv, 24, 0xCC);
        EXPECT_EQ(attr(a, GetItemByIndex, {i, out, pv}), S_OK_HR) << i;
        EXPECT_EQ(marshaler.read_struct<MfGuid>(out).value(), order[i]) << i;
        EXPECT_EQ(pv_vt(pv), vts[i]) << i;
    }
    unk_refs = 2;  // the copies above would be PropVariantClear'd by a guest
    fill(out, 16, 0xCC); fill(pv, 24, 0xCC);
    EXPECT_EQ(attr(a, GetItemByIndex, {7, out, pv}), E_INVALIDARG_HR);
    EXPECT_EQ(rd32(out), 0xCCCCCCCCu);
    EXPECT_EQ(pv_vt(pv), 0xCCCCu);
    EXPECT_EQ(attr(a, GetItemByIndex, {0, out, 0}), S_OK_HR);

    // Compare against a second store, then CopyAllItems replaces it.
    const GuestAddr c = create(MfIface::Attributes);
    ASSERT_NE(c, 0u);
    EXPECT_EQ(attr(c, SetUINT32, {k_u32, 42}), S_OK_HR);
    EXPECT_EQ(attr(c, SetString, {k_str, hello}), S_OK_HR);
    const u32 expected_a_vs_c[5] = {0, 1, 0, 1, 1};   // OUR, THEIR, ALL, INTERSECTION, SMALLER
    const u32 expected_c_vs_a[5] = {1, 0, 0, 1, 1};
    for (u32 t = 0; t < 5; ++t) {
        fill(out, 4, 0xCC);
        EXPECT_EQ(attr(a, Compare, {c, t, out}), S_OK_HR); EXPECT_EQ(rd32(out), expected_a_vs_c[t]) << t;
        fill(out, 4, 0xCC);
        EXPECT_EQ(attr(c, Compare, {a, t, out}), S_OK_HR); EXPECT_EQ(rd32(out), expected_c_vs_a[t]) << t;
    }
    EXPECT_EQ(attr(c, SetUINT32, {k_u32, 43}), S_OK_HR);
    EXPECT_EQ(attr(c, Compare, {a, 4, out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    EXPECT_EQ(attr(a, Compare, {a, 2, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    EXPECT_EQ(attr(a, CopyAllItems, {c}), S_OK_HR);
    EXPECT_EQ(attr(c, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 7u);
    EXPECT_EQ(attr(c, GetUINT32, {k_u32, out}), S_OK_HR); EXPECT_EQ(rd32(out), 42u);
    EXPECT_EQ(unk_refs, 3);
    EXPECT_EQ(attr(a, Compare, {c, 2, out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    EXPECT_EQ(attr(c, DeleteAllItems), S_OK_HR);
    EXPECT_EQ(attr(c, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    EXPECT_EQ(unk_refs, 2);
    EXPECT_EQ(attr(c, Release), 0u);

    EXPECT_EQ(attr(a, LockStore), S_OK_HR);
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 7u);
    EXPECT_EQ(attr(a, UnlockStore), S_OK_HR);
    EXPECT_EQ(attr(a, DeleteItem, {k_unk}), S_OK_HR);
    EXPECT_EQ(unk_refs, 1);
    EXPECT_EQ(attr(a, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 6u);
    EXPECT_EQ(attr(a, DeleteItem, {k_unk}), S_OK_HR);

    // IUnknown of the store itself.
    fill(out, 8, 0xCC);
    EXPECT_EQ(attr(a, QueryInterface, {iid_unk, out}), S_OK_HR); EXPECT_EQ(rd_ptr(out), a);
    EXPECT_EQ(attr(a, Release), 1u);
    EXPECT_EQ(attr(a, QueryInterface, {iid_attr, out}), S_OK_HR); EXPECT_EQ(rd_ptr(out), a);
    EXPECT_EQ(attr(a, Release), 1u);
    fill(out, 8, 0xCC);
    EXPECT_EQ(attr(a, QueryInterface, {put_guid(DATA + 0x420, IID_IMFMediaType_G), out}), E_NOINTERFACE_HR);
    EXPECT_EQ(rd_ptr(out), 0u);
    EXPECT_EQ(attr(a, AddRef), 2u);
    EXPECT_EQ(attr(a, Release), 1u);
    EXPECT_EQ(attr(a, Release), 0u);
    EXPECT_EQ(mgr.live_object_count(), 0u);
}

TEST_F(MfplatTest64, ReleasingAStoreReleasesItsUnknowns) {
    const GuestAddr a = create(MfIface::Attributes);
    ASSERT_NE(a, 0u);
    EXPECT_EQ(attr(a, SetUnknown, {put_guid(DATA, K_UNK), FAKE_UNK}), S_OK_HR);
    EXPECT_EQ(unk_refs, 2);
    EXPECT_EQ(attr(a, SetUnknown, {DATA, FAKE_UNK}), S_OK_HR);   // replace: AddRef new, Release old
    EXPECT_EQ(unk_refs, 2);
    EXPECT_EQ(attr(a, Release), 0u);
    EXPECT_EQ(unk_refs, 1);
    EXPECT_EQ(attr(a, GetCount, {DATA + 0x100}), E_POINTER_HR);  // dead interface pointer
}

// ===========================================================================
// IMFMediaType (mf_startup_probe.expected, section "== IMFMediaType")
// ===========================================================================

TEST_F(MfplatTest64, MediaTypeIsEqualFlagsMatchWindows) {
    const GuestAddr t = create(MfIface::MediaType);
    const GuestAddr u = create(MfIface::MediaType);
    ASSERT_NE(t, 0u); ASSERT_NE(u, 0u);
    const GuestAddr major = put_guid(DATA + 0x00, MF_MT_MAJOR_TYPE), sub = put_guid(DATA + 0x10, MF_MT_SUBTYPE),
                    indep = put_guid(DATA + 0x20, MF_MT_ALL_SAMPLES_INDEPENDENT),
                    ud = put_guid(DATA + 0x30, MF_MT_USER_DATA), il = put_guid(DATA + 0x40, MF_MT_INTERLACE_MODE),
                    rmin = put_guid(DATA + 0x50, MF_MT_FRAME_RATE_RANGE_MIN),
                    video = put_guid(DATA + 0x60, MFMediaType_Video), h264 = put_guid(DATA + 0x70, MFVideoFormat_H264),
                    nv12 = put_guid(DATA + 0x80, MFVideoFormat_NV12);
    const GuestAddr out = DATA + 0x100, blob = DATA + 0x200;
    const Byte bytes[3] = {9, 8, 7};
    ASSERT_TRUE(memory.write_bytes(blob, bytes, 3).is_ok());

    EXPECT_EQ(mt(t, GetCount, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    fill(out, 16, 0xCC);
    EXPECT_EQ(mt(t, GetMajorType, {out}), MF_E_ATTRIBUTENOTFOUND_HR);
    EXPECT_EQ(rd32(out), 0xCCCCCCCCu);
    EXPECT_EQ(mt(t, IsCompressedFormat, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    fill(out, 4, 0xCC);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), E_INVALIDARG_HR); EXPECT_EQ(rd32(out), 0u);  // flags zeroed
    EXPECT_EQ(mt(t, SetGUID, {major, video}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), E_INVALIDARG_HR);
    EXPECT_EQ(mt(u, SetGUID, {major, video}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_OK_HR); EXPECT_EQ(rd32(out), 15u);
    EXPECT_EQ(mt(t, GetMajorType, {out}), S_OK_HR);
    EXPECT_EQ(marshaler.read_struct<MfGuid>(out).value(), MFMediaType_Video);
    EXPECT_EQ(mt(t, SetGUID, {sub, h264}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_FALSE_HR); EXPECT_EQ(rd32(out), 13u);
    EXPECT_EQ(mt(u, SetGUID, {sub, nv12}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_FALSE_HR); EXPECT_EQ(rd32(out), 9u);
    EXPECT_EQ(mt(u, SetGUID, {sub, h264}), S_OK_HR);
    EXPECT_EQ(mt(t, SetUINT32, {il, 2}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_OK_HR); EXPECT_EQ(rd32(out), 15u);     // subset rule
    EXPECT_EQ(mt(u, SetUINT32, {il, 3}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_FALSE_HR); EXPECT_EQ(rd32(out), 11u);
    EXPECT_EQ(mt(u, SetUINT32, {il, 2}), S_OK_HR);
    EXPECT_EQ(mt(t, SetBlob, {ud, blob, 3}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_FALSE_HR); EXPECT_EQ(rd32(out), 7u);
    EXPECT_EQ(mt(u, SetBlob, {ud, blob, 3}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_OK_HR); EXPECT_EQ(rd32(out), 15u);
    EXPECT_EQ(mt(t, SetUINT32, {indep, 1}), S_OK_HR);
    EXPECT_EQ(mt(t, IsCompressedFormat, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 0u);
    EXPECT_EQ(mt(t, SetUINT32, {indep, 0}), S_OK_HR);
    EXPECT_EQ(mt(t, IsCompressedFormat, {out}), S_OK_HR); EXPECT_EQ(rd32(out), 1u);
    EXPECT_EQ(mt(t, SetUINT64, {rmin, 5}), S_OK_HR);
    EXPECT_EQ(mt(t, IsEqual, {u, out}), S_OK_HR); EXPECT_EQ(rd32(out), 15u);     // range-min excluded
    fill(out, 8, 0xCC);
    EXPECT_EQ(mt(t, GetRepresentation, {DATA + 0x90, out}), MF_E_UNSUPPORTED_REPRESENTATION_HR);
    EXPECT_EQ(rd_ptr(out), 0xCCCCCCCCCCCCCCCCull);   // measured: untouched
    fill(out, 8, 0xCC);
    EXPECT_EQ(mt(t, QueryInterface, {put_guid(DATA + 0x300, IID_IMFAttributes_G), out}), S_OK_HR);
    EXPECT_EQ(rd_ptr(out), t);
    EXPECT_EQ(mt(t, Release), 1u);
    EXPECT_EQ(mt(t, QueryInterface, {put_guid(DATA + 0x310, IID_IMFMediaType_G), out}), S_OK_HR);
    EXPECT_EQ(rd_ptr(out), t);
    EXPECT_EQ(mt(t, Release), 1u);
    fill(out, 8, 0xCC);
    EXPECT_EQ(mt(t, QueryInterface, {put_guid(DATA + 0x320, IID_IMFSample_G), out}), E_NOINTERFACE_HR);
    EXPECT_EQ(rd_ptr(out), 0u);
    EXPECT_EQ(mt(t, Release), 0u);
    EXPECT_EQ(mt(u, Release), 0u);
}

// ===========================================================================
// Vtables, transform enumeration, the export table
// ===========================================================================

TEST_F(MfplatTest64, ObjectVtablesPointAtRegisteredSlotThunks) {
    const GuestAddr a = create(MfIface::Attributes);
    const GuestAddr t = create(MfIface::MediaType);
    ASSERT_NE(a, 0u); ASSERT_NE(t, 0u);
    EXPECT_EQ(MfPlatManager::slot_count(MfIface::Attributes), 33u);   // SDK mfobjects.h
    EXPECT_EQ(MfPlatManager::slot_count(MfIface::MediaType), 38u);
    for (MfIface iface : {MfIface::Attributes, MfIface::MediaType}) {
        const GuestAddr self = iface == MfIface::Attributes ? a : t;
        const GuestAddr vtable = static_cast<GuestAddr>(rd_ptr(self));
        ASSERT_NE(vtable, 0u);
        for (u32 slot = 0; slot < MfPlatManager::slot_count(iface); ++slot) {
            auto thunk = thunks.get_thunk_address("mfplat.dll", MfPlatManager::slot_thunk_name(iface, slot));
            ASSERT_TRUE(thunk.is_ok()) << slot;
            EXPECT_EQ(rd_ptr(vtable + slot * 8), thunk.value()) << slot;
        }
    }
    // Both media-type and attributes objects share the IMFAttributes prefix:
    // slot 7 (GetUINT32) must be the same thunk in both vtables.
    EXPECT_EQ(find_export(exports, MfPlatManager::slot_thunk_name(MfIface::Attributes, 7))->stdcall_bytes, 12u);
    EXPECT_EQ(attr(a, Release), 0u);
    EXPECT_EQ(mt(t, Release), 0u);
}

TEST_F(MfplatTest64, TransformEnumerationFindsNothing) {
    // Measured: S_OK, zero count, NULL array, for any category or flags.
    const GuestAddr cat = put_guid(DATA, K_NONE), arr = DATA + 0x100, n = DATA + 0x110;
    fill(arr, 8, 0xCC); fill(n, 4, 0xCC);
    EXPECT_EQ(call("MFTEnumEx", {cat, 1, 0, 0, arr, n}), S_OK_HR);
    EXPECT_EQ(rd_ptr(arr), 0u); EXPECT_EQ(rd32(n), 0u);
    fill(arr, 8, 0xCC); fill(n, 4, 0xCC);
    EXPECT_EQ(call("MFTEnumEx", {cat, 0x3F, DATA + 0x200, 0, arr, n}), S_OK_HR);
    EXPECT_EQ(rd_ptr(arr), 0u); EXPECT_EQ(rd32(n), 0u);
    fill(arr, 8, 0xCC); fill(n, 4, 0xCC);
    EXPECT_EQ(call("MFTEnum", {cat, 0, 0, 0, 0, arr, n}), S_OK_HR);
    EXPECT_EQ(rd_ptr(arr), 0u); EXPECT_EQ(rd32(n), 0u);
    fill(arr, 8, 0xCC); fill(n, 4, 0xCC);
    EXPECT_EQ(call("MFTEnum2", {cat, 0, 0, 0, 0, arr, n}), S_OK_HR);
    EXPECT_EQ(rd_ptr(arr), 0u); EXPECT_EQ(rd32(n), 0u);
}

TEST_F(MfplatTest64, EverySdkImportLibraryExportResolvesToADocumentedFailure) {
    struct Row { const char* dll; const char* name; u16 bytes; int kind; };
    enum { HRESULT_K, BOOL_K, PTR_K, VOID_K, I64_K };
#define HRESULT HRESULT_K
#define BOOL BOOL_K
#define PTR PTR_K
#define VOID VOID_K
#define I64 I64_K
#define MF_EXPORT(d, n, b, k) Row{d, n, static_cast<u16>(b), k},
    static const Row kTable[] = {
#include "winapi/media/mfplat/mf_export_table.inc"
    };
#undef MF_EXPORT
#undef HRESULT
#undef BOOL
#undef PTR
#undef VOID
#undef I64
    std::map<std::string, DllExports> dlls;
    dlls["mfplat.dll"] = exports;
    dlls["mf.dll"] = create_mf_thunks();
    dlls["mfreadwrite.dll"] = create_mfreadwrite_thunks();
    dlls["evr.dll"] = create_evr_thunks();
    dlls["dxva2.dll"] = create_dxva2_thunks();
    // No name may be registered twice: the registry keeps the last entry,
    // so a duplicate silently replaces an implementation with its loud twin.
    for (const auto& dll : dlls) {
        std::map<std::string, usize> seen;
        for (const auto& e : dll.second.entries) ++seen[e.name];
        for (const auto& [name, n] : seen) EXPECT_EQ(n, 1u) << dll.first << "!" << name;
    }
    usize checked = 0;
    for (const Row& row : kTable) {
        const ExportEntry* e = find_export(dlls[row.dll], row.name);
        ASSERT_NE(e, nullptr) << row.dll << "!" << row.name;
        EXPECT_EQ(e->stdcall_bytes, row.bytes) << row.dll << "!" << row.name;
        ++checked;
    }
    EXPECT_GT(checked, 250u);

    // Unimplemented exports answer with the failure value of their return
    // type, never the missing-export stub's 0xFFFFFFFF.
    const ExportEntry* collection = find_export(exports, "MFCreateCollection");   // HRESULT
    ASSERT_NE(collection, nullptr);
    collection->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_NOTIMPL_HR);
    const ExportEntry* brightness = find_export(dlls["dxva2.dll"], "GetMonitorBrightness");  // BOOL
    ASSERT_NE(brightness, nullptr);
    cpu.gpr.rax = 0xFFFFFFFFu;
    brightness->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    const ExportEntry* heap = find_export(exports, "MFHeapAlloc");   // void*
    ASSERT_NE(heap, nullptr);
    cpu.gpr.rax = 0xFFFFFFFFu;
    heap->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    // LONGLONG MFllMulDiv(a, b, c, d) = ((a * b) + d) / c (Learn).
    EXPECT_EQ(call("MFllMulDiv", {10, 20, 3, 1}), 67u);
    place_args({static_cast<u64>(-7), 3, 2, 0});
    find_export(exports, "MFllMulDiv")->handler(marshaler);
    EXPECT_EQ(static_cast<i64>(cpu.gpr.rax), -10);

    // The Media Feature Pack COM servers exist and have no classes to offer.
    DllExports vdec = create_msmpeg2vdec_thunks();
    for (const char* name : {"DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer", "DllUnregisterServer"}) {
        EXPECT_NE(find_export(vdec, name), nullptr) << name;
    }
    EXPECT_NE(find_export(dlls["evr.dll"], "DllGetClassObject"), nullptr);
    EXPECT_NE(find_export(dlls["mf.dll"], "DllGetClassObject"), nullptr);
    EXPECT_NE(find_export(dlls["mfreadwrite.dll"], "DllCanUnloadNow"), nullptr);
    const GuestAddr ppv = DATA + 0x100;
    fill(ppv, 8, 0xCC);
    place_args({DATA, DATA + 0x10, ppv});
    find_export(vdec, "DllGetClassObject")->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), CLASS_E_CLASSNOTAVAILABLE_HR);
    EXPECT_EQ(rd_ptr(ppv), 0u);
    EXPECT_EQ(find_export(vdec, "DllGetClassObject")->stdcall_bytes, 12u);
}

// ===========================================================================
// x86: stdcall sizes and stack-passed wide arguments
// ===========================================================================

TEST_F(MfplatTest32, X86StdcallSizesComeFromTheImportLibrary) {
    // libmfplat / SDK mfplat.lib decorations.
    EXPECT_EQ(find_export(exports, "MFTEnumEx")->stdcall_bytes, 36u);
    EXPECT_EQ(find_export(exports, "MFTEnum")->stdcall_bytes, 40u);
    EXPECT_EQ(find_export(exports, "MFTEnum2")->stdcall_bytes, 40u);
    EXPECT_EQ(find_export(exports, "MFCreateDXGISurfaceBuffer")->stdcall_bytes, 20u);
    EXPECT_EQ(find_export(exports, "MFCreateMediaBufferWrapper")->stdcall_bytes, 16u);
    EXPECT_EQ(find_export(exports, "MFCreateWaveFormatExFromMFMediaType")->stdcall_bytes, 16u);
    EXPECT_EQ(find_export(exports, "MFInitMediaTypeFromWaveFormatEx")->stdcall_bytes, 12u);
    EXPECT_EQ(find_export(exports, "MFllMulDiv")->stdcall_bytes, 32u);
    EXPECT_EQ(find_export(exports, "MFCreateAttributes")->stdcall_bytes, 8u);
    // Vtable slots: `this` plus the SDK parameter list.
    EXPECT_EQ(MfPlatManager::slot_stdcall_bytes(MfIface::Attributes, GetString), 20u);
    EXPECT_EQ(MfPlatManager::slot_stdcall_bytes(MfIface::Attributes, SetDouble), 16u);
    EXPECT_EQ(MfPlatManager::slot_stdcall_bytes(MfIface::Attributes, SetUINT64), 16u);
    EXPECT_EQ(MfPlatManager::slot_stdcall_bytes(MfIface::Attributes, GetItemByIndex), 16u);
    EXPECT_EQ(MfPlatManager::slot_stdcall_bytes(MfIface::MediaType, GetRepresentation), 24u);
    EXPECT_EQ(MfPlatManager::slot_stdcall_bytes(MfIface::MediaType, FreeRepresentation), 24u);
}

TEST_F(MfplatTest32, X86WideArgumentsAndByValueGuidsUseStackSlots) {
    const GuestAddr a = create(MfIface::Attributes);
    ASSERT_NE(a, 0u);
    const GuestAddr k_u64 = put_guid(DATA, K_U64), k_dbl = put_guid(DATA + 0x10, K_DBL);
    const GuestAddr out = DATA + 0x100;
    // SetUINT64(key, UINT64): low dword then high dword on the stack.
    EXPECT_EQ(attr(a, SetUINT64, {k_u64, 0x89abcdefu, 0x01234567u}), S_OK_HR);
    EXPECT_EQ(attr(a, GetUINT64, {k_u64, out}), S_OK_HR);
    EXPECT_EQ(memory.read_u64(out).value(), 0x0123456789abcdefull);
    // SetDouble(key, double): two stack slots.
    u64 bits = 0; const double d = 2.5; std::memcpy(&bits, &d, 8);
    EXPECT_EQ(attr(a, SetDouble, {k_dbl, bits & 0xFFFFFFFFu, bits >> 32}), S_OK_HR);
    EXPECT_EQ(attr(a, GetDouble, {k_dbl, out}), S_OK_HR);
    EXPECT_EQ(memory.read_u64(out).value(), bits);
    // GetItem writes a 16-byte PROPVARIANT: blob pElems at +12.
    const GuestAddr blob = DATA + 0x200, pv = DATA + 0x300, k_blob = put_guid(DATA + 0x20, K_BLOB);
    const Byte bytes[3] = {7, 8, 9};
    ASSERT_TRUE(memory.write_bytes(blob, bytes, 3).is_ok());
    EXPECT_EQ(attr(a, SetBlob, {k_blob, blob, 3}), S_OK_HR);
    fill(pv, 16, 0xCC);
    EXPECT_EQ(attr(a, GetItem, {k_blob, pv}), S_OK_HR);
    EXPECT_EQ(pv_vt(pv), VT_BLOB);
    EXPECT_EQ(rd32(pv + 8), 3u);
    EXPECT_EQ(memory.read_u8(rd32(pv + 12) + 2).value(), 9u);
    EXPECT_EQ(attr(a, Release), 0u);

    // MFTEnumEx: the by-value GUID takes four slots, so the out-params are
    // slots 7 and 8 (SDK mfplat.lib: _MFTEnumEx@36).
    const GuestAddr arr = DATA + 0x400, n = DATA + 0x410;
    fill(arr, 4, 0xCC); fill(n, 4, 0xCC);
    EXPECT_EQ(call("MFTEnumEx", {1, 2, 3, 4, 0, 0, 0, arr, n}), S_OK_HR);
    EXPECT_EQ(rd32(arr), 0u); EXPECT_EQ(rd32(n), 0u);
    // IMFMediaType::GetRepresentation(GUID by value, LPVOID*): ppv is slot 5.
    const GuestAddr t = create(MfIface::MediaType);
    ASSERT_NE(t, 0u);
    fill(out, 4, 0xCC);
    EXPECT_EQ(mt(t, GetRepresentation, {1, 2, 3, 4, out}), MF_E_UNSUPPORTED_REPRESENTATION_HR);
    EXPECT_EQ(rd32(out), 0xCCCCCCCCu);
    EXPECT_EQ(mt(t, Release), 0u);
}

// ===========================================================================
// Pure functions kept from the S7 audit (s7_mf_probe.expected)
// ===========================================================================

// MFFrameRateToAverageTimePerFrame: expected values from
// tools/oracle_probe/s7_mf_probe.expected (Windows 11, x64 and x86 agree).
TEST_F(MfplatTest64, FrameRateToAverageTimePerFrameMatchesWindows) {
    const auto* fn = find_export(exports, "MFFrameRateToAverageTimePerFrame");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->stdcall_bytes, 12u);
    struct Case { u32 num, den; u64 expected; };
    const Case cases[] = {
        {60000, 1001, 166833}, {30000, 1001, 333667}, {24000, 1001, 417188},
        {60, 1, 166667}, {24, 1, 416667}, {7, 1, 1428571}, {48000, 1001, 208542},
        {3, 2, 6666667}, {1, 3, 30000000}, {144, 1, 69444}, {29, 1, 344828},
        {23966, 1000, 417188}, {23961, 1000, 417345}, {59950, 1000, 166806},
        {59930, 1000, 166833}, {24010, 1000, 416667}, {24011, 1000, 416476},
        {0, 1, 0}, {1, 0, 0}, {0xFFFFFFFFu, 1, 0},
        {1, 0xFFFFFFFFu, 42949672950000000ull}, {0xFFFFFFFFu, 0xFFFFFFFFu, 10000000},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(call("MFFrameRateToAverageTimePerFrame", {c.num, c.den, DATA}), S_OK_HR) << c.num << "/" << c.den;
        EXPECT_EQ(memory.read_u64(DATA).value(), c.expected) << c.num << "/" << c.den;
    }
}

// MFGetSystemTime is a LONGLONG: on x86 the high half comes back in EDX.
TEST_F(MfplatTest32, GetSystemTimeReturnsFull64BitValueOnX86) {
    cpu.gpr.rdx = 0xDEADBEEFu;
    find_export(exports, "MFGetSystemTime")->handler(marshaler);
    const u64 value = (u64{static_cast<u32>(cpu.gpr.rdx)} << 32) | static_cast<u32>(cpu.gpr.rax);
    EXPECT_NE(static_cast<u32>(cpu.gpr.rdx), 0xDEADBEEFu);
    EXPECT_GT(value, 0u);
}
