// SPDX-License-Identifier: LGPL-2.1-only
// Stage 2: a named file-mapping is backed by POSIX shm so MapViewOfFile views
// are shared across the forked Vitreo host processes. IPC is enabled by
// default; VITREO_IPC=0 is only a diagnostic override.

#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/error.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/shared_memory.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {
constexpr u32 PAGE_READWRITE = 0x04;
constexpr u32 FILE_MAP_READ_WRITE = 0x0006;
}

TEST(Stage2SharedMapping, CrossProcessNamedMappingShares) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }

    const std::string name =
        "Global\\vitreo_stage2_" + std::to_string(::getpid());
    constexpr u64 kSize = 4096;
    constexpr u32 kParentMagic = 0x0A0B0C0Du;
    constexpr u32 kChildMagic  = 0x1F2E3D4Cu;

    HandleTable ph;
    GuestMemory pmem;
    FileMappingManager pfm(ph, pmem);

    Handle hmap = pfm.create_file_mapping(FILE_MAPPING_INVALID_FILE,
                                          PAGE_READWRITE, kSize, name);
    ASSERT_NE(hmap, NULL_HANDLE);
    GuestAddr pview =
        pfm.map_view_of_file(hmap, FILE_MAP_READ_WRITE, 0, 0);
    ASSERT_NE(pview, 0u);
    ASSERT_TRUE(pmem.write_u32(pview + 0x10, kParentMagic));

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        HandleTable ch;
        GuestMemory cmem;
        FileMappingManager cfm(ch, cmem);
        Handle chmap = cfm.open_file_mapping(FILE_MAP_READ_WRITE, name);
        if (chmap == NULL_HANDLE) _exit(10);
        GuestAddr cview =
            cfm.map_view_of_file(chmap, FILE_MAP_READ_WRITE, 0, 0);
        if (cview == 0) _exit(11);
        auto v = cmem.read_u32(cview + 0x10);
        if (!v.is_ok() || v.value() != kParentMagic) _exit(12);  // didn't see parent
        if (!cmem.write_u32(cview + 0x20, kChildMagic)) _exit(13);
        _exit(0);
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0) << "child failed at step " << WEXITSTATUS(status);

    // Parent sees the child's write through the shared mapping.
    auto cv = pmem.read_u32(pview + 0x20);
    ASSERT_TRUE(cv.is_ok());
    EXPECT_EQ(cv.value(), kChildMagic);

    EXPECT_TRUE(pfm.unmap_view_of_file(pview));
}

// Intra-process: two views of the same named mapping share writes (gate-on).
TEST(Stage2SharedMapping, IntraProcessTwoViewsShare) {
    if (!ipc::ipc_enabled()) {
        GTEST_SKIP() << "disabled by VITREO_IPC=0";
    }
    const std::string name =
        "Global\\vitreo_stage2b_" + std::to_string(::getpid());
    HandleTable h;
    GuestMemory mem;
    FileMappingManager fm(h, mem);

    Handle m1 = fm.create_file_mapping(FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, 4096, name);
    ASSERT_NE(m1, NULL_HANDLE);
    Handle m2 = fm.open_file_mapping(FILE_MAP_READ_WRITE, name);
    ASSERT_NE(m2, NULL_HANDLE);

    GuestAddr v1 =
        fm.map_view_of_file(m1, FILE_MAP_READ_WRITE, 0, 0);
    GuestAddr v2 =
        fm.map_view_of_file(m2, FILE_MAP_READ_WRITE, 0, 0);
    ASSERT_NE(v1, 0u);
    ASSERT_NE(v2, 0u);

    ASSERT_TRUE(mem.write_u32(v1 + 0x100, 0xFEEDFACEu));
    auto r = mem.read_u32(v2 + 0x100);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 0xFEEDFACEu);  // write to v1 visible at v2 (same shm)

    EXPECT_TRUE(fm.unmap_view_of_file(v1));
    EXPECT_TRUE(fm.unmap_view_of_file(v2));
}

TEST(Stage2SharedMapping, HostNamesAreCapsuleScoped) {
    const char* previous = std::getenv("VITREO_IPC_SCOPE");
    const std::string saved = previous != nullptr ? previous : "";
    const bool had_previous = previous != nullptr;

    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", "/capsules/a", 1), 0);
    const std::string shm_a =
        ipc::posix_shm_name("Global\\Valve_SteamIPC_Class");
    const std::string pipe_a =
        ipc::posix_pipe_path("\\\\.\\pipe\\mojo.fixed");

    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", "/capsules/b", 1), 0);
    const std::string shm_b =
        ipc::posix_shm_name("Global\\Valve_SteamIPC_Class");
    const std::string pipe_b =
        ipc::posix_pipe_path("\\\\.\\pipe\\mojo.fixed");

    if (had_previous) {
        ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", saved.c_str(), 1), 0);
    } else {
        ASSERT_EQ(::unsetenv("VITREO_IPC_SCOPE"), 0);
    }

    EXPECT_NE(shm_a, shm_b);
    EXPECT_NE(pipe_a, pipe_b);
}

