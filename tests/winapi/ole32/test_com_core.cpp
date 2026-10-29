#include "com_test_common.hpp"
#include <algorithm>
#include <thread>
#include <vector>

class ComCoreTest : public ::testing::Test {
protected:
    ComCoreManager mgr;
};

// 1. CoInitializeEx first call with MULTITHREADED returns S_OK
TEST_F(ComCoreTest, CoInitializeEx_ReturnsSOK) {
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_OK);
    EXPECT_TRUE(mgr.is_initialized());
    EXPECT_EQ(mgr.init_count(), 1u);
}

// 2. Second call with same apartment type returns S_FALSE
TEST_F(ComCoreTest, CoInitializeEx_SameApartment_ReturnsSFalse) {
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_FALSE);
    EXPECT_EQ(mgr.init_count(), 2u);
}

// 3. Second call with different apartment type returns RPC_E_CHANGED_MODE
TEST_F(ComCoreTest, CoInitializeEx_DifferentApartment_ReturnsChangedMode) {
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_APARTMENTTHREADED), com::RPC_E_CHANGED_MODE);
    // init_count should not have incremented
    EXPECT_EQ(mgr.init_count(), 1u);
}

// 4. CoUninitialize fully, then CoInitializeEx returns S_OK again
TEST_F(ComCoreTest, CoUninitialize_ThenReinit) {
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_OK);
    mgr.co_uninitialize();
    EXPECT_FALSE(mgr.is_initialized());
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_APARTMENTTHREADED), com::S_OK);
    EXPECT_TRUE(mgr.is_initialized());
}

// 5. CoInitialize sets APARTMENTTHREADED, then CoInitializeEx(MT) returns CHANGED_MODE
TEST_F(ComCoreTest, CoInitialize_IsApartmentThreaded) {
    EXPECT_EQ(mgr.co_initialize(), com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::RPC_E_CHANGED_MODE);
}

TEST_F(ComCoreTest, CoIncrementMTAUsageRejectsNullCookieOutput) {
    EXPECT_EQ(mgr.co_increment_mta_usage(nullptr), com::E_INVALIDARG);
    EXPECT_EQ(mgr.mta_usage_count(), 0u);
}

TEST_F(ComCoreTest, MTAUsageCookiesAreUniqueAndConsumedOnce) {
    GuestAddr first = 0;
    GuestAddr second = 0;
    ASSERT_EQ(mgr.co_increment_mta_usage(&first), com::S_OK);
    ASSERT_EQ(mgr.co_increment_mta_usage(&second), com::S_OK);
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);
    EXPECT_EQ(mgr.mta_usage_count(), 2u);

    EXPECT_EQ(mgr.co_decrement_mta_usage(first), com::S_OK);
    EXPECT_EQ(mgr.co_decrement_mta_usage(first), com::E_INVALIDARG);
    EXPECT_EQ(mgr.co_decrement_mta_usage(second), com::S_OK);
    EXPECT_EQ(mgr.mta_usage_count(), 0u);
}

TEST_F(ComCoreTest, MTAUsageCanBeReleasedFromAnotherThread) {
    GuestAddr cookie = 0;
    ASSERT_EQ(mgr.co_increment_mta_usage(&cookie), com::S_OK);
    u32 result = com::E_INVALIDARG;
    std::thread releaser([&] {
        result = mgr.co_decrement_mta_usage(cookie);
    });
    releaser.join();
    EXPECT_EQ(result, com::S_OK);
    EXPECT_EQ(mgr.mta_usage_count(), 0u);
}

TEST_F(ComCoreTest, MTAUsagePreservesExistingStaApartment) {
    ASSERT_EQ(mgr.co_initialize_ex(com::COINIT_APARTMENTTHREADED), com::S_OK);
    GuestAddr cookie = 0;
    ASSERT_EQ(mgr.co_increment_mta_usage(&cookie), com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED),
              com::RPC_E_CHANGED_MODE);
    EXPECT_EQ(mgr.co_decrement_mta_usage(cookie), com::S_OK);
    EXPECT_TRUE(mgr.is_initialized());
}

