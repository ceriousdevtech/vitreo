// SPDX-License-Identifier: LGPL-2.1-only

#include "ipc/hwnd_registry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace vitreo::ipc {
namespace {

TEST(HwndRegistryTest, AllocatesDesktopWideHandlesAcrossProcesses) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-hwnd-registry-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    std::array<int, 2> pipe_fds{};
    ASSERT_EQ(::pipe(pipe_fds.data()), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(pipe_fds[0]);
        const u32 handle = hwnd_registry_allocate_handle();
        const ssize_t ignored =
            ::write(pipe_fds[1], &handle, sizeof(handle));
        (void)ignored;
        ::close(pipe_fds[1]);
        ::_exit(handle != 0 ? 0 : 1);
    }

    ::close(pipe_fds[1]);
    const u32 parent_handle = hwnd_registry_allocate_handle();
    u32 child_handle = 0;
    ASSERT_EQ(::read(pipe_fds[0], &child_handle, sizeof(child_handle)),
              static_cast<ssize_t>(sizeof(child_handle)));
    ::close(pipe_fds[0]);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    EXPECT_GE(parent_handle, 0x00010000u);
    EXPECT_GE(child_handle, 0x00010000u);
    EXPECT_NE(parent_handle, child_handle);

    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

TEST(HwndRegistryTest, PublishesOwningGuestThreadIdentity) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-hwnd-identity-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    constexpr u32 hwnd = 0x00123456u;
    constexpr u32 thread_id = 73u;
    hwnd_registry_publish(hwnd, 348, 646, thread_id);
    const auto entry = hwnd_registry_lookup(hwnd);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->hwnd, hwnd);
    EXPECT_EQ(entry->owner_host_pid, static_cast<u32>(::getpid()));
    EXPECT_EQ(entry->owner_guest_thread_id, thread_id);
    EXPECT_EQ(entry->parent_hwnd, 0u);
    EXPECT_EQ(entry->client_w, 348);
    EXPECT_EQ(entry->client_h, 646);

    hwnd_registry_unpublish(hwnd);
    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

TEST(HwndRegistryTest, PreservesPrimaryGuestThreadIdentityWithoutPidRemap) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-hwnd-primary-tid-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    constexpr u32 hwnd = 0x00123458u;
    hwnd_registry_publish(hwnd, 100, 100, /*owner_guest_thread_id=*/1);
    const auto entry = hwnd_registry_lookup(hwnd);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->owner_guest_thread_id, 1u);

    const u32 a = allocate_system_thread_id();
    const u32 b = allocate_system_thread_id();
    EXPECT_GE(a, 0x01000000u);
    EXPECT_GE(b, 0x01000000u);
    EXPECT_NE(a, b);

    hwnd_registry_unpublish(hwnd);
    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

TEST(HwndRegistryTest, ExhaustedOrDamagedThreadCounterNeverRecyclesIdentities) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("vitreo-thread-counter-test-" + std::to_string(::getpid()));
    fs::create_directories(root / "runtime");
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            ::unsetenv("VITREO_IPC_SCOPE");
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{root};
    const auto path = root / "runtime/next-tid";
    // Persistent sentinel after the last allocated ID in this internal
    // namespace. A subsequent allocation must not reset the counter.
    const u32 exhausted = 0x7FFFFFFFu;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&exhausted), sizeof(exhausted));
    }
    EXPECT_EQ(allocate_system_thread_id(), 0u);
    EXPECT_EQ(allocate_system_thread_id(), 0u);
    std::ofstream(path, std::ios::binary | std::ios::trunc).put('x');
    EXPECT_EQ(allocate_system_thread_id(), 0u);
    EXPECT_EQ(fs::file_size(path), 1u);
}

TEST(HwndRegistryTest, TracksCrossProcessParentChangesSeparatelyFromRefresh) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-hwnd-parent-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);

    constexpr u32 child = 0x00123457u;
    hwnd_registry_publish(child, 800, 600, 7);
    EXPECT_EQ(hwnd_registry_set_parent(child, 0x00120000u), 0u);
    hwnd_registry_publish(child, 348, 646, 7);
    const auto entry = hwnd_registry_lookup(child);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->parent_hwnd, 0x00120000u);
    EXPECT_EQ(entry->client_w, 348);
    EXPECT_EQ(entry->client_h, 646);

    hwnd_registry_unpublish(child);
    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

// =============================================================================
// The in-process query cache.
//
// Every query used to hit the filesystem (steam.exe: ~4% of a core in stat /
// getdirentries from the message loop). The cache must still observe every
// change any process makes: FindWindow is desktop-wide on Windows, and a
// stale answer here is a window that exists but cannot be found, or one that
// is gone but still "exists".
// =============================================================================

