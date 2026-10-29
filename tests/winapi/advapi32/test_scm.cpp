/// @file test_scm.cpp
/// @brief Unit tests for the advapi32 Service Control Manager (scm.hpp).
///
/// These cover the contracts that the fake stubs used to violate: a service
/// database that actually remembers what was created, handles that are typed
/// and validated, status that reflects reality, and honest failures where
/// Vitreo cannot do the Windows thing.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include "winapi/advapi32/services/scm.hpp"
#include "capsule/registry/registry.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::service_constants;

namespace fs = std::filesystem;

namespace {

ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

} // namespace

// =============================================================================
// Fixture
// =============================================================================

class ScmTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_root = fs::temp_directory_path() /
                 ("vitreo_scm_test_" + std::to_string(::getpid()) + "_" +
                  std::to_string(m_counter++));
        fs::create_directories(m_root);

        m_registry = std::make_unique<capsule::Registry>();
        m_vfs      = std::make_unique<capsule::Vfs>();
        m_scm      = std::make_unique<ScmManager>(*m_registry, *m_vfs);
        m_scm->set_capsule(m_root.string(), "test");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    static ServiceConfig make_config(const std::wstring& name) {
        ServiceConfig config;
        config.name = name;
        config.display_name = L"Test Service";
        config.image_path = L"C:\\Windows\\System32\\svc.exe";
        config.service_type = SERVICE_WIN32_OWN_PROCESS;
        config.start_type = SERVICE_DEMAND_START;
        return config;
    }

    fs::path m_root;
    std::unique_ptr<capsule::Registry> m_registry;
    std::unique_ptr<capsule::Vfs> m_vfs;
    std::unique_ptr<ScmManager> m_scm;
    static inline int m_counter = 0;
};

// =============================================================================
// Database
// =============================================================================

TEST_F(ScmTest, CreateServicePersistsToRegistry) {
    u32 error = 0xFFFF;
    ASSERT_TRUE(m_scm->create_service(make_config(L"TestSvc"), error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);

    // The service must be a real registry entry, where Windows puts it, so a
    // guest that reads the key directly (installers do) sees the same thing.
    auto key = m_registry->open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\TestSvc", false);
    ASSERT_TRUE(key.is_ok());

    auto image = m_registry->query_value(key.value(), L"ImagePath");
    ASSERT_TRUE(image.is_ok());
    EXPECT_EQ(std::get<std::wstring>(image.value().data),
              L"C:\\Windows\\System32\\svc.exe");

    auto type = m_registry->query_value(key.value(), L"Type");
    ASSERT_TRUE(type.is_ok());
    EXPECT_EQ(std::get<u32>(type.value().data), SERVICE_WIN32_OWN_PROCESS);
}

TEST_F(ScmTest, BuiltinSecondaryLogonServiceIsVisibleAndRunning) {
    m_scm->install_builtin_services();

    ASSERT_TRUE(m_scm->service_exists(L"seclogon"));
    auto config = m_scm->query_config(L"SECLOGON");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->display_name, L"Secondary Logon");
    EXPECT_EQ(config->service_type, SERVICE_WIN32_SHARE_PROCESS);
    EXPECT_EQ(config->start_type, SERVICE_DEMAND_START);

    // The entry is persisted because Windows applications also inspect the
    // Services registry tree directly instead of going exclusively through
    // SCM APIs.
    auto key = m_registry->open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\seclogon", false);
    ASSERT_TRUE(key.is_ok());

    const ServiceStatus status = m_scm->query_status(L"seclogon");
    EXPECT_EQ(status.current_state, SERVICE_RUNNING);
    EXPECT_EQ(status.service_type, SERVICE_WIN32_SHARE_PROCESS);
    EXPECT_EQ(status.process_id, static_cast<u32>(::getpid()));
}

TEST_F(ScmTest, InstallingBuiltinServicesIsIdempotent) {
    m_scm->install_builtin_services();
    m_scm->install_builtin_services();

    auto config = m_scm->query_config(L"seclogon");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->display_name, L"Secondary Logon");
}

TEST_F(ScmTest, CreateServiceRejectsDuplicate) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Dup"), error));

    error = 0;
    EXPECT_FALSE(m_scm->create_service(make_config(L"Dup"), error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_EXISTS);
}

TEST_F(ScmTest, CreateServiceRejectsEmptyImagePath) {
    auto config = make_config(L"NoImage");
    config.image_path.clear();
    u32 error = 0;
    EXPECT_FALSE(m_scm->create_service(config, error));
    EXPECT_EQ(error, error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(ScmTest, CreateServiceRejectsPathSeparatorInName) {
    // The name becomes both a registry key and a file name; a separator would
    // escape either namespace.
    u32 error = 0;
    EXPECT_FALSE(m_scm->create_service(make_config(L"bad\\name"), error));
    EXPECT_EQ(error, error_codes::ERROR_INVALID_NAME);
}

TEST_F(ScmTest, QueryConfigRoundTrips) {
    u32 error = 0;
    auto config = make_config(L"Round");
    config.start_type = SERVICE_AUTO_START;
    config.object_name = L"LocalSystem";
    ASSERT_TRUE(m_scm->create_service(config, error));

    auto read = m_scm->query_config(L"Round");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->image_path, config.image_path);
    EXPECT_EQ(read->display_name, L"Test Service");
    EXPECT_EQ(read->start_type, SERVICE_AUTO_START);
    EXPECT_EQ(read->object_name, L"LocalSystem");
}

TEST_F(ScmTest, QueryConfigOnMissingServiceReturnsNullopt) {
    EXPECT_FALSE(m_scm->query_config(L"Nope").has_value());
}

TEST_F(ScmTest, ChangeConfigPreservesUnchangedFields) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Change"), error));

    ServiceConfig update;
    update.name = L"Change";
    // SERVICE_NO_CHANGE for the DWORD fields; empty strings mean "leave alone".
    update.service_type = 0xFFFFFFFFu;
    update.start_type = SERVICE_DISABLED;
    update.error_control = 0xFFFFFFFFu;
    ASSERT_TRUE(m_scm->change_config(update, error));

    auto read = m_scm->query_config(L"Change");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->start_type, SERVICE_DISABLED);
    EXPECT_EQ(read->service_type, SERVICE_WIN32_OWN_PROCESS);  // untouched
    EXPECT_EQ(read->image_path, L"C:\\Windows\\System32\\svc.exe");
}