TEST_F(ComCoreTest, MTAUsageJoinsUninitializedCallerToMta) {
    GuestAddr cookie = 0;
    ASSERT_EQ(mgr.co_increment_mta_usage(&cookie), com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(com::COINIT_MULTITHREADED), com::S_FALSE);
    EXPECT_EQ(mgr.init_count(), 1u);
    EXPECT_EQ(mgr.co_decrement_mta_usage(cookie), com::S_OK);
    EXPECT_TRUE(mgr.is_initialized());
    mgr.co_uninitialize();
    EXPECT_FALSE(mgr.is_initialized());
}

// =============================================================================
// Thread apartment publication (TEB.ReservedForOle)
//
// Offsets and flag values below were measured on a real Windows host by
// snapshotting the TEB across CoInitializeEx / CoUninitialize; guest code
// (Chromium's GetComApartmentTypeForThread) reads them directly instead of
// calling CoGetApartmentType.
// =============================================================================

namespace {

constexpr GuestAddr kTebA = 0x40000000;
constexpr GuestAddr kTebB = 0x40010000;
constexpr u32 kTebReservedForOle64 = 0x1758;
constexpr u32 kOleTlsApartmentFlags64 = 0x14;

class ComApartmentTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kTebA, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kTebB, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(0x50000, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    /// A Marshaler standing on the guest thread whose TEB is @p teb.
    struct Thread {
        cpu::CpuState cpu;
        std::unique_ptr<Marshaler> m;
    };

    std::unique_ptr<Thread> thread_at(GuestAddr teb) {
        auto t = std::make_unique<Thread>();
        t->cpu.init_long_mode();
        t->cpu.gpr.rsp = 0x51000;
        t->cpu.seg.gs_desc.base = teb;
        t->m = std::make_unique<Marshaler>(memory);
        t->m->attach_cpu(t->cpu);
        return t;
    }

    /// Read TEB.ReservedForOle -> OleTlsData.apartment_flags, 0 when unset.
    u32 published_flags(GuestAddr teb) {
        const GuestAddr block = static_cast<GuestAddr>(
            memory.read_u64(teb + kTebReservedForOle64).value_or(0));
        if (block == 0) return 0;
        return memory.read_u32(block + kOleTlsApartmentFlags64).value_or(0);
    }

    GuestMemory memory;
    ComCoreManager mgr;
};

}  // namespace

// Learn "CoGetApartmentType" / APTTYPEQUALIFIER_IMPLICIT_MTA and
// "CoIncrementMTAUsage" (keeps MTA support active when no MTA threads are
// running); s7_winrt_probe.expected "implicit-MTA thread -> 0". A thread that
// never initialized COM is in the MTA exactly while the process MTA exists.
namespace {
constexpr u32 kCoENotInitialized = 0x800401F0u;  // winerror.h
constexpr u32 kAptTypeCurrent = 0xFFFFFFFFu;
constexpr u32 kAptTypeMta = 1u;
constexpr u32 kQualifierNone = 0u;
constexpr u32 kQualifierImplicitMta = 1u;
constexpr GuestAddr kTypeOut = 0x50100;
constexpr GuestAddr kQualifierOut = 0x50108;
}  // namespace

TEST_F(ComApartmentTest, UninitializedThreadJoinsMtaOnlyWhileTheMtaExists) {
    auto worker = thread_at(kTebB);
    auto apartment_type = [&](u32& type, u32& qualifier) {
        worker->cpu.gpr.rcx = kTypeOut;
        worker->cpu.gpr.rdx = kQualifierOut;
        const u32 hr = mgr.co_get_apartment_type(*worker->m);
        type = memory.read_u32(kTypeOut).value_or(0xDEAD);
        qualifier = memory.read_u32(kQualifierOut).value_or(0xDEAD);
        return hr;
    };
    u32 type = 0, qualifier = 0;

    // No MTA anywhere: outside every apartment.
    EXPECT_FALSE(mgr.thread_in_apartment(*worker->m));
    EXPECT_EQ(apartment_type(type, qualifier), kCoENotInitialized);
    EXPECT_EQ(type, kAptTypeCurrent);
    EXPECT_EQ(qualifier, kQualifierNone);

    // Another thread holds the MTA.
    auto mta = thread_at(kTebA);
    ASSERT_EQ(mgr.co_initialize_ex(*mta->m, com::COINIT_MULTITHREADED), com::S_OK);
    EXPECT_TRUE(mgr.thread_in_apartment(*worker->m));
    EXPECT_EQ(apartment_type(type, qualifier), com::S_OK);
    EXPECT_EQ(type, kAptTypeMta);
    EXPECT_EQ(qualifier, kQualifierImplicitMta);

    // Its last CoUninitialize ends the MTA again.
    mgr.co_uninitialize(*mta->m);
    EXPECT_FALSE(mgr.thread_in_apartment(*worker->m));
    EXPECT_EQ(apartment_type(type, qualifier), kCoENotInitialized);

    // CoIncrementMTAUsage keeps the MTA with no MTA thread running.
    GuestAddr cookie = 0;
    ASSERT_EQ(mgr.co_increment_mta_usage(&cookie), com::S_OK);
    EXPECT_TRUE(mgr.thread_in_apartment(*worker->m));
    EXPECT_EQ(apartment_type(type, qualifier), com::S_OK);
    EXPECT_EQ(type, kAptTypeMta);
    EXPECT_EQ(qualifier, kQualifierImplicitMta);
    ASSERT_EQ(mgr.co_decrement_mta_usage(cookie), com::S_OK);
    EXPECT_FALSE(mgr.thread_in_apartment(*worker->m));
}