namespace {
struct ScopedRegistry {
    std::filesystem::path root;
    explicit ScopedRegistry(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               (std::string("vitreo-hwnd-") + tag + "-" +
                std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        ::setenv("VITREO_IPC_SCOPE", root.c_str(), 1);
    }
    ~ScopedRegistry() {
        (void)::unsetenv("VITREO_IPC_SCOPE");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
} // namespace

TEST(HwndRegistryTest, CachedQueriesObserveEveryInProcessChange) {
    ScopedRegistry scope("cache-local");
    constexpr u32 a = 0x00200001u;
    constexpr u32 p = 0x00200002u;
    hwnd_registry_publish(a, 100, 50, 7, "Alpha", "One");
    hwnd_registry_publish(p, 300, 200, 7, "Parent", "P");

    // Warm every cache path: lookup, is_window (negative), find hit and miss.
    ASSERT_TRUE(hwnd_registry_lookup(a).has_value());
    EXPECT_FALSE(hwnd_registry_is_window(0x00200099u));
    ASSERT_TRUE(hwnd_registry_find_window("alpha", "ONE").has_value())
        << "Learn FindWindow: class and title compare case-insensitively";
    EXPECT_FALSE(hwnd_registry_find_window("Alpha", "Two").has_value());

    // Retitle (record rewritten by rename, same size).
    hwnd_registry_publish(a, 100, 50, 7, "Alpha", "Two");
    EXPECT_FALSE(hwnd_registry_find_window("Alpha", "One").has_value());
    ASSERT_TRUE(hwnd_registry_find_window("Alpha", "Two").has_value());
    EXPECT_EQ(hwnd_registry_find_window("Alpha", "Two")->hwnd, a);

    // Geometry-only publish keeps identity (empty fields carry forward).
    hwnd_registry_publish(a, 120, 60, 7);
    const auto resized = hwnd_registry_lookup(a);
    ASSERT_TRUE(resized.has_value());
    EXPECT_EQ(resized->client_w, 120);
    EXPECT_EQ(resized->client_h, 60);
    ASSERT_TRUE(hwnd_registry_find_window("Alpha", "Two").has_value());

    // Sidecars are rewritten IN PLACE with the same size: each change must
    // be visible to the very next query.
    EXPECT_TRUE(hwnd_registry_lookup(a)->visible);
    hwnd_registry_set_visible(a, false);
    EXPECT_FALSE(hwnd_registry_lookup(a)->visible);
    EXPECT_EQ(hwnd_registry_lookup(a)->parent_hwnd, 0u);
    EXPECT_EQ(hwnd_registry_set_parent(a, p), 0u);
    EXPECT_EQ(hwnd_registry_lookup(a)->parent_hwnd, p);
    EXPECT_EQ(hwnd_registry_lookup(a)->present_host_pid, 0u);
    hwnd_registry_set_present_host(a, 4242);
    EXPECT_EQ(hwnd_registry_lookup(a)->present_host_pid, 4242u);
    const auto all = hwnd_registry_enumerate();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].hwnd, a);
    EXPECT_EQ(all[0].parent_hwnd, p);
    EXPECT_FALSE(all[0].visible);

    // A new window appears to a query that previously missed it.
    constexpr u32 b = 0x00200003u;
    EXPECT_FALSE(hwnd_registry_is_window(b));
    hwnd_registry_publish(b, 10, 10, 7, "Beta", "B");
    EXPECT_TRUE(hwnd_registry_is_window(b));
    EXPECT_EQ(hwnd_registry_find_window("Beta", "")->hwnd, b);

