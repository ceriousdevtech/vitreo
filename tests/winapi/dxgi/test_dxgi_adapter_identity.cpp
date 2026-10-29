// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"
#include "capsule/registry/registry.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "winapi/common/display_device_identity.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace vitreo::winapi::dxgi_test {
namespace {

// -----------------------------------------------------------------------------
// Advertised adapter identity (per-capsule configuration)
// -----------------------------------------------------------------------------

/// Installs an advertised identity for one test and restores the previous one.
class ScopedAdapterIdentity {
public:
    explicit ScopedAdapterIdentity(
        display_device_identity::AdapterIdentity identity)
        : saved_(display_device_identity::adapter_identity()) {
        ::unsetenv("VITREO_DXGI_ADAPTER_ID");
        ::unsetenv("VITREO_DXGI_DRIVER_VERSION");
        display_device_identity::set_adapter_identity(std::move(identity));
    }
    ScopedAdapterIdentity(const ScopedAdapterIdentity&) = delete;
    ScopedAdapterIdentity& operator=(const ScopedAdapterIdentity&) = delete;
    ~ScopedAdapterIdentity() {
        display_device_identity::set_adapter_identity(saved_);
    }

private:
    display_device_identity::AdapterIdentity saved_;
};

// Without configuration DXGI keeps advertising the truthful Apple vendor ID
// and no device/subsystem/revision. The description is configured here only so
// that an adapter enumerates at all -- this host has no Metal device inside the
// unit-test process, and an adapter with no description is reported as absent.
// The PCI fields are the assertion: they must stay at the detected defaults.
TEST_F(DxgiManagerTest, AdapterDescDefaultIsTheDetectedAppleIdentity) {
    display_device_identity::AdapterIdentity description_only;
    description_only.description = "Test Apple GPU";
    const ScopedAdapterIdentity scoped{description_only};
    detected_manager.refresh_adapter_info();
    const GuestAddr adapter =
        enum_adapter(detected_manager, create_factory(detected_manager));
    ASSERT_NE(adapter, 0u);

    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    Marshaler describe(memory);
    describe.attach_cpu(cpu);
    detected_manager.impl_GetDesc1(describe);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 256).value(),
              dxgi::VENDOR_ID_APPLE);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 260).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 264).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 268).value(), 0u);
}