TEST_F(ScmTest, ChangeConfigOnMissingServiceFails) {
    ServiceConfig update;
    update.name = L"Ghost";
    u32 error = 0;
    EXPECT_FALSE(m_scm->change_config(update, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
}

TEST_F(ScmTest, ChangeServiceConfig2WPersistsDescriptionAndFailureActionsX86) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Optional"), error));
    const u32 service_handle = m_scm->open_service_handle(L"Optional");

    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x12000;
    constexpr GuestAddr description = 0x10100;
    constexpr GuestAddr failure_actions = 0x10200;
    constexpr GuestAddr action = 0x10300;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x4000, MemoryProtection::ReadWrite).is_ok());

    const std::u16string description_text = u"Steam helper service";
    for (usize index = 0; index < description_text.size(); ++index) {
        ASSERT_TRUE(memory.write_u16(
            description + index * sizeof(u16),
            static_cast<u16>(description_text[index])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(
        description + description_text.size() * sizeof(u16), 0).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_advapi32_scm_thunks(*m_scm);
    auto* change = find_export(exports, "ChangeServiceConfig2W");
    ASSERT_NE(change, nullptr);
    EXPECT_EQ(change->stdcall_bytes, 12u);

    // SERVICE_DESCRIPTIONW contains one guest pointer.
    ASSERT_TRUE(memory.write_u32(data, description).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, service_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, data).is_ok());
    change->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    // SteamService uses one SC_ACTION_RESTART with a 60-second delay.
    ASSERT_TRUE(memory.write_u32(failure_actions + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(failure_actions + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(failure_actions + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(failure_actions + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(failure_actions + 16, action).is_ok());
    ASSERT_TRUE(memory.write_u32(action + 0, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(action + 4, 60000).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, failure_actions).is_ok());
    change->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);

    auto key = m_registry->open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Optional", false);
    ASSERT_TRUE(key.is_ok());

    auto stored_description =
        m_registry->query_value(key.value(), L"Description");
    ASSERT_TRUE(stored_description.is_ok());
    EXPECT_EQ(std::get<std::wstring>(stored_description.value().data),
              L"Steam helper service");

    auto stored_actions =
        m_registry->query_value(key.value(), L"FailureActions");
    ASSERT_TRUE(stored_actions.is_ok());
    ASSERT_EQ(stored_actions.value().type, capsule::RegType::Binary);
    const auto& bytes =
        std::get<std::vector<u8>>(stored_actions.value().data);
    ASSERT_GE(bytes.size(), 28u);
    const auto read_le32 = [&bytes](usize offset) {
        return static_cast<u32>(bytes[offset]) |
               (static_cast<u32>(bytes[offset + 1]) << 8) |
               (static_cast<u32>(bytes[offset + 2]) << 16) |
               (static_cast<u32>(bytes[offset + 3]) << 24);
    };
    EXPECT_EQ(read_le32(0), 0u);
    EXPECT_EQ(read_le32(12), 1u);
    EXPECT_EQ(read_le32(16), 20u);
    EXPECT_EQ(read_le32(20), 1u);
    EXPECT_EQ(read_le32(24), 60000u);
}

TEST_F(ScmTest, ChangeServiceConfig2RejectsUnknownLevel) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Level"), error));
    const u32 service_handle = m_scm->open_service_handle(L"Level");

    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x11000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x2000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(stack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_scm_thunks(*m_scm);
    auto* change = find_export(exports, "ChangeServiceConfig2W");
    ASSERT_NE(change, nullptr);

    ASSERT_TRUE(memory.write_u32(stack + 4, service_handle).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 99).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, data).is_ok());
    change->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_LEVEL);
}

TEST_F(ScmTest, DeleteServiceRemovesIt) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Gone"), error));
    ASSERT_TRUE(m_scm->service_exists(L"Gone"));

    ASSERT_TRUE(m_scm->delete_service(L"Gone", error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(m_scm->service_exists(L"Gone"));
}

TEST_F(ScmTest, DeleteMissingServiceFails) {
    u32 error = 0;
    EXPECT_FALSE(m_scm->delete_service(L"Absent", error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
}

// =============================================================================
// Handles
// =============================================================================

TEST_F(ScmTest, ManagerAndServiceHandlesAreDistinctKinds) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Handles"), error));

    const u32 manager = m_scm->open_manager_handle();
    const u32 service = m_scm->open_service_handle(L"Handles");
    EXPECT_NE(manager, service);

    EXPECT_TRUE(m_scm->is_manager_handle(manager));
    EXPECT_FALSE(m_scm->is_manager_handle(service));

    // A manager handle must not resolve as a service handle — that confusion is
    // exactly what let the old stubs accept anything.
    EXPECT_FALSE(m_scm->handle_service_name(manager).has_value());
    auto name = m_scm->handle_service_name(service);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, L"Handles");
}

TEST_F(ScmTest, CloseHandleInvalidatesIt) {
    const u32 handle = m_scm->open_service_handle(L"Any");
    EXPECT_TRUE(m_scm->close_handle(handle));
    EXPECT_FALSE(m_scm->handle_service_name(handle).has_value());
    // Double close must fail, so CloseServiceHandle can report INVALID_HANDLE.
    EXPECT_FALSE(m_scm->close_handle(handle));
}

TEST_F(ScmTest, UnknownHandleResolvesToNothing) {
    EXPECT_FALSE(m_scm->handle_service_name(0xDEADBEEF).has_value());
    EXPECT_FALSE(m_scm->is_manager_handle(0xDEADBEEF));
}

TEST_F(ScmTest, QueryServiceConfigWUsesWindowsX64Layout) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Layout64"), error));
    const u32 handle = m_scm->open_service_handle(L"Layout64");

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;
    ASSERT_TRUE(memory.map_region(kData, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    constexpr GuestAddr kBuffer = kData + 0x100;
    constexpr GuestAddr kNeeded = kData + 0x80;
    cpu.gpr.rcx = handle;
    cpu.gpr.rdx = kBuffer;
    cpu.gpr.r8 = 0x800;
    cpu.gpr.r9 = kNeeded;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    DllExports exports = create_advapi32_scm_thunks(*m_scm);
    auto* entry = find_export(exports, "QueryServiceConfigW");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 needed = memory.read_u32(kNeeded).value();
    EXPECT_GT(needed, 64u);
    EXPECT_EQ(memory.read_u32(kBuffer + 0).value(),
              SERVICE_WIN32_OWN_PROCESS);
    EXPECT_EQ(memory.read_u32(kBuffer + 4).value(), SERVICE_DEMAND_START);

    const GuestAddr binary = memory.read_u64(kBuffer + 16).value();
    const GuestAddr group = memory.read_u64(kBuffer + 24).value();
    EXPECT_EQ(memory.read_u32(kBuffer + 32).value(), 0u);
    const GuestAddr dependencies = memory.read_u64(kBuffer + 40).value();
    const GuestAddr start_name = memory.read_u64(kBuffer + 48).value();
    const GuestAddr display_name = memory.read_u64(kBuffer + 56).value();

    for (GuestAddr pointer :
         {binary, group, dependencies, start_name, display_name}) {
        EXPECT_GE(pointer, kBuffer + 64);
        EXPECT_LT(pointer, kBuffer + needed);
    }
}

TEST_F(ScmTest, ServiceSecurityDescriptorPersistsInRegistry) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Secured"), error));

    auto initial = m_scm->query_security_descriptor(L"Secured");
    ASSERT_TRUE(initial.has_value());
    ASSERT_EQ(initial->size(), 20u);
    EXPECT_EQ((*initial)[0], 1u);
    EXPECT_EQ((*initial)[2], 0x04u);
    EXPECT_EQ((*initial)[3], 0x80u);

    std::vector<u8> descriptor(28, 0);
    descriptor[0] = 1;
    descriptor[2] = 0x04;
    descriptor[3] = 0x80;
    descriptor[16] = 20;
    descriptor[20] = 2;  // ACL_REVISION
    descriptor[22] = 8;  // ACL size
    ASSERT_TRUE(
        m_scm->set_security_descriptor(L"Secured", descriptor, error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);

    auto stored = m_scm->query_security_descriptor(L"Secured");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(*stored, descriptor);

    auto key = m_registry->open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\Secured", false);
    ASSERT_TRUE(key.is_ok());
    auto value = m_registry->query_value(key.value(), L"Security");
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value().type, capsule::RegType::Binary);
    EXPECT_EQ(std::get<std::vector<u8>>(value.value().data), descriptor);
}

TEST_F(ScmTest, QueryServiceObjectSecurityUsesSizeQueryProtocol) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"SecurityQuery"), error));
    const u32 handle = m_scm->open_service_handle(L"SecurityQuery");

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kStack = 0x20000;
    ASSERT_TRUE(memory.map_region(kData, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x400;
    constexpr GuestAddr kNeeded = kData + 0x80;
    cpu.gpr.rcx = handle;
    cpu.gpr.rdx = 0x4;  // DACL_SECURITY_INFORMATION
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kNeeded).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    DllExports exports = create_advapi32_scm_thunks(*m_scm);
    auto* entry = find_export(exports, "QueryServiceObjectSecurity");
    ASSERT_NE(entry, nullptr);
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(memory.read_u32(kNeeded).value(), 20u);

    constexpr GuestAddr kBuffer = kData + 0x100;
    cpu.gpr.r8 = kBuffer;
    cpu.gpr.r9 = 20;
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(memory.read_u8(kBuffer).value(), 1u);
    EXPECT_EQ(memory.read_u16(kBuffer + 2).value(), 0x8004u);
}

