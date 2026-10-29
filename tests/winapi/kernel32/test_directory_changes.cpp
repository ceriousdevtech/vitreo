/// @file test_directory_changes.cpp
/// @brief ReadDirectoryChangesW tracking: record translation, layout, request
///        completion, and real FSEvents end to end.
///
/// Expected values: tools/oracle_probe/read_dir_changes_probe{32,64}.expected
/// (identical on both bitnesses).

#include <gtest/gtest.h>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/filesystem/directory_changes.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dirchange;

namespace {

constexpr u32 kAllFilters = 0x15F;   // the probe's ALL_FILTERS

HostEvent created(const std::string& name, bool dir = false) {
    HostEvent e; e.relative = name; e.created = true; e.exists_now = true; e.is_directory = dir; return e;
}
HostEvent removed(const std::string& name, bool dir = false) {
    HostEvent e; e.relative = name; e.removed = true; e.exists_now = false; e.is_directory = dir; return e;
}
HostEvent modified(const std::string& name) {
    HostEvent e; e.relative = name; e.content_modified = true; e.exists_now = true; return e;
}
HostEvent renamed(const std::string& name, bool exists) {
    HostEvent e; e.relative = name; e.renamed = true; e.exists_now = exists; return e;
}

std::vector<Record> run(std::vector<HostEvent> events, bool subtree = false, u32 filter = kAllFilters) {
    std::vector<std::string> added;
    return translate(events, subtree, filter, added);
}

}  // namespace

TEST(DirectoryChangesTranslate, OperationsMatchWindowsRecords) {
    EXPECT_EQ(run({created("a.txt")}), (std::vector<Record>{{1, "a.txt"}}));
    EXPECT_EQ(run({modified("a.txt")}), (std::vector<Record>{{3, "a.txt"}}));
    EXPECT_EQ(run({renamed("a.txt", false), renamed("b.txt", true)}),
              (std::vector<Record>{{4, "a.txt"}, {5, "b.txt"}}));
    EXPECT_EQ(run({removed("b.txt")}), (std::vector<Record>{{2, "b.txt"}}));
    EXPECT_EQ(run({created("sub", true)}), (std::vector<Record>{{1, "sub"}}));
    EXPECT_EQ(run({removed("sub", true)}), (std::vector<Record>{{2, "sub"}}));
}

TEST(DirectoryChangesTranslate, SubtreeAndFilters) {
    // Without bWatchSubtree a change inside "sub" is not reported at all.
    EXPECT_TRUE(run({created("sub/x.txt")}).empty());
    EXPECT_EQ(run({created("sub/x.txt")}, true), (std::vector<Record>{{1, "sub\\x.txt"}}));
    EXPECT_EQ(run({removed("sub/x.txt")}, true), (std::vector<Record>{{2, "sub\\x.txt"}}));
    // FILE_NAME only: directories and modifications are filtered out.
    EXPECT_TRUE(run({created("sub", true)}, false, FILE_NOTIFY_CHANGE_FILE_NAME).empty());
    EXPECT_TRUE(run({modified("a.txt")}, false, FILE_NOTIFY_CHANGE_FILE_NAME).empty());
    EXPECT_TRUE(run({created("a.txt")}, false, FILE_NOTIFY_CHANGE_LAST_WRITE).empty());
}

TEST(DirectoryChangesTranslate, HostQuirks) {
    // FSEvents keeps "created" on later events for the same path: one ADDED.
    std::vector<std::string> added;
    HostEvent write = created("a.txt");
    write.content_modified = true;
    EXPECT_EQ(translate({created("a.txt")}, false, kAllFilters, added),
              (std::vector<Record>{{1, "a.txt"}}));
    EXPECT_EQ(translate({write}, false, kAllFilters, added), (std::vector<Record>{{3, "a.txt"}}));
    // A rename whose source left the tree: the target is ADDED; a source
    // without a target is REMOVED.
    EXPECT_EQ(run({renamed("in.txt", true)}), (std::vector<Record>{{1, "in.txt"}}));
    EXPECT_EQ(run({renamed("out.txt", false)}), (std::vector<Record>{{2, "out.txt"}}));
    // Created and already gone: both are reported.
    HostEvent flash = created("tmp.txt");
    flash.exists_now = false;
    EXPECT_EQ(run({flash}), (std::vector<Record>{{1, "tmp.txt"}, {2, "tmp.txt"}}));
    // A path that existed before tracking began is not "added".
    HostEvent old = created("old.txt");
    old.born_after_start = false;
    EXPECT_TRUE(run({old}).empty());
}

TEST(DirectoryChangesSerialize, LayoutMatchesWindows) {
    // Probe "create": bytes=22 [1 a.txt next=0]; "rename": bytes=46, next=24.
    auto one = serialize({{1, "a.txt"}}, 8192);
    ASSERT_TRUE(one);
    EXPECT_EQ(one->size(), 22u);
    auto two = serialize({{4, "a.txt"}, {5, "b.txt"}}, 8192);
    ASSERT_TRUE(two);
    EXPECT_EQ(two->size(), 46u);
    u32 next = 0, action = 0, length = 0;
    std::memcpy(&next, two->data(), 4);
    std::memcpy(&action, two->data() + 4, 4);
    std::memcpy(&length, two->data() + 8, 4);
    EXPECT_EQ(next, 24u);
    EXPECT_EQ(action, 4u);
    EXPECT_EQ(length, 10u);
    EXPECT_EQ((*two)[12], 'a');
    std::memcpy(&next, two->data() + 24, 4);
    EXPECT_EQ(next, 0u);
    // Probe "tiny buffer (8 bytes)": does not fit.
    EXPECT_FALSE(serialize({{1, "a.txt"}}, 8));
}