// A backend with only what detect_adapter_info and QueryVideoMemoryInfo read:
// a unified-memory GPU (the Apple-silicon host fact) with a working set and a
// live budget below it.
class WorkingSetBackend final : public graphics::GraphicsBackendDevice {
public:
    static constexpr u64 kWorkingSet = 21ull * 1024 * 1024 * 1024 + 0x5550000;
    static constexpr u64 kLiveBudget = 17ull * 1024 * 1024 * 1024;
    static constexpr u64 kUsage = 640ull * 1024 * 1024;
    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string backend_device_name() const override {
        return "Working Set GPU";
    }
    [[nodiscard]] void* native_device() const noexcept override { return nullptr; }
    [[nodiscard]] void* native_command_queue() const noexcept override {
        return nullptr;
    }
    [[nodiscard]] bool has_unified_memory() const override { return true; }
    [[nodiscard]] u64 recommended_working_set_size() const noexcept override {
        return kWorkingSet;
    }
    [[nodiscard]] bool query_video_memory(
        graphics::BackendVideoMemoryInfo& info) const noexcept override {
        info = {kLiveBudget, kUsage};
        return true;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendBuffer> create_buffer(
        const graphics::BackendBufferDesc&, const void*) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendTexture2D> create_texture_2d(
        const graphics::BackendTexture2DDesc&, const void*, unsigned) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendSwapChain> create_swap_chain(
        const graphics::BackendSwapChainDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendPipeline> create_pipeline(
        const graphics::BackendPipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendComputePipeline>
    create_compute_pipeline(const graphics::BackendComputePipelineDesc&) override {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<graphics::BackendRenderPass>
    create_render_pass_standalone(const graphics::BackendRenderPassDesc&) override {
        return nullptr;
    }
    [[nodiscard]] bool dispatch(const graphics::BackendDispatchCall&) override {
        return false;
    }
    [[nodiscard]] bool copy_buffer(graphics::BackendBuffer*,
                                   const graphics::BackendBuffer*) override {
        return false;
    }
    [[nodiscard]] bool copy_texture_2d(graphics::BackendTexture2D*,
                                       const graphics::BackendTexture2D*) override {
        return false;
    }
    [[nodiscard]] bool generate_mips(graphics::BackendTexture2D*) override {
        return false;
    }
    void flush() override {}
    [[nodiscard]] bool supports_texture_sample_count(u32 count) const override {
        return count == 1;
    }
    [[nodiscard]] u64 max_buffer_length() const override { return 1ull << 34; }
};

/// Installs `device` as the process's active graphics backend for one test,
/// the way the composition root does at start-up, and clears it afterwards.
struct ScopedActiveBackend {
    explicit ScopedActiveBackend(graphics::GraphicsBackendDevice* device) {
        graphics::set_active_backend_device(device);
    }
    ScopedActiveBackend(const ScopedActiveBackend&) = delete;
    ScopedActiveBackend& operator=(const ScopedActiveBackend&) = delete;
    ~ScopedActiveBackend() { graphics::set_active_backend_device(nullptr); }
};

// Adapter profile policy (.docs/contracts/dxgi.md "Adapter profile policy
// (discrete-class)"): on a unified-memory host the detected adapter still
// describes a discrete-class GPU. The working set is DedicatedVideoMemory,
// nothing is dedicated system or shared system memory, and the budgets agree
// with that: LOCAL is the dedicated pool (live Budget <= DedicatedVideoMemory,
// CurrentUsage the backend's), NON_LOCAL -- the system-memory pool behind
// SharedSystemMemory (Learn D3D12_MEMORY_POOL, non-UMA) -- is empty.
TEST_F(DxgiManagerTest, DetectedAdapterIsDiscreteClassOnAUnifiedMemoryHost) {
    WorkingSetBackend backend;
    const ScopedActiveBackend active{&backend};
    const ScopedAdapterIdentity scoped{{}};
    detected_manager.refresh_adapter_info();
    const GuestAddr adapter =
        enum_adapter(detected_manager, create_factory(detected_manager));
    ASSERT_NE(adapter, 0u);

    Marshaler m(memory);
    m.attach_cpu(cpu);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    detected_manager.impl_GetDesc1(m);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const u64 dedicated = memory.read_u64(DESC_ADDRESS + 272).value();
    const u64 shared = memory.read_u64(DESC_ADDRESS + 288).value();
    EXPECT_EQ(dedicated, WorkingSetBackend::kWorkingSet)
        << "DedicatedVideoMemory is the working set";
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 280).value(), 0u)
        << "DedicatedSystemMemory";
    EXPECT_EQ(shared, 0u) << "SharedSystemMemory";
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 304).value(), 0u) << "Flags";

    constexpr GuestAddr INFO = DESC_ADDRESS + 0x200;
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;  // DXGI_MEMORY_SEGMENT_GROUP_LOCAL
    cpu.gpr.r9 = INFO;
    detected_manager.impl_QueryVideoMemoryInfo(m);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(INFO).value(), WorkingSetBackend::kLiveBudget);
    EXPECT_LE(memory.read_u64(INFO).value(), dedicated);
    EXPECT_EQ(memory.read_u64(INFO + 8).value(), WorkingSetBackend::kUsage);
    EXPECT_LE(memory.read_u64(INFO + 16).value(), memory.read_u64(INFO).value())
        << "AvailableForReservation <= Budget";
    EXPECT_EQ(memory.read_u64(INFO + 24).value(), 0u);

    cpu.gpr.r8 = 1;  // DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL
    detected_manager.impl_QueryVideoMemoryInfo(m);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(INFO).value(), shared);
    for (u32 at = 0; at < 32; at += 8) {
        EXPECT_EQ(memory.read_u64(INFO + at).value(), 0u) << at;
    }
}