// =============================================================================
// Status
// =============================================================================

TEST_F(ScmTest, NeverStartedServiceReadsAsStopped) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Idle"), error));

    const ServiceStatus status = m_scm->query_status(L"Idle");
    EXPECT_EQ(status.current_state, SERVICE_STOPPED);
    EXPECT_EQ(status.process_id, 0u);
    EXPECT_EQ(status.service_type, SERVICE_WIN32_OWN_PROCESS);
}

TEST_F(ScmTest, StartWithoutProcessSessionFailsHonestly) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"NoSession"), error));

    // No ProcessSession is wired, so no process can be created. The old stub
    // returned TRUE here; a controller then waited forever on a service that
    // was never running.
    error = 0;
    EXPECT_FALSE(m_scm->start_service(L"NoSession", {}, error));
    // Learn StartServiceW: a thread could not be created for the service.
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_NO_THREAD);
    EXPECT_EQ(m_scm->query_status(L"NoSession").current_state, SERVICE_STOPPED);
}

TEST_F(ScmTest, StartServiceDiscardsStaleDispatcherHandshake) {
    // Learn StartServiceW: success means the SCM received notification that
    // this start created the ServiceMain thread. A leftover handshake from a
    // previous start must not remain to satisfy a later StartService wait.
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"StaleHandshake"), error));

    const fs::path status_dir = m_root / "runtime" / "services";
    fs::create_directories(status_dir);
    const fs::path handshake = status_dir / "stalehandshake.dispatcher";
    {
        std::ofstream out(handshake, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
    }
    ASSERT_TRUE(fs::exists(handshake));

    error = 0;
    EXPECT_FALSE(m_scm->start_service(L"StaleHandshake", {}, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_NO_THREAD);
    EXPECT_FALSE(fs::exists(handshake));
}

TEST_F(ScmTest, StartMissingServiceFails) {
    u32 error = 0;
    EXPECT_FALSE(m_scm->start_service(L"Missing", {}, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
}

TEST_F(ScmTest, DisabledServiceCannotStart) {
    auto config = make_config(L"Disabled");
    config.start_type = SERVICE_DISABLED;
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(config, error));

    error = 0;
    EXPECT_FALSE(m_scm->start_service(L"Disabled", {}, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_DISABLED);
}

TEST_F(ScmTest, StartPendingWithNoPidReadsAsStopped) {
    // Learn QueryServiceStatus / StartService: SERVICE_STATUS_PROCESS.dwProcessId
    // is 0 when the service is not running. A leftover START_PENDING record
    // with pid 0 (crash after publishing pending, before CreateProcess)
    // must not look like an instance that is already running.
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Orphan"), error));

    const fs::path status_dir = m_root / "runtime" / "services";
    fs::create_directories(status_dir);
    struct Record {
        u32 magic, version, pid, service_type, current_state,
            controls_accepted, win32_exit_code, service_specific_exit_code,
            check_point, wait_hint;
    } record{0x56545356, 1, 0, SERVICE_WIN32_OWN_PROCESS,
             SERVICE_START_PENDING, 0, 0, 0, 0, 30000};
    {
        std::ofstream out(status_dir / "orphan.status", std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
    }

    const ServiceStatus status = m_scm->query_status(L"Orphan");
    EXPECT_EQ(status.current_state, SERVICE_STOPPED);
    EXPECT_EQ(status.process_id, 0u);

    error = 0;
    EXPECT_FALSE(m_scm->start_service(L"Orphan", {}, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_NO_THREAD);
    EXPECT_NE(error, error_codes::ERROR_SERVICE_ALREADY_RUNNING);
}

TEST_F(ScmTest, StatusOfDeadProcessReadsAsStopped) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Zombie"), error));

    // Forge a RUNNING status naming a pid that cannot be alive. A stale
    // RUNNING would leave a controller waiting on a service that can never
    // answer, so the SCM must resolve it to STOPPED.
    const fs::path status_dir = m_root / "runtime" / "services";
    fs::create_directories(status_dir);
    struct Record {
        u32 magic, version, pid, service_type, current_state,
            controls_accepted, win32_exit_code, service_specific_exit_code,
            check_point, wait_hint;
    } record{0x56545356, 1, 0x7FFFFFFF, SERVICE_WIN32_OWN_PROCESS,
             SERVICE_RUNNING, SERVICE_ACCEPT_STOP, 0, 0, 0, 0};
    {
        std::ofstream out(status_dir / "zombie.status", std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
    }

    const ServiceStatus status = m_scm->query_status(L"Zombie");
    EXPECT_EQ(status.current_state, SERVICE_STOPPED);
    EXPECT_EQ(status.process_id, 0u);
}

TEST_F(ScmTest, NativeKernelDriverIgnoresStaleRunningStatus) {
    auto config = make_config(L"CPUZ162");
    config.service_type = SERVICE_KERNEL_DRIVER;
    config.image_path = L"C:\\Windows\\Temp\\cpuz162_x32.sys";
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(config, error));

    // A native provider belongs to one Vitreo process. Simulate the status
    // file left by an earlier process that had the provider active.
    const fs::path status_dir = m_root / "runtime" / "services";
    fs::create_directories(status_dir);
    struct Record {
        u32 magic, version, pid, service_type, current_state,
            controls_accepted, win32_exit_code, service_specific_exit_code,
            check_point, wait_hint;
    } record{0x56545356, 1, 0, SERVICE_KERNEL_DRIVER, SERVICE_RUNNING,
             SERVICE_ACCEPT_STOP, 0, 0, 0, 0};
    {
        std::ofstream out(status_dir / "cpuz162.status", std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
    }

    bool provider_running = false;
    m_scm->set_kernel_driver_callbacks(
        [](const std::wstring&, const std::wstring&, const std::string&,
           u32&) { return false; },
        [](const std::wstring&) {},
        [&](const std::wstring& name) {
            EXPECT_EQ(name, L"CPUZ162");
            return provider_running;
        });

    ServiceStatus status = m_scm->query_status(L"CPUZ162");
    EXPECT_EQ(status.current_state, SERVICE_STOPPED);
    EXPECT_EQ(status.service_type, SERVICE_KERNEL_DRIVER);

    provider_running = true;
    status = m_scm->query_status(L"CPUZ162");
    EXPECT_EQ(status.current_state, SERVICE_RUNNING);
    EXPECT_EQ(status.controls_accepted,
              SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    EXPECT_EQ(status.process_id, 0u);
}

TEST_F(ScmTest, KernelDriverImagePathIsOneFileEvenWithSpaces) {
    // CPU-Z registers its driver unquoted under a directory with a space:
    // "C:\ProgramData\CPUID Software\cpu-z\\<random>". A driver's
    // ImagePath is one image file, not a command line; splitting it at the
    // space handed the provider "C:\ProgramData\CPUID" (a directory), and
    // CPU-Z reported "CPUID init driver error 11" (ERROR_BAD_FORMAT).
    const fs::path drive = m_root / "c";
    const fs::path dir = drive / "ProgramData" / "CPUID Software" / "cpu-z";
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "xngytV3ViGXl", std::ios::binary);
        out << "MZ";
    }
    ASSERT_TRUE(m_vfs->mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(drive.string())).is_ok());

    for (const wchar_t* image :
         {L"C:\\ProgramData\\CPUID Software\\cpu-z\\\\xngytV3ViGXl",
          L"\"C:\\ProgramData\\CPUID Software\\cpu-z\\xngytV3ViGXl\""}) {
        SCOPED_TRACE(std::string(image, image + std::wcslen(image)));
        auto config = make_config(L"cpuz162");
        config.service_type = SERVICE_KERNEL_DRIVER;
        config.image_path = image;
        u32 error = 0;
        ASSERT_TRUE(m_scm->create_service(config, error));

        std::string started_host;
        bool running = false;
        m_scm->set_kernel_driver_callbacks(
            [&](const std::wstring& name, const std::wstring&,
                const std::string& host_path, u32& err) {
                EXPECT_EQ(name, L"cpuz162");
                started_host = host_path;
                running = true;
                err = error_codes::ERROR_SUCCESS;
                return true;
            },
            [&](const std::wstring&) { running = false; },
            [&](const std::wstring&) { return running; });

        // No process session: a driver start creates no process.
        error = 0;
        ASSERT_TRUE(m_scm->start_service(L"cpuz162", {}, error));
        EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
        EXPECT_EQ(fs::canonical(started_host), fs::canonical(dir / "xngytV3ViGXl"));
        EXPECT_EQ(m_scm->query_status(L"cpuz162").current_state, SERVICE_RUNNING);

        ServiceStatus status;
        ASSERT_TRUE(m_scm->control_service(L"cpuz162", SERVICE_CONTROL_STOP,
                                           status, error));
        ASSERT_TRUE(m_scm->delete_service(L"cpuz162", error));
    }
}

TEST_F(ScmTest, ControlOnStoppedServiceReportsNotActive) {
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Stopped"), error));

    ServiceStatus status;
    error = 0;
    EXPECT_FALSE(m_scm->control_service(L"Stopped", SERVICE_CONTROL_STOP,
                                        status, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_NOT_ACTIVE);
}

TEST_F(ScmTest, ControlStopTerminatesAndReportsStopped) {
    // Learn ControlService: a successful STOP leaves the service stopped.
    // Vitreo terminates the worker instead of delivering the control to a
    // guest handler, so QueryServiceStatusEx must not keep STOP_PENDING with
    // a pid that OpenProcess cannot open (ERROR_INVALID_PARAMETER).
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Live"), error));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        for (;;) {
            ::pause();
        }
    }

    const fs::path status_dir = m_root / "runtime" / "services";
    fs::create_directories(status_dir);
    struct Record {
        u32 magic, version, pid, service_type, current_state,
            controls_accepted, win32_exit_code, service_specific_exit_code,
            check_point, wait_hint;
    } record{0x56545356, 1, static_cast<u32>(child), SERVICE_WIN32_OWN_PROCESS,
             SERVICE_RUNNING, SERVICE_ACCEPT_STOP, 0, 0, 0, 0};
    {
        std::ofstream out(status_dir / "live.status", std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
    }

    ServiceStatus status;
    error = 0;
    EXPECT_TRUE(m_scm->control_service(L"Live", SERVICE_CONTROL_STOP,
                                       status, error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(status.current_state, SERVICE_STOPPED);
    EXPECT_EQ(status.process_id, 0u);

    const ServiceStatus seen = m_scm->query_status(L"Live");
    EXPECT_EQ(seen.current_state, SERVICE_STOPPED);
    EXPECT_EQ(seen.process_id, 0u);

    int wait_status = 0;
    EXPECT_EQ(::waitpid(child, &wait_status, 0), child);
}

TEST_F(ScmTest, QueryStatusReportsStoppedWhenWorkerIsZombie) {
    // Learn StartService / QueryServiceStatus: when the service process exits
    // without reaching SERVICE_RUNNING, the SCM reports SERVICE_STOPPED.
    // kill(pid, 0) still succeeds for a zombie, which previously left
    // START_PENDING until a controller timed out.
    u32 error = 0;
    ASSERT_TRUE(m_scm->create_service(make_config(L"Zombie"), error));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        _exit(0);
    }

    const fs::path status_dir = m_root / "runtime" / "services";
    fs::create_directories(status_dir);
    struct Record {
        u32 magic, version, pid, service_type, current_state,
            controls_accepted, win32_exit_code, service_specific_exit_code,
            check_point, wait_hint;
    } record{0x56545356, 1, static_cast<u32>(child), SERVICE_WIN32_OWN_PROCESS,
             SERVICE_START_PENDING, 0, 0, 0, 0, 30000};
    {
        std::ofstream out(status_dir / "zombie.status", std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
    }

    const ServiceStatus seen = m_scm->query_status(L"Zombie");
    EXPECT_EQ(seen.current_state, SERVICE_STOPPED);
    EXPECT_EQ(seen.process_id, 0u);

    int wait_status = 0;
    const pid_t reaped = ::waitpid(child, &wait_status, WNOHANG);
    EXPECT_TRUE(reaped == child || (reaped < 0 && errno == ECHILD));
}

TEST_F(ScmTest, ControlOnMissingServiceFails) {
    ServiceStatus status;
    u32 error = 0;
    EXPECT_FALSE(m_scm->control_service(L"Nothing", SERVICE_CONTROL_STOP,
                                        status, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
}

// =============================================================================
// Service side
// =============================================================================

TEST_F(ScmTest, ProcessNotLaunchedAsServiceIsNotAService) {
    // VITREO_SERVICE_NAME is unset in the test process.
    EXPECT_FALSE(m_scm->launched_as_service());
    EXPECT_TRUE(m_scm->own_service_name().empty());
}

TEST_F(ScmTest, DispatcherFailsWhenNotLaunchedAsService) {
    // This is the documented Windows behaviour that lets a service binary
    // discover it was run from a console — and the exact contract the generic
    // missing-export stub broke by returning a nonzero "success".
    u32 error = 0;
    EXPECT_FALSE(m_scm->run_dispatcher(0x401000, L"Svc", false, true, error));
    EXPECT_EQ(error, error_codes::ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
}

TEST_F(ScmTest, RegisterControlHandlerFailsWhenNotAService) {
    EXPECT_EQ(m_scm->register_control_handler(0x401000, 0, false), 0u);
    EXPECT_EQ(get_last_error(),
              error_codes::ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
}

TEST_F(ScmTest, SetStatusFailsWhenNotAService) {
    ServiceStatus status;
    status.current_state = SERVICE_RUNNING;
    EXPECT_FALSE(m_scm->set_status(1, status));
}

// A service process is one whose VITREO_SERVICE_NAME is set before the manager
// is constructed. These tests build a second manager under that condition.
class ScmServiceProcessTest : public ScmTest {
protected:
    void SetUp() override {
        ScmTest::SetUp();
        ::setenv("VITREO_SERVICE_NAME", "SelfSvc", 1);
        m_service_scm = std::make_unique<ScmManager>(*m_registry, *m_vfs);
        m_service_scm->set_capsule(m_root.string(), "test");
    }

    void TearDown() override {
        ::unsetenv("VITREO_SERVICE_NAME");
        ScmTest::TearDown();
    }

    std::unique_ptr<ScmManager> m_service_scm;
};

TEST_F(ScmServiceProcessTest, DiscoversItsOwnServiceName) {
    EXPECT_TRUE(m_service_scm->launched_as_service());
    EXPECT_EQ(m_service_scm->own_service_name(), L"SelfSvc");
}

TEST_F(ScmServiceProcessTest, SetStatusPublishesWhatControllersRead) {
    const u32 handle = m_service_scm->register_control_handler(0x401000, 0, true);
    ASSERT_NE(handle, 0u);

    ServiceStatus status;
    status.service_type = SERVICE_WIN32_OWN_PROCESS;
    status.current_state = SERVICE_RUNNING;
    status.controls_accepted = SERVICE_ACCEPT_STOP;
    status.check_point = 3;
    ASSERT_TRUE(m_service_scm->set_status(handle, status));

    // A separate manager — standing in for the controller process — must read
    // back the state the service published, including our live pid.
    const ServiceStatus seen = m_scm->query_status(L"SelfSvc");
    EXPECT_EQ(seen.current_state, SERVICE_RUNNING);
    EXPECT_EQ(seen.controls_accepted, SERVICE_ACCEPT_STOP);
    EXPECT_EQ(seen.check_point, 3u);
    EXPECT_EQ(seen.process_id, static_cast<u32>(::getpid()));
}

TEST_F(ScmServiceProcessTest, SetStatusRejectsForeignHandle) {
    const u32 handle = m_service_scm->register_control_handler(0x401000, 0, false);
    ASSERT_NE(handle, 0u);

    ServiceStatus status;
    status.current_state = SERVICE_RUNNING;
    EXPECT_FALSE(m_service_scm->set_status(handle + 1, status));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(ScmServiceProcessTest, StoppedStatusClearsThePid) {
    const u32 handle = m_service_scm->register_control_handler(0x401000, 0, false);
    ServiceStatus running;
    running.current_state = SERVICE_RUNNING;
    ASSERT_TRUE(m_service_scm->set_status(handle, running));

    ServiceStatus stopped;
    stopped.current_state = SERVICE_STOPPED;
    ASSERT_TRUE(m_service_scm->set_status(handle, stopped));

    const ServiceStatus seen = m_scm->query_status(L"SelfSvc");
    EXPECT_EQ(seen.current_state, SERVICE_STOPPED);
    EXPECT_EQ(seen.process_id, 0u);
}

TEST_F(ScmServiceProcessTest, DispatcherNeedsAGuestCallBinding) {
    // Launched as a service, but with no way to reach guest code: the
    // dispatcher must fail rather than claim the service ran.
    u32 error = 0;
    EXPECT_FALSE(m_service_scm->run_dispatcher(0x401000, L"SelfSvc", false,
                                               true, error));
    EXPECT_EQ(error, error_codes::ERROR_SERVICE_NO_THREAD);
}

TEST_F(ScmServiceProcessTest, DispatcherRunsServiceMainAndWaitsForReportedStop) {
    GuestAddr called_with = 0;
    int call_count = 0;
    m_service_scm->set_service_thread_call(
        [&](GuestAddr addr, const std::vector<std::wstring>& args,
            bool wide, bool is_64bit) -> bool {
            called_with = addr;
            ++call_count;
            EXPECT_EQ(args, (std::vector<std::wstring>{L"SelfSvc"}));
            EXPECT_TRUE(wide);
            EXPECT_FALSE(is_64bit);
            // While ServiceMain runs, the SCM has already published the
            // documented StartService defaults: START_PENDING, WaitHint 2s.
            const ServiceStatus pending = m_scm->query_status(L"SelfSvc");
            EXPECT_EQ(pending.current_state, SERVICE_START_PENDING);
            EXPECT_EQ(pending.wait_hint, 2000u);
            EXPECT_TRUE(fs::exists(m_root / "runtime" / "services" /
                                   "selfsvc.dispatcher"));
            const u32 handle=m_service_scm->register_control_handler(0x402000,0,true);
            ServiceStatus stopped;stopped.current_state=SERVICE_STOPPED;
            EXPECT_TRUE(m_service_scm->set_status(handle,stopped));
            return true;
        });

    u32 error = 0xFFFF;
    EXPECT_TRUE(m_service_scm->run_dispatcher(0x401000, L"SelfSvc", false, true,
                                              error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(called_with, 0x401000u);

    // The dispatcher returning means the service ended; controllers must see
    // STOPPED, not a stale START_PENDING, and the handshake for this start
    // must not remain to satisfy a later StartService.
    EXPECT_EQ(m_scm->query_status(L"SelfSvc").current_state, SERVICE_STOPPED);
    EXPECT_FALSE(fs::exists(m_root / "runtime" / "services" /
                            "selfsvc.dispatcher"));
}

TEST_F(ScmServiceProcessTest, DispatcherDeliversStartServiceArguments) {
    ASSERT_TRUE(m_service_scm->set_own_service_arguments_utf16({
        "002D002D0063006F006D002D0073006500720076006900630065",
        ""}));

    int call_count = 0;
    m_service_scm->set_service_thread_call(
        [&](GuestAddr, const std::vector<std::wstring>& args,
            bool wide, bool is_64bit) -> bool {
            ++call_count;
            EXPECT_EQ(args, (std::vector<std::wstring>{
                L"SelfSvc", L"--com-service", L""}));
            EXPECT_FALSE(wide);
            EXPECT_TRUE(is_64bit);
            const u32 handle=m_service_scm->register_control_handler(0x402000,0,true);
            ServiceStatus stopped;stopped.current_state=SERVICE_STOPPED;
            EXPECT_TRUE(m_service_scm->set_status(handle,stopped));
            return true;
        });

    u32 error = 0;
    EXPECT_TRUE(m_service_scm->run_dispatcher(
        0x401000, L"SelfSvc", true, false, error));
    EXPECT_EQ(error, error_codes::ERROR_SUCCESS);
    EXPECT_EQ(call_count, 1);
}

TEST_F(ScmServiceProcessTest, RejectsMalformedEncodedServiceArgument) {
    EXPECT_FALSE(
        m_service_scm->set_own_service_arguments_utf16({"not-utf16-hex"}));
}

#include <future>
#include <atomic>
#include <thread>
TEST_F(ScmServiceProcessTest, DispatcherWaitsForExplicitStopAfterServiceMainReturns) {
    // Learn StartServiceCtrlDispatcher: return only once every running service
    // has entered SERVICE_STOPPED. ServiceMain may return after starting workers.
    for(bool wide:{false,true})for(bool x64:{false,true}) {
        ScmManager service(*m_registry,*m_vfs);
        service.set_capsule(m_root.string(),"test");
        std::atomic<unsigned> wait_calls{0};
        service.set_service_dispatch_wait([&] {
            ++wait_calls;std::this_thread::sleep_for(std::chrono::milliseconds(1));return true;
        });
        const u32 handle=service.register_control_handler(0x402000,0,true);
        ASSERT_NE(handle,0u);
        std::promise<void> service_main_returned;
        service.set_service_thread_call([&](GuestAddr,const std::vector<std::wstring>&,bool got_wide,bool got_x64) {
            EXPECT_EQ(got_wide,wide);EXPECT_EQ(got_x64,x64);
            ServiceStatus status;status.current_state=SERVICE_RUNNING;
            EXPECT_TRUE(service.set_status(handle,status));
            service_main_returned.set_value();return true;
        });
        u32 error=0xffffffff;
        auto dispatcher=std::async(std::launch::async,[&]{return service.run_dispatcher(0x401000,L"SelfSvc",x64,wide,error);});
        service_main_returned.get_future().wait();
        EXPECT_EQ(dispatcher.wait_for(std::chrono::milliseconds(25)),std::future_status::timeout);
        EXPECT_EQ(m_scm->query_status(L"SelfSvc").current_state,SERVICE_RUNNING);
        EXPECT_TRUE(fs::exists(m_root/"runtime"/"services"/"selfsvc.dispatcher"));
        ServiceStatus stopped;stopped.current_state=SERVICE_STOPPED;
        EXPECT_TRUE(service.set_status(handle,stopped));
        EXPECT_TRUE(dispatcher.get());EXPECT_EQ(error,error_codes::ERROR_SUCCESS);
        EXPECT_GT(wait_calls.load(),0u);
        EXPECT_EQ(m_scm->query_status(L"SelfSvc").current_state,SERVICE_STOPPED);
        EXPECT_FALSE(fs::exists(m_root/"runtime"/"services"/"selfsvc.dispatcher"));
    }
}

#include "scm_dispatch_table.inc"
TEST_F(ScmServiceProcessTest, DispatcherExportValidatesTablesAndConsumesItsOneShotConnection) {
    for(bool wide:{false,true})for(bool x64:{false,true})for(unsigned scenario=0;scenario<6;++scenario) {
        ScmManager service(*m_registry,*m_vfs);service.set_capsule(m_root.string(),"test");
        unsigned calls=0;
        service.set_service_thread_call([&](GuestAddr proc,const std::vector<std::wstring>& args,bool got_wide,bool got_x64) {
            ++calls;EXPECT_EQ(proc,0x401000u);EXPECT_EQ(args[0],L"SelfSvc");
            EXPECT_EQ(got_wide,wide);EXPECT_EQ(got_x64,x64);
            const u32 handle=service.register_control_handler(0x402000,0,true);
            ServiceStatus stopped;stopped.current_state=SERVICE_STOPPED;
            EXPECT_TRUE(service.set_status(handle,stopped));return true;
        });
        GuestMemory memory;ASSERT_TRUE(memory.map_region(0x3000,0x3000,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x5800;Marshaler m(memory);m.attach_cpu(cpu);
        const auto write_table=[&](auto pointer) {
            using Pointer=decltype(pointer);
            SdkServiceTableEntry<Pointer> table[3]={{1,0x401000},{0,0},{0,0}};
            if(scenario==2)table[0]={0,0};
            if(scenario==3)table[0]={1,0};
            if(scenario==4)table[1]={1,0x403000};
            EXPECT_TRUE(memory.write_bytes(0x3000,reinterpret_cast<const u8*>(table),sizeof(table)).is_ok());
        };
        if(x64)write_table(u64{});else write_table(u32{});
        const auto exports=create_advapi32_scm_thunks(service);
        const ExportEntry* entry=nullptr;
        for(const auto& candidate:exports.entries)
            if(candidate.name==(wide?"StartServiceCtrlDispatcherW":"StartServiceCtrlDispatcherA"))entry=&candidate;
        ASSERT_NE(entry,nullptr);EXPECT_EQ(entry->stdcall_bytes,4u);
        const GuestAddr table=scenario==1?0:scenario==5?1:0x3000;
        if(x64)cpu.gpr.rcx=table;else ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp+4,static_cast<u32>(table)).is_ok());
        entry->handler(m);
        EXPECT_EQ(cpu.gpr.rax,scenario==0?1u:0u);
        EXPECT_EQ(calls,scenario==0?1u:0u);
        if(scenario==1||scenario==2||scenario==3)EXPECT_EQ(get_last_error(),error_codes::ERROR_INVALID_DATA);
        if(scenario==4)EXPECT_EQ(get_last_error(),error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        EXPECT_EQ(cpu.pending_raise.active,scenario==5);
        cpu.pending_raise.active=false;
        entry->handler(m);
        EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_SERVICE_ALREADY_RUNNING);
        EXPECT_FALSE(cpu.pending_raise.active);
    }
}

TEST_F(ScmTest, DispatcherConnectionFailureStillConsumesTheProcessAttempt) {
    u32 error=0;
    EXPECT_FALSE(m_scm->run_dispatcher(0x401000,L"Svc",false,true,error));
    EXPECT_EQ(error,error_codes::ERROR_FAILED_SERVICE_CONTROLLER_CONNECT);
    EXPECT_FALSE(m_scm->run_dispatcher(0,L"",false,true,error));
    EXPECT_EQ(error,error_codes::ERROR_SERVICE_ALREADY_RUNNING);
}

TEST_F(ScmServiceProcessTest, DispatcherFailedThreadCreationRemovesReadinessAndPendingStatus) {
    m_service_scm->set_service_thread_call([](GuestAddr,const std::vector<std::wstring>&,bool,bool){return false;});
    u32 error=0;
    EXPECT_FALSE(m_service_scm->run_dispatcher(0x401000,L"SelfSvc",true,true,error));
    EXPECT_EQ(error,error_codes::ERROR_SERVICE_NO_THREAD);
    EXPECT_FALSE(fs::exists(m_root/"runtime"/"services"/"selfsvc.dispatcher"));
    EXPECT_FALSE(fs::exists(m_root/"runtime"/"services"/"selfsvc.status"));
}

TEST_F(ScmServiceProcessTest, DispatcherCancellationAndMissingBindingPreserveReportedState) {
    for(bool cancellation:{false,true}) {
        ScmManager service(*m_registry,*m_vfs);service.set_capsule(m_root.string(),"test");
        service.set_service_thread_call([&](GuestAddr,const std::vector<std::wstring>&,bool,bool) {
            const u32 handle=service.register_control_handler(0x402000,0,true);
            ServiceStatus running;running.current_state=SERVICE_RUNNING;
            EXPECT_TRUE(service.set_status(handle,running));return true;
        });
        unsigned calls=0;
        if(cancellation)service.set_service_dispatch_wait([&]{++calls;return false;});
        u32 error=0;EXPECT_FALSE(service.run_dispatcher(0x401000,L"SelfSvc",true,true,error));
        EXPECT_EQ(error,cancellation?error_codes::ERROR_OPERATION_ABORTED:error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        EXPECT_EQ(calls,cancellation?1u:0u);
        EXPECT_EQ(m_scm->query_status(L"SelfSvc").current_state,SERVICE_RUNNING);
        EXPECT_FALSE(fs::exists(m_root/"runtime"/"services"/"selfsvc.dispatcher"));
    }
}

TEST_F(ScmServiceProcessTest, DispatcherPublicationFailureDoesNotRunServiceMain) {
    const auto blocked=m_root/"blocked-service-root";
    std::ofstream(blocked)<<"not a directory";
    m_service_scm->set_capsule(blocked.string(),"test");
    unsigned calls=0;
    m_service_scm->set_service_thread_call([&](GuestAddr,const std::vector<std::wstring>&,bool,bool){++calls;return true;});
    u32 error=0;
    EXPECT_FALSE(m_service_scm->run_dispatcher(0x401000,L"SelfSvc",true,true,error));
    EXPECT_EQ(error,error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(calls,0u);
}