// The audio engine's work-queue threads are MTA threads for the life of the
// process (Learn "Media Foundation and COM"), so once the engine starts the
// MTA never ends, and starting it twice holds it once.
TEST_F(ComApartmentTest, AudioEngineHoldsTheMtaForTheProcessLifetime) {
    auto worker = thread_at(kTebB);
    EXPECT_FALSE(mgr.thread_in_apartment(*worker->m));
    mgr.hold_audio_engine_mta_usage();
    mgr.hold_audio_engine_mta_usage();
    EXPECT_EQ(mgr.mta_usage_count(), 1u);
    EXPECT_TRUE(mgr.thread_in_apartment(*worker->m));
}

TEST_F(ComApartmentTest, MtaAndStaPublishMeasuredFlagsInTeb) {
    auto mta = thread_at(kTebA);
    auto sta = thread_at(kTebB);

    EXPECT_EQ(published_flags(kTebA), 0u);  // nothing before CoInitializeEx

    ASSERT_EQ(mgr.co_initialize_ex(*mta->m, com::COINIT_MULTITHREADED),
              com::S_OK);
    EXPECT_EQ(published_flags(kTebA), 0x141u);

    ASSERT_EQ(mgr.co_initialize_ex(*sta->m, com::COINIT_APARTMENTTHREADED),
              com::S_OK);
    EXPECT_EQ(published_flags(kTebB), 0x081u);

    // Each thread keeps its own block, and one does not disturb the other.
    EXPECT_EQ(published_flags(kTebA), 0x141u);
    EXPECT_NE(memory.read_u64(kTebA + kTebReservedForOle64).value_or(0),
              memory.read_u64(kTebB + kTebReservedForOle64).value_or(0));
}

TEST_F(ComApartmentTest, StaWithDisableOle1DdeSetsExtraFlag) {
    auto sta = thread_at(kTebA);
    ASSERT_EQ(mgr.co_initialize_ex(*sta->m, com::COINIT_APARTMENTTHREADED |
                                            com::COINIT_DISABLE_OLE1DDE),
              com::S_OK);
    EXPECT_EQ(published_flags(kTebA), 0x0C1u);
}

TEST_F(ComApartmentTest, OneThreadsMtaDoesNotBlockAnotherThreadsSta) {
    // Process-wide apartment state made this return RPC_E_CHANGED_MODE.
    auto a = thread_at(kTebA);
    auto b = thread_at(kTebB);
    ASSERT_EQ(mgr.co_initialize_ex(*a->m, com::COINIT_MULTITHREADED),
              com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(*b->m, com::COINIT_APARTMENTTHREADED),
              com::S_OK);
}

TEST_F(ComApartmentTest, ExtraCoinitBitsDoNotChangeTheApartment) {
    // Measured: only COINIT_APARTMENTTHREADED (0x2) selects the apartment, so
    // re-entering the same model with extra bits is S_FALSE, not a mode change.
    auto t = thread_at(kTebA);
    ASSERT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_MULTITHREADED),
              com::S_OK);
    EXPECT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_MULTITHREADED |
                                          com::COINIT_DISABLE_OLE1DDE),
              com::S_FALSE);
    EXPECT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_MULTITHREADED |
                                          com::COINIT_SPEED_OVER_MEMORY),
              com::S_FALSE);
    EXPECT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_APARTMENTTHREADED),
              com::RPC_E_CHANGED_MODE);
    EXPECT_EQ(published_flags(kTebA), 0x141u);
}