TEST(Stage2SharedMapping, LastLiveReferenceRemovesWindowsObjectName) {
    const std::string name =
        "Global\\vitreo_lifetime_" + std::to_string(::getpid());

    {
        auto created = ipc::SharedMemory::create_or_open(
            name, 4096, /*create=*/true);
        ASSERT_NE(created, nullptr);
        EXPECT_TRUE(created->is_creator());

        auto opened = ipc::SharedMemory::create_or_open(
            name, 0, /*create=*/false);
        ASSERT_NE(opened, nullptr);
        EXPECT_FALSE(opened->is_creator());
    }

    // Windows removes a named kernel object after its final handle closes.
    auto missing = ipc::SharedMemory::create_or_open(
        name, 0, /*create=*/false);
    EXPECT_EQ(missing, nullptr);
}

TEST(Stage2SharedMapping, OpenerObservesCreatorsExactLogicalSize) {
    const std::string name =
        "Global\\vitreo_logical_size_" + std::to_string(::getpid());
    constexpr u64 logical_size = 8208;

    auto created = ipc::SharedMemory::create_or_open(
        name, logical_size, /*create=*/true);
    ASSERT_NE(created, nullptr);
    ASSERT_TRUE(created->is_creator());
    EXPECT_EQ(created->size(), logical_size);

    auto opened = ipc::SharedMemory::create_or_open(
        name, 0, /*create=*/false);
    ASSERT_NE(opened, nullptr);
    EXPECT_FALSE(opened->is_creator());
    EXPECT_EQ(opened->size(), logical_size);
}

TEST(Stage2SharedMapping, DeadOwnerReferenceIsPrunedBeforeRecreate) {
    const std::string name =
        "Global\\vitreo_dead_owner_" + std::to_string(::getpid());
    int ready_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(ready_pipe), 0);

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        auto created = ipc::SharedMemory::create_or_open(
            name, 4096, /*create=*/true);
        const char ready = created != nullptr ? 1 : 0;
        (void)::write(ready_pipe[1], &ready, 1);
        (void)::close(ready_pipe[1]);
        if (!created) _exit(10);
        for (;;) ::pause();
    }

    (void)::close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    (void)::close(ready_pipe[0]);
    ASSERT_EQ(ready, 1);
    ASSERT_EQ(::kill(child, SIGKILL), 0);

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    auto recreated = ipc::SharedMemory::create_or_open(
        name, 4096, /*create=*/true);
    ASSERT_NE(recreated, nullptr);
    EXPECT_TRUE(recreated->is_creator());
}

namespace {

// Runs the body with VITREO_IPC_SCOPE set to a scope unique to this test
// process, so sweeps never see objects that belong to real capsules.
class ScopedIpcScope {
public:
    explicit ScopedIpcScope(const std::string& tag) {
        if (const char* previous = std::getenv("VITREO_IPC_SCOPE")) {
            m_saved = previous;
            m_had_saved = true;
        }
        const std::string scope = "/test-scope/" + tag + "/" +
                                  std::to_string(::getpid());
        (void)::setenv("VITREO_IPC_SCOPE", scope.c_str(), 1);
    }
    ~ScopedIpcScope() {
        if (m_had_saved) {
            (void)::setenv("VITREO_IPC_SCOPE", m_saved.c_str(), 1);
        } else {
            (void)::unsetenv("VITREO_IPC_SCOPE");
        }
    }

private:
    std::string m_saved;
    bool m_had_saved{false};
};

// Forks a child that creates @p name and then waits forever. Returns the
// child's pid once the object exists, or -1.
pid_t spawn_creator(const std::string& name) {
    int ready_pipe[2] = {-1, -1};
    if (::pipe(ready_pipe) != 0) return -1;
    const pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        (void)::close(ready_pipe[0]);
        auto created = ipc::SharedMemory::create_or_open(
            name, 64 * 1024, /*create=*/true);
        const char ready = created != nullptr ? 1 : 0;
        (void)::write(ready_pipe[1], &ready, 1);
        (void)::close(ready_pipe[1]);
        if (!created) _exit(10);
        for (;;) ::pause();
    }
    (void)::close(ready_pipe[1]);
    char ready = 0;
    const bool ok = ::read(ready_pipe[0], &ready, 1) == 1 && ready == 1;
    (void)::close(ready_pipe[0]);
    return ok ? child : -1;
}