// The per-capsule configuration surface itself: an untouched capsule registry
// must resolve to the all-empty identity, i.e. every surface keeps its own
// truthful default. It must also publish the honest driver version so a guest
// reading the registry sees what DXGI reports.
TEST(AdapterIdentityRegistry, UnconfiguredCapsuleLeavesEveryDefaultInPlace) {
    namespace identity = display_device_identity;
    const ScopedAdapterIdentity scoped{{}};
    capsule::Registry registry;
    identity::configure_adapter_identity_from_registry(registry);

    const auto& resolved = identity::adapter_identity();
    EXPECT_FALSE(resolved.vendor_id.has_value());
    EXPECT_FALSE(resolved.device_id.has_value());
    EXPECT_FALSE(resolved.subsystem_id.has_value());
    EXPECT_FALSE(resolved.revision.has_value());
    EXPECT_FALSE(resolved.description.has_value());
    EXPECT_FALSE(resolved.driver_version.has_value());
    EXPECT_FALSE(resolved.has_pci_identity());

    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 identity::ADAPTER_CLASS_KEY, false);
    ASSERT_TRUE(key.is_ok());
    auto version = registry.query_value(key.value(), L"DriverVersion");
    ASSERT_TRUE(version.is_ok());
    EXPECT_EQ(std::get<std::wstring>(version.value().data),
              L"32.0.101.5972");
    auto matching = registry.query_value(key.value(), L"MatchingDeviceId");
    ASSERT_TRUE(matching.is_ok());
    EXPECT_EQ(std::get<std::wstring>(matching.value().data),
              L"root\\vitreo_metal");
}

// A configured capsule resolves the whole tuple and republishes it as the
// informational values a guest can read straight out of the registry.
TEST(AdapterIdentityRegistry, ConfiguredValuesResolveAndArePublished) {
    namespace identity = display_device_identity;
    const ScopedAdapterIdentity scoped{{}};
    // A detected adapter with a different name is present, and the mirrors
    // carry a stale value from an earlier identity: the configured DriverDesc
    // outranks the detected name and replaces the stale mirrors.
    WorkingSetBackend backend;
    const ScopedActiveBackend active{&backend};
    capsule::Registry registry;
    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 identity::ADAPTER_CLASS_KEY, true);
    ASSERT_TRUE(key.is_ok());
    for (const wchar_t* mirror : {L"HardwareInformation.AdapterString",
                                  L"HardwareInformation.ChipType"}) {
        ASSERT_TRUE(registry.set_value(
            key.value(), mirror,
            capsule::RegValue{capsule::RegType::Sz,
                              std::wstring(L"Intel(R) Arc(TM) Graphics")})
                        .is_ok());
    }
    const auto set_dword = [&](const wchar_t* name, u32 value) {
        ASSERT_TRUE(registry.set_value(
            key.value(), name,
            capsule::RegValue{capsule::RegType::Dword, value}).is_ok());
    };
    set_dword(L"VideoPciVendorID", 0x8086u);
    set_dword(L"VideoPciDeviceID", 0x7D55u);
    set_dword(L"VideoPciSubSystemID", 0x2212103Cu);
    set_dword(L"VideoPciRevisionID", 0x08u);
    ASSERT_TRUE(registry.set_value(
        key.value(), L"DriverDesc",
        capsule::RegValue{capsule::RegType::Sz,
                          std::wstring(L"Configured Test Adapter")}).is_ok());
    ASSERT_TRUE(registry.set_value(
        key.value(), L"DriverVersion",
        capsule::RegValue{capsule::RegType::Sz,
                          std::wstring(L"31.0.101.4502")}).is_ok());

    identity::configure_adapter_identity_from_registry(registry);
    const auto& resolved = identity::adapter_identity();
    EXPECT_EQ(resolved.vendor_id.value_or(0), 0x8086u);
    EXPECT_EQ(resolved.device_id.value_or(0), 0x7D55u);
    EXPECT_EQ(resolved.subsystem_id.value_or(0), 0x2212103Cu);
    EXPECT_EQ(resolved.revision.value_or(0), 0x08u);
    ASSERT_TRUE(resolved.description.has_value());
    EXPECT_EQ(*resolved.description, "Configured Test Adapter");
    ASSERT_TRUE(resolved.driver_version.has_value());
    EXPECT_EQ(identity::format_driver_version(*resolved.driver_version),
              "31.0.101.4502");

    auto matching = registry.query_value(key.value(), L"MatchingDeviceId");
    ASSERT_TRUE(matching.is_ok());
    EXPECT_EQ(std::get<std::wstring>(matching.value().data),
              L"pci\\ven_8086&dev_7d55&subsys_2212103c&rev_08");
    for (const wchar_t* mirror : {L"HardwareInformation.AdapterString",
                                  L"HardwareInformation.ChipType"}) {
        auto value = registry.query_value(key.value(), mirror);
        ASSERT_TRUE(value.is_ok());
        EXPECT_EQ(value.value().type, capsule::RegType::Sz);
        EXPECT_EQ(std::get<std::wstring>(value.value().data),
                  L"Configured Test Adapter");
    }
}

// -----------------------------------------------------------------------------
// HardwareInformation.* mirrors always describe the RESOLVED identity
// -----------------------------------------------------------------------------