    // Destroyed: gone from lookup, FindWindow and enumeration at once.
    hwnd_registry_unpublish(a);
    EXPECT_FALSE(hwnd_registry_lookup(a).has_value());
    EXPECT_FALSE(hwnd_registry_is_window(a));
    EXPECT_FALSE(hwnd_registry_find_window("Alpha", "").has_value());
    EXPECT_EQ(hwnd_registry_enumerate().size(), 2u);
    hwnd_registry_unpublish(b);
    hwnd_registry_unpublish(p);
    EXPECT_TRUE(hwnd_registry_enumerate().empty());
}

// The brief's hard case: an in-place rewrite of the same size that lands in
// the same timestamp tick as the previous one. Toggle visibility as fast as
// possible with a cached read between every write.
TEST(HwndRegistryTest, SameSizeInPlaceRewriteWithinOneTickIsObserved) {
    ScopedRegistry scope("cache-tick");
    constexpr u32 a = 0x00210001u;
    hwnd_registry_publish(a, 100, 50, 7, "Tick", "T");
    ASSERT_TRUE(hwnd_registry_lookup(a).has_value());
    for (int i = 0; i < 500; ++i) {
        const bool visible = (i & 1) != 0;
        hwnd_registry_set_visible(a, visible);
        const auto e = hwnd_registry_lookup(a);
        ASSERT_TRUE(e.has_value());
        ASSERT_EQ(e->visible, visible) << "iteration " << i;
        hwnd_registry_set_present_host(a, static_cast<u32>(i));
        ASSERT_EQ(hwnd_registry_lookup(a)->present_host_pid,
                  static_cast<u32>(i)) << "iteration " << i;
    }
    hwnd_registry_unpublish(a);
}

// Changes made by ANOTHER process: create, retitle, hide, reparent, destroy,
// and finally the owner dying. The child runs each step on command so the
// parent's cached view is checked after every one.
TEST(HwndRegistryTest, CachedQueriesObserveChangesMadeByAnotherProcess) {
    ScopedRegistry scope("cache-peer");
    constexpr u32 a = 0x00220001u;
    constexpr u32 p = 0x00220002u;
    hwnd_registry_publish(p, 1, 1, 7, "PeerParent", "PP");

    std::array<int, 2> to_child{};
    std::array<int, 2> to_parent{};
    ASSERT_EQ(::pipe(to_child.data()), 0);
    ASSERT_EQ(::pipe(to_parent.data()), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(to_child[1]);
        ::close(to_parent[0]);
        char cmd = 0;
        auto ack = [&] { (void)::write(to_parent[1], "k", 1); };
        ack();  // hello: the parent waits for it before issuing commands
        while (::read(to_child[0], &cmd, 1) == 1) {
            switch (cmd) {
                case 'c': hwnd_registry_publish(a, 20, 30, 9, "Peer", "First"); break;
                case 't': hwnd_registry_publish(a, 20, 30, 9, "Peer", "Second"); break;
                case 'h': hwnd_registry_set_visible(a, false); break;
                case 'r': (void)hwnd_registry_set_parent(a, p); break;
                case 'o': hwnd_registry_publish(0x00220003u, 5, 5, 9, "Other", "O"); break;
                case 'd': hwnd_registry_unpublish(a); break;
                case 'q': ack(); ::_exit(0);
                default: break;
            }
            ack();
        }
        ::_exit(0);
    }
    ::close(to_child[0]);
    ::close(to_parent[1]);
    auto step = [&](char cmd) {
        ASSERT_EQ(::write(to_child[1], &cmd, 1), 1);
        char k = 0;
        ASSERT_EQ(::read(to_parent[0], &k, 1), 1) << "command " << cmd;
    };
    {
        char hello = 0;
        ASSERT_EQ(::read(to_parent[0], &hello, 1), 1);
    }

    // Warm the cache on a miss first: the window does not exist yet.
    EXPECT_FALSE(hwnd_registry_is_window(a));
    EXPECT_FALSE(hwnd_registry_find_window("Peer", "").has_value());

    step('c');
    const auto created = hwnd_registry_lookup(a);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->owner_host_pid, static_cast<u32>(child));
    EXPECT_EQ(created->owner_guest_thread_id, 9u);
    EXPECT_TRUE(created->visible);
    EXPECT_EQ(hwnd_registry_find_window("Peer", "First")->hwnd, a);
    EXPECT_EQ(hwnd_registry_live_peer_owners(),
              std::vector<u32>{static_cast<u32>(child)});

    step('t');
    EXPECT_FALSE(hwnd_registry_find_window("Peer", "First").has_value());
    EXPECT_EQ(hwnd_registry_find_window("Peer", "Second")->hwnd, a);

    step('h');
    EXPECT_FALSE(hwnd_registry_lookup(a)->visible);

    step('r');
    EXPECT_EQ(hwnd_registry_lookup(a)->parent_hwnd, p);

    step('o');
    EXPECT_EQ(hwnd_registry_enumerate().size(), 3u);
    EXPECT_TRUE(hwnd_registry_is_window(0x00220003u));

    step('d');
    EXPECT_FALSE(hwnd_registry_lookup(a).has_value());
    EXPECT_FALSE(hwnd_registry_find_window("Peer", "").has_value());
    EXPECT_EQ(hwnd_registry_enumerate().size(), 2u);

    // The owner dies: its remaining window no longer exists.
    EXPECT_TRUE(hwnd_registry_is_window(0x00220003u));
    step('q');
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_FALSE(hwnd_registry_is_window(0x00220003u));
    EXPECT_FALSE(hwnd_registry_find_window("Other", "").has_value());
    EXPECT_TRUE(hwnd_registry_live_peer_owners().empty());
    ::close(to_child[1]);
    ::close(to_parent[0]);
    hwnd_registry_unpublish(p);
}