bool host_object_exists(const std::string& posix_name) {
    const int fd = ::shm_open(posix_name.c_str(), O_RDONLY);
    if (fd < 0) return false;
    (void)::close(fd);
    return true;
}

} // namespace

// A host process killed without running destructors (kill -9 of a guest
// tree) must not leave its sections behind for the life of the machine.
// Windows frees a section once every handle and view is gone, which process
// termination guarantees. Unnamed sections get pid-unique host names, so the
// prune-on-reopen path never runs for them; the sweep must find them.
TEST(Stage2SharedMapping, KilledCreatorsObjectsAreReapedWithoutReopen) {
    ScopedIpcScope scope("reap-dead");
    const std::string name =
        "VitreoAnonymousMapping_reap_" + std::to_string(::getpid());
    const std::string data_name = ipc::posix_shm_name(name);
    const std::string lifetime_name =
        ipc::posix_shm_name("lifetime:" + name);

    const pid_t child = spawn_creator(name);
    ASSERT_GT(child, 0);
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));

    ASSERT_TRUE(host_object_exists(data_name)) << "precondition: leaked";
    ASSERT_TRUE(host_object_exists(lifetime_name)) << "precondition: leaked";

    EXPECT_GE(ipc::SharedMemory::reap_stale_objects(), 1u);
    EXPECT_FALSE(host_object_exists(data_name));
    EXPECT_FALSE(host_object_exists(lifetime_name));
}

TEST(Stage2SharedMapping, SweepKeepsObjectsOfLiveProcesses) {
    ScopedIpcScope scope("reap-live");
    const std::string name =
        "VitreoAnonymousMapping_live_" + std::to_string(::getpid());
    const std::string data_name = ipc::posix_shm_name(name);

    const pid_t child = spawn_creator(name);
    ASSERT_GT(child, 0);

    (void)ipc::SharedMemory::reap_stale_objects();
    EXPECT_TRUE(host_object_exists(data_name));

    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_GE(ipc::SharedMemory::reap_stale_objects(), 1u);
    EXPECT_FALSE(host_object_exists(data_name));
}

// A clean close must also retire the sweep's record of the object.
TEST(Stage2SharedMapping, CleanCloseLeavesNothingToReap) {
    ScopedIpcScope scope("reap-clean");
    const std::string name =
        "VitreoAnonymousMapping_clean_" + std::to_string(::getpid());
    {
        auto created = ipc::SharedMemory::create_or_open(
            name, 4096, /*create=*/true);
        ASSERT_NE(created, nullptr);
    }
    EXPECT_EQ(ipc::SharedMemory::reap_stale_objects(), 0u);
    EXPECT_FALSE(host_object_exists(ipc::posix_shm_name(name)));
}

TEST(Stage2SharedMapping, SectionDescriptorIsCreatorOwnedAndOutsidePayload) {
    const auto name="VitreoSectionDescriptor_"+std::to_string(::getpid());
    auto creator=ipc::SharedMemory::create_section(name,4096,true,0x2);
    ASSERT_NE(creator,nullptr);
    ASSERT_TRUE(creator->section_protection().has_value());
    EXPECT_EQ(*creator->section_protection(),0x2u);
    auto existing=ipc::SharedMemory::create_section(name,8192,true,0x40);
    ASSERT_NE(existing,nullptr);EXPECT_FALSE(existing->is_creator());
    EXPECT_EQ(existing->size(),4096u);EXPECT_EQ(*existing->section_protection(),0x2u);
    auto* payload=static_cast<unsigned char*>(creator->map_view(0,4096));
    ASSERT_NE(payload,nullptr);EXPECT_EQ(payload[0],0u);
    payload[0]=0x75;EXPECT_EQ(*existing->section_protection(),0x2u);
    ipc::SharedMemory::unmap_view(payload,4096);
}

TEST(Stage2SharedMapping, MissingSectionDescriptorFailsInsteadOfInferringProtection) {
    const auto name="VitreoMissingSectionDescriptor_"+std::to_string(::getpid());
    auto generic=ipc::SharedMemory::create_or_open(name,4096,true);
    ASSERT_NE(generic,nullptr);EXPECT_FALSE(generic->section_protection().has_value());
    HandleTable handles;GuestMemory memory;FileMappingManager manager(handles,memory);
    EXPECT_EQ(manager.open_file_mapping(0xf001f,name),0u);
    EXPECT_EQ(get_last_error(),error_codes::ERROR_NOT_SUPPORTED);
    auto existing=ipc::SharedMemory::create_section(name,4096,true,0x4);
    ASSERT_NE(existing,nullptr);EXPECT_FALSE(existing->section_protection().has_value());
}