/// The REG_* type tokens the oracle printed for `value_name` across every
/// display-class instance that carries it, replayed from
/// tools/oracle_probe/display_class_hardware_info_probe.expected (Windows:
/// a Red Hat QXL display driver and an NVIDIA WDDM driver, 32.0.15.6094).
/// Learn ("Registering Hardware Information") says only "null-terminated
/// string" and the archived Device.Graphics certification page says
/// REG_BINARY; the drivers actually shipping on Windows write the type below.
std::set<std::string> oracle_value_types(const std::string& value_name) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) +
                     "/display_class_hardware_info_probe.expected");
    const std::string needle = " " + value_name + " type=";
    std::set<std::string> types;
    for (std::string line; std::getline(in, line);) {
        const auto at = line.find(needle);
        if (at == std::string::npos) continue;
        const auto begin = at + needle.size();
        types.insert(line.substr(begin, line.find(' ', begin) - begin));
    }
    return types;
}

constexpr const wchar_t* kMirrorNames[] = {
    L"HardwareInformation.AdapterString", L"HardwareInformation.ChipType"};

void seed_stale_mirrors(capsule::Registry& registry) {
    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 display_device_identity::ADAPTER_CLASS_KEY,
                                 true);
    ASSERT_TRUE(key.is_ok());
    for (const wchar_t* mirror : kMirrorNames) {
        ASSERT_TRUE(registry.set_value(
            key.value(), mirror,
            capsule::RegValue{capsule::RegType::Sz,
                              std::wstring(L"Intel(R) Arc(TM) Graphics")})
                        .is_ok());
    }
    registry.close_key(key.value());
}

// The value names and registry types are Windows' own. The oracle shows REG_SZ
// for both on every adapter that publishes them (and REG_DWORD MemorySize /
// REG_QWORD qwMemorySize, which Vitreo does not publish: see
// .docs/graphics-subsystem.md).
TEST(AdapterIdentityRegistry, MirrorValueTypesAreTheOraclesRegistryTypes) {
    EXPECT_EQ(oracle_value_types("HardwareInformation.AdapterString"),
              std::set<std::string>{"REG_SZ"});
    EXPECT_EQ(oracle_value_types("HardwareInformation.ChipType"),
              std::set<std::string>{"REG_SZ"});

    namespace identity = display_device_identity;
    const ScopedAdapterIdentity scoped{{}};
    WorkingSetBackend backend;
    const ScopedActiveBackend active{&backend};
    capsule::Registry registry;
    identity::configure_adapter_identity_from_registry(registry);
    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 identity::ADAPTER_CLASS_KEY, false);
    ASSERT_TRUE(key.is_ok());
    for (const wchar_t* mirror : kMirrorNames) {
        auto value = registry.query_value(key.value(), mirror);
        ASSERT_TRUE(value.is_ok());
        EXPECT_EQ(value.value().type, capsule::RegType::Sz);
    }
}

// The defect: a hive that still carries mirrors from an earlier identity
// (and no DriverDesc) kept reporting that GPU to a guest reading the registry
// while DXGI reported the real adapter. The mirrors are refreshed on every
// start from the adapter the host actually has.
TEST(AdapterIdentityRegistry, StaleMirrorsBecomeTheDetectedAdapterName) {
    namespace identity = display_device_identity;
    const ScopedAdapterIdentity scoped{{}};
    WorkingSetBackend backend;
    const ScopedActiveBackend active{&backend};
    capsule::Registry registry;
    seed_stale_mirrors(registry);

    identity::configure_adapter_identity_from_registry(registry);
    // The detected name is a mirror, never configuration: the identity every
    // other surface resolves stays unconfigured.
    EXPECT_FALSE(identity::adapter_identity().description.has_value());

    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 identity::ADAPTER_CLASS_KEY, false);
    ASSERT_TRUE(key.is_ok());
    for (const wchar_t* mirror : kMirrorNames) {
        auto value = registry.query_value(key.value(), mirror);
        ASSERT_TRUE(value.is_ok());
        EXPECT_EQ(std::get<std::wstring>(value.value().data),
                  L"Working Set GPU");
    }
    // Republishing must not turn the mirror into a DriverDesc the next start
    // would read back as a configured identity.
    EXPECT_TRUE(registry.query_value(key.value(), L"DriverDesc").is_err());
    identity::configure_adapter_identity_from_registry(registry);
    EXPECT_FALSE(identity::adapter_identity().description.has_value());
}