TEST_F(ComApartmentTest, LastUninitializeClearsApartmentBitsButKeepsBlock) {
    auto t = thread_at(kTebA);
    ASSERT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_MULTITHREADED),
              com::S_OK);
    ASSERT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_MULTITHREADED),
              com::S_FALSE);
    const GuestAddr block =
        static_cast<GuestAddr>(memory.read_u64(kTebA + kTebReservedForOle64).value_or(0));
    ASSERT_NE(block, 0u);

    mgr.co_uninitialize(*t->m);
    EXPECT_EQ(published_flags(kTebA), 0x141u);  // still one initialize to undo
    mgr.co_uninitialize(*t->m);
    EXPECT_EQ(published_flags(kTebA), 0x001u);  // OLETLS_LOCALTID only
    // Windows leaves the block allocated and reuses it.
    EXPECT_EQ(memory.read_u64(kTebA + kTebReservedForOle64).value_or(0), block);

    ASSERT_EQ(mgr.co_initialize_ex(*t->m, com::COINIT_APARTMENTTHREADED),
              com::S_OK);
    EXPECT_EQ(published_flags(kTebA), 0x081u);
    EXPECT_EQ(memory.read_u64(kTebA + kTebReservedForOle64).value_or(0), block);
}

TEST_F(ComApartmentTest, RoInitializeEntersTheSameApartmentAsCoInitializeEx) {
    // Measured on real Windows: RoInitialize(RO_INIT_MULTITHREADED) publishes
    // 0x00200141 and RoInitialize(RO_INIT_SINGLETHREADED) 0x002000C1 — the
    // CoInitializeEx flags plus a WinRT marker — and the two share one count.
    auto mta = thread_at(kTebA);
    ASSERT_EQ(mgr.ro_initialize(*mta->m, /*RO_INIT_MULTITHREADED=*/1),
              com::S_OK);
    EXPECT_EQ(published_flags(kTebA), 0x00200141u);
    EXPECT_EQ(mgr.co_initialize_ex(*mta->m, com::COINIT_MULTITHREADED),
              com::S_FALSE);
    EXPECT_EQ(mgr.co_initialize_ex(*mta->m, com::COINIT_APARTMENTTHREADED),
              com::RPC_E_CHANGED_MODE);
    mgr.co_uninitialize(*mta->m);
    mgr.co_uninitialize(*mta->m);
    EXPECT_EQ(published_flags(kTebA), 0x001u);

    auto sta = thread_at(kTebB);
    ASSERT_EQ(mgr.ro_initialize(*sta->m, /*RO_INIT_SINGLETHREADED=*/0),
              com::S_OK);
    EXPECT_EQ(published_flags(kTebB), 0x002000C1u);
}

TEST_F(ComApartmentTest, RoInitializeRejectsUnknownInitType) {
    auto t = thread_at(kTebA);
    EXPECT_EQ(mgr.ro_initialize(*t->m, 7), com::E_INVALIDARG);
    EXPECT_EQ(published_flags(kTebA), 0u);
}

// 6. CoCreateInstance always returns CLASS_E_CLASSNOTAVAILABLE
TEST_F(ComCoreTest, CoCreateInstance_ReturnsClassNotAvailable) {
    EXPECT_EQ(mgr.co_create_instance(), com::CLASS_E_CLASSNOTAVAILABLE);
}

// 7. CoTaskMemAlloc returns non-null for size 100
TEST_F(ComCoreTest, CoTaskMemAlloc_ReturnsNonNull) {
    void* p = mgr.co_task_mem_alloc(100);
    EXPECT_NE(p, nullptr);
    mgr.co_task_mem_free(p);
}

// 8. CoTaskMemFree after alloc does not crash
TEST_F(ComCoreTest, CoTaskMemFree_DoesNotCrash) {
    void* p = mgr.co_task_mem_alloc(64);
    ASSERT_NE(p, nullptr);
    mgr.co_task_mem_free(p);
    // Also free nullptr — should be safe
    mgr.co_task_mem_free(nullptr);
}