// Two registries in one process: switching VITREO_IPC_SCOPE switches the
// registry the cache describes, and nothing leaks across.
TEST(HwndRegistryTest, SwitchingScopeSwitchesRegistry) {
    namespace fs = std::filesystem;
    const fs::path root1 = fs::temp_directory_path() /
        ("vitreo-hwnd-scope1-" + std::to_string(::getpid()));
    const fs::path root2 = fs::temp_directory_path() /
        ("vitreo-hwnd-scope2-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root1, ec);
    fs::remove_all(root2, ec);
    constexpr u32 a = 0x00230001u;
    constexpr u32 b = 0x00230002u;

    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root1.c_str(), 1), 0);
    hwnd_registry_publish(a, 1, 1, 7, "ScopeA", "A");
    ASSERT_TRUE(hwnd_registry_is_window(a));
    ASSERT_TRUE(hwnd_registry_find_window("ScopeA", "").has_value());

    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root2.c_str(), 1), 0);
    EXPECT_FALSE(hwnd_registry_is_window(a));
    EXPECT_FALSE(hwnd_registry_find_window("ScopeA", "").has_value());
    hwnd_registry_publish(b, 1, 1, 7, "ScopeB", "B");
    EXPECT_TRUE(hwnd_registry_is_window(b));
    EXPECT_EQ(hwnd_registry_enumerate().size(), 1u);

    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root1.c_str(), 1), 0);
    EXPECT_TRUE(hwnd_registry_is_window(a));
    EXPECT_FALSE(hwnd_registry_is_window(b));
    EXPECT_EQ(hwnd_registry_enumerate().size(), 1u);

    (void)::unsetenv("VITREO_IPC_SCOPE");
    EXPECT_FALSE(hwnd_registry_is_window(a));
    fs::remove_all(root1, ec);
    fs::remove_all(root2, ec);
}

// Shell properties (SHGetPropertyStoreForWindow) belong to the window, so a
// peer process sees and edits the same values
// (window_propstore_probe64.expected "peer-store", "after-peer").
TEST(HwndRegistryTest, ShellPropertiesAreSharedAcrossProcessesAndDieWithTheWindow) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-hwnd-shellprops-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "runtime" / "hwnds", ec);
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);
    constexpr u32 hwnd = 0x00012345u;
    WindowShellProperty aumid;
    aumid.fmtid[0] = 0x55;
    aumid.pid = 5;
    aumid.text = u"Vitreo.Probe.Window";
    ASSERT_TRUE(hwnd_registry_update_shell_properties(
        hwnd, [&](std::vector<WindowShellProperty>& values) { values.push_back(aumid); }));

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        const auto seen = hwnd_registry_shell_properties(hwnd);
        const bool ok = seen.size() == 1 && seen[0].text == u"Vitreo.Probe.Window" &&
            hwnd_registry_update_shell_properties(
                hwnd, [](std::vector<WindowShellProperty>& values) {
                    WindowShellProperty relaunch;
                    relaunch.fmtid[0] = 0x55;
                    relaunch.pid = 2;
                    relaunch.text = u"Vitreo.Probe.FromPeer";
                    values.push_back(relaunch);
                });
        ::_exit(ok ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);

    const auto values = hwnd_registry_shell_properties(hwnd);
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[0].pid, 5u);
    EXPECT_EQ(values[0].text, u"Vitreo.Probe.Window");
    EXPECT_EQ(values[1].pid, 2u);
    EXPECT_EQ(values[1].text, u"Vitreo.Probe.FromPeer");

    hwnd_registry_unpublish(hwnd);
    EXPECT_TRUE(hwnd_registry_shell_properties(hwnd).empty());

    (void)::unsetenv("VITREO_IPC_SCOPE");
    fs::remove_all(root, ec);
}

TEST(HwndRegistryTest, ShellPropertiesWithoutAScopeAreProcessLocal) {
    ASSERT_EQ(::unsetenv("VITREO_IPC_SCOPE"), 0);
    constexpr u32 hwnd = 0x00054321u;
    EXPECT_TRUE(hwnd_registry_shell_properties(hwnd).empty());
    ASSERT_TRUE(hwnd_registry_update_shell_properties(
        hwnd, [](std::vector<WindowShellProperty>& values) {
            values.push_back({{}, 9, u"-1"});
        }));
    ASSERT_EQ(hwnd_registry_shell_properties(hwnd).size(), 1u);
    hwnd_registry_unpublish(hwnd);
    EXPECT_TRUE(hwnd_registry_shell_properties(hwnd).empty());
}

} // namespace
} // namespace vitreo::ipc