// With no adapter at all (nothing detected, nothing configured) there is no
// identity to describe, so a stale mirror is removed rather than left behind.
TEST(AdapterIdentityRegistry, StaleMirrorsAreRemovedWhenNoAdapterIsDetected) {
    namespace identity = display_device_identity;
    const ScopedAdapterIdentity scoped{{}};
    capsule::Registry registry;
    seed_stale_mirrors(registry);

    identity::configure_adapter_identity_from_registry(registry);

    auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                 identity::ADAPTER_CLASS_KEY, false);
    ASSERT_TRUE(key.is_ok());
    for (const wchar_t* mirror : kMirrorNames) {
        EXPECT_TRUE(registry.query_value(key.value(), mirror).is_err());
    }
}

// One source of truth: the registry mirror is the very string
// IDXGIAdapter::GetDesc reports, for an unconfigured and a configured capsule.
TEST_F(DxgiManagerTest, RegistryMirrorsEqualTheAdapterDescDescription) {
    namespace identity = display_device_identity;
    WorkingSetBackend backend;
    const ScopedActiveBackend active{&backend};
    for (const bool configured : {false, true}) {
        const ScopedAdapterIdentity scoped{{}};
        capsule::Registry registry;
        seed_stale_mirrors(registry);
        if (configured) {
            auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                         identity::ADAPTER_CLASS_KEY, true);
            ASSERT_TRUE(key.is_ok());
            ASSERT_TRUE(registry.set_value(
                key.value(), L"DriverDesc",
                capsule::RegValue{capsule::RegType::Sz,
                                  std::wstring(L"Configured Test Adapter")})
                            .is_ok());
        }
        identity::configure_adapter_identity_from_registry(registry);
        detected_manager.refresh_adapter_info();

        const GuestAddr adapter =
            enum_adapter(detected_manager, create_factory(detected_manager));
        ASSERT_NE(adapter, 0u);
        cpu.gpr.rcx = adapter;
        cpu.gpr.rdx = DESC_ADDRESS;
        Marshaler describe(memory);
        describe.attach_cpu(cpu);
        detected_manager.impl_GetDesc1(describe);
        ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
        std::wstring description;
        for (usize index = 0; index < 128; ++index) {
            const u16 character =
                memory.read_u16(DESC_ADDRESS + index * 2).value();
            if (character == 0) break;
            description.push_back(static_cast<wchar_t>(character));
        }

        auto key = registry.open_key(capsule::HKEY_LOCAL_MACHINE,
                                     identity::ADAPTER_CLASS_KEY, false);
        ASSERT_TRUE(key.is_ok());
        for (const wchar_t* mirror : kMirrorNames) {
            auto value = registry.query_value(key.value(), mirror);
            ASSERT_TRUE(value.is_ok());
            EXPECT_EQ(std::get<std::wstring>(value.value().data), description);
        }
        EXPECT_EQ(description, configured ? L"Configured Test Adapter"
                                          : L"Working Set GPU");
    }
}

// The configured identity must reach DXGI_ADAPTER_DESC in full, including the
// Description: a Direct3D vendor pair next to an "Apple ..." name is a
// contradiction a guest can observe between two fields of one struct.
TEST_F(DxgiManagerTest, ConfiguredAdapterIdentityReachesAdapterDesc) {
    display_device_identity::AdapterIdentity identity;
    identity.vendor_id = 0x8086u;
    identity.device_id = 0x7D55u;
    identity.subsystem_id = 0x2212103Cu;
    identity.revision = 0x08u;
    identity.description = "Configured Test Adapter";
    const ScopedAdapterIdentity scoped{identity};
    detected_manager.refresh_adapter_info();

    const GuestAddr adapter =
        enum_adapter(detected_manager, create_factory(detected_manager));
    ASSERT_NE(adapter, 0u);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    Marshaler describe(memory);
    describe.attach_cpu(cpu);
    detected_manager.impl_GetDesc1(describe);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);

    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 256).value(), 0x8086u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 260).value(), 0x7D55u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 264).value(), 0x2212103Cu);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 268).value(), 0x08u);

    std::string description;
    for (usize index = 0; index < 128; ++index) {
        const u16 character =
            memory.read_u16(DESC_ADDRESS + index * 2).value();
        if (character == 0) break;
        description.push_back(static_cast<char>(character));
    }
    EXPECT_EQ(description, "Configured Test Adapter");
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