// 9. CoTaskMemRealloc grows an allocation
TEST_F(ComCoreTest, CoTaskMemRealloc_GrowsAlloc) {
    void* p = mgr.co_task_mem_alloc(10);
    ASSERT_NE(p, nullptr);
    void* p2 = mgr.co_task_mem_realloc(p, 100);
    EXPECT_NE(p2, nullptr);
    mgr.co_task_mem_free(p2);
}

// 10. CoTaskMemAlloc(0) returns a valid (non-null) pointer
TEST_F(ComCoreTest, CoTaskMemAlloc_ZeroSize_ReturnsValid) {
    void* p = mgr.co_task_mem_alloc(0);
    EXPECT_NE(p, nullptr);
    mgr.co_task_mem_free(p);
}

// ---------------------------------------------------------------------------
// CoCreateFreeThreadedMarshaler: the FTM answers IID_IMarshal (Learn: the
// outer object delegates QueryInterface(IID_IMarshal) to it) and marshals
// an in-process interface pointer through the stream (PEAK audit 08ab).
// ---------------------------------------------------------------------------
class FreeThreadedMarshalerTest : public ::testing::Test {
protected:
    static constexpr GuestAddr DATA = 0x00100000;
    static constexpr GuestAddr STACK = 0x00200000;
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(ComCoreManager::COM_SCRATCH_BASE, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        ASSERT_TRUE(registry.register_dll("ole32.dll", create_com_core_thunks(manager)).is_ok());
        manager.set_thunk_registry(&registry);
        exports = create_com_core_thunks(manager);
    }
    u32 call(const char* name, std::initializer_list<u32> args) {
        const ExportEntry* entry = nullptr;
        for (const auto& e : exports.entries) if (e.name == name) entry = &e;
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0xFFFFFFFFu;
        const GuestAddr frame = STACK + 0x400;
        cpu.set_sp(frame);
        (void)memory.write_u32(frame, 0xDEADBEEF);
        usize i = 0;
        for (u32 v : args) (void)memory.write_u32(frame + 4 + 4 * i++, v);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        entry->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_manager{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_manager};
    cpu::CpuState cpu;
    ComCoreManager manager{task_memory};
    ThunkRegistry registry{0x70000000};
    DllExports exports;
};

TEST_F(FreeThreadedMarshalerTest, AnswersIMarshalAndMarshalsInProcess) {
    constexpr GuestAddr out = DATA, marshal_out = DATA + 4, iid = DATA + 0x10;
    ASSERT_EQ(call("CoCreateFreeThreadedMarshaler", {0, out}), com::S_OK);
    const u32 ftm = memory.read_u32(out).value();
    ASSERT_NE(ftm, 0u);
    // IID_IMarshal {00000003-0000-0000-C000-000000000046}
    const u8 imarshal[16] = {3, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
    (void)memory.write_bytes(iid, imarshal, 16);
    ASSERT_EQ(call("Vitreo_FreeThreadedMarshaler_QueryInterface", {ftm, iid, marshal_out}),
              com::S_OK);
    const u32 marshal = memory.read_u32(marshal_out).value();
    ASSERT_NE(marshal, 0u);
    ASSERT_NE(marshal, ftm);
    // GetUnmarshalClass(riid, pv, MSHCTX_INPROC, NULL, NORMAL, pCid)
    const GuestAddr cid = DATA + 0x40;
    ASSERT_EQ(call("Vitreo_FtmMarshal_GetUnmarshalClass",
                   {marshal, iid, 0x1234, 3, 0, 0, cid}), com::S_OK);
    EXPECT_EQ(memory.read_u32(cid).value(), 0x0000033Au); // CLSID_InProcFreeMarshaler
    // Cross-process contexts need the standard marshaler: refused loudly.
    EXPECT_EQ(call("Vitreo_FtmMarshal_GetUnmarshalClass",
                   {marshal, iid, 0x1234, 0 /*MSHCTX_LOCAL*/, 0, 0, cid}), com::E_NOTIMPL);

    // MarshalInterface writes the pointer through IStream::Write and AddRefs;
    // UnmarshalInterface reads it back, QIs, and consumes the NORMAL ref.
    constexpr GuestAddr object = DATA + 0x100, object_vtbl = DATA + 0x140;
    constexpr GuestAddr stream = DATA + 0x200, stream_vtbl = DATA + 0x240;
    constexpr GuestAddr kQi = 0x401000, kAddRef = 0x402000, kRelease = 0x403000;
    constexpr GuestAddr kRead = 0x404000, kWrite = 0x405000;
    (void)memory.write_u32(object, object_vtbl);
    (void)memory.write_u32(object_vtbl, kQi);
    (void)memory.write_u32(object_vtbl + 4, kAddRef);
    (void)memory.write_u32(object_vtbl + 8, kRelease);
    (void)memory.write_u32(stream, stream_vtbl);
    (void)memory.write_u32(stream_vtbl + 12, kRead);
    (void)memory.write_u32(stream_vtbl + 16, kWrite);
    std::vector<u8> packet;
    int addrefs = 0, releases = 0, qis = 0;
    manager.set_guest_call([&](GuestAddr fn, const u64* args, usize, bool) -> u64 {
        if (fn == kAddRef) return ++addrefs;
        if (fn == kRelease) return ++releases;
        if (fn == kQi) { ++qis; (void)memory.write_u32(static_cast<GuestAddr>(args[2]), object); return 0; }
        if (fn == kWrite) {
            packet.resize(args[2]);
            (void)memory.read_bytes(static_cast<GuestAddr>(args[1]), packet.data(), packet.size());
            return 0;
        }
        if (fn == kRead) {
            (void)memory.write_bytes(static_cast<GuestAddr>(args[1]), packet.data(), packet.size());
            return 0;
        }
        return 0x80004001u;
    });
    ASSERT_EQ(call("Vitreo_FtmMarshal_MarshalInterface",
                   {marshal, stream, iid, object, 3, 0, 0 /*NORMAL*/}), com::S_OK);
    EXPECT_EQ(addrefs, 1);
    const GuestAddr unmarshaled = DATA + 0x60;
    ASSERT_EQ(call("Vitreo_FtmMarshal_UnmarshalInterface", {marshal, stream, iid, unmarshaled}),
              com::S_OK);
    EXPECT_EQ(memory.read_u32(unmarshaled).value(), object);
    EXPECT_EQ(qis, 1);
    EXPECT_EQ(releases, 1);
}

TEST_F(FreeThreadedMarshalerTest, OleInitializeNullEntersAnSta) {
    // Learn OleInitialize: the (only valid) NULL argument initialises an STA,
    // so a later CoInitializeEx(COINIT_MULTITHREADED) is RPC_E_CHANGED_MODE.
    EXPECT_EQ(call("OleInitialize", {0}), com::S_OK);
    EXPECT_EQ(call("CoInitializeEx", {0, com::COINIT_MULTITHREADED}), com::RPC_E_CHANGED_MODE);
    (void)call("OleUninitialize", {});
}

// CoGetCurrentProcess (Learn: unique to the calling thread, never reused;
// tests/oracle_data/cogetcurrentprocess_probe{32,64}.expected): a thread's
// value is fixed across calls, and every new thread gets a new one even when
// the previous thread has exited.
TEST_F(FreeThreadedMarshalerTest, CoGetCurrentProcessIsPerThreadAndNeverReused) {
    const u32 main_first = call("CoGetCurrentProcess", {});
    EXPECT_NE(main_first, 0u);
    EXPECT_EQ(call("CoGetCurrentProcess", {}), main_first);
    std::vector<u32> seen{main_first};
    for (int i = 0; i < 3; ++i) {
        u32 a = 0, b = 0;
        std::thread([&] {
            const ExportEntry* entry = nullptr;
            for (const auto& e : exports.entries) if (e.name == std::string("CoGetCurrentProcess")) entry = &e;
            ASSERT_NE(entry, nullptr);
            cpu::CpuState thread_cpu;
            thread_cpu.init_protected_mode();
            Marshaler m(memory);
            m.attach_cpu(thread_cpu);
            entry->handler(m);
            a = static_cast<u32>(thread_cpu.gpr.rax);
            entry->handler(m);
            b = static_cast<u32>(thread_cpu.gpr.rax);
        }).join();
        EXPECT_EQ(a, b);
        EXPECT_EQ(std::count(seen.begin(), seen.end(), a), 0);
        seen.push_back(a);
    }
    EXPECT_EQ(call("CoGetCurrentProcess", {}), main_first);
}