namespace {

struct Completed {
    u32 bytes = 0, status = 0, error = 0;
    GuestAddr overlapped = 0;
};

class DirectoryChangeServiceTest : public ::testing::Test {
protected:
    static constexpr GuestAddr BUF = 0x10000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(BUF, 0x10000, MemoryProtection::ReadWrite).is_ok());
    }

    DirectoryChangeService::Request request(GuestAddr overlapped, u32 length = 4096) {
        DirectoryChangeService::Request r;
        r.handle = 0x40;
        r.buffer = BUF;
        r.length = length;
        r.filter = kAllFilters;
        r.overlapped = overlapped;
        return r;
    }

    GuestMemory memory;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Completed> completed;
    DirectoryChangeService service{memory, DirectoryChangeService::Completion{
        [this](u32, u32 bytes, GuestAddr ov, bool, u32 status, u32 error) {
            std::lock_guard lock(mutex);
            completed.push_back({bytes, status, error, ov});
            cv.notify_all();
        },
        nullptr}};
};

}  // namespace

TEST_F(DirectoryChangeServiceTest, CompletesBuffersOverflowsCancelsAndCloses) {
    ASSERT_TRUE(service.start(request(0x100)));
    EXPECT_TRUE(completed.empty()) << "pending until a change";
    service.deliver(0x40, {created("a.txt")});
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].bytes, 22u);
    EXPECT_EQ(completed[0].status, 0u);

    // Changes while nothing is pending complete the next request at once.
    service.deliver(0x40, {modified("a.txt"), renamed("a.txt", false), renamed("b.txt", true)});
    ASSERT_TRUE(service.start(request(0x200)));
    ASSERT_EQ(completed.size(), 2u);
    EXPECT_EQ(completed[1].bytes, 24u + 24u + 22u);

    // More than the first call's buffer: 0 bytes, STATUS_NOTIFY_ENUM_DIR.
    std::vector<HostEvent> many;
    for (int i = 0; i < 300; ++i) many.push_back(created("many_" + std::to_string(i) + ".txt"));
    service.deliver(0x40, many);
    ASSERT_TRUE(service.start(request(0x300)));
    ASSERT_EQ(completed.size(), 3u);
    EXPECT_EQ(completed[2].bytes, 0u);
    EXPECT_EQ(completed[2].status, STATUS_NOTIFY_ENUM_DIR);
    EXPECT_EQ(completed[2].error, ERROR_NOTIFY_ENUM_DIR);

    // A request buffer too small for the records: also ENUM_DIR.
    ASSERT_TRUE(service.start(request(0x400, 8)));
    service.deliver(0x40, {created("c.txt")});
    ASSERT_EQ(completed.size(), 4u);
    EXPECT_EQ(completed[3].status, STATUS_NOTIFY_ENUM_DIR);

    // CancelIoEx: STATUS_CANCELLED / ERROR_OPERATION_ABORTED.
    ASSERT_TRUE(service.start(request(0x500)));
    EXPECT_EQ(service.cancel(0x40, 0x999, 0), 0u) << "a different OVERLAPPED";
    EXPECT_EQ(service.cancel(0x40, 0x500, 0), 1u);
    ASSERT_EQ(completed.size(), 5u);
    EXPECT_EQ(completed[4].status, STATUS_CANCELLED);
    EXPECT_EQ(completed[4].error, ERROR_OPERATION_ABORTED);

    // CloseHandle: STATUS_NOTIFY_CLEANUP, 0 bytes.
    ASSERT_TRUE(service.start(request(0x600)));
    service.close(0x40);
    ASSERT_EQ(completed.size(), 6u);
    EXPECT_EQ(completed[5].status, STATUS_NOTIFY_CLEANUP);
    EXPECT_EQ(completed[5].bytes, 0u);
}

TEST_F(DirectoryChangeServiceTest, SynchronousRequestBlocksUntilAChange) {
    u32 bytes = 0;
    u32 status = 0xFFFFFFFFu;
    std::thread waiter([&] { status = service.wait(request(0), bytes); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    service.deliver(0x40, {created("a.txt")});
    waiter.join();
    EXPECT_EQ(status, 0u);
    EXPECT_EQ(bytes, 22u);
    EXPECT_EQ(memory.read_u32(BUF + 4).value_or(0), 1u);
}

// Real host events: FSEvents on a temporary directory.
TEST_F(DirectoryChangeServiceTest, HostFileEventsReachAPendingRequest) {
    char templ[] = "/tmp/vitreo_rdcw_XXXXXX";
    const std::string dir = ::mkdtemp(templ);
    auto r = request(0x100);
    r.host_directory = dir;
    ASSERT_TRUE(service.start(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // stream warm-up
    std::ofstream(dir + "/hello.txt") << "";
    std::unique_lock lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return !completed.empty(); }))
        << "no FSEvents notification within 5 s";
    lock.unlock();
    EXPECT_EQ(completed[0].status, 0u);
    ASSERT_GE(completed[0].bytes, 12u);
    EXPECT_EQ(memory.read_u32(BUF + 4).value_or(0), 1u) << "FILE_ACTION_ADDED";
    EXPECT_EQ(memory.read_u16(BUF + 12).value_or(0), u'h');
    service.close(0x40);
    std::filesystem::remove_all(dir);
}
