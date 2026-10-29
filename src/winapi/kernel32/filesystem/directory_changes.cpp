/// @file directory_changes.cpp
/// @brief ReadDirectoryChangesW change tracking (see header).

#include "winapi/kernel32/filesystem/directory_changes.hpp"

#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <sys/stat.h>

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace vitreo::winapi {

namespace dirchange {

namespace {

void utf8_to_utf16(const std::string& text, std::vector<u16>& out) {
    for (usize i = 0; i < text.size();) {
        u32 c = static_cast<u8>(text[i]);
        usize n = 1;
        if (c >= 0xF0) { c &= 0x07; n = 4; }
        else if (c >= 0xE0) { c &= 0x0F; n = 3; }
        else if (c >= 0xC0) { c &= 0x1F; n = 2; }
        for (usize k = 1; k < n && i + k < text.size(); ++k) {
            c = (c << 6) | (static_cast<u8>(text[i + k]) & 0x3F);
        }
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<u16>(0xD800 | (c >> 10)));
            out.push_back(static_cast<u16>(0xDC00 | (c & 0x3FF)));
        } else {
            out.push_back(static_cast<u16>(c));
        }
    }
}

}  // namespace

std::vector<Record> translate(const std::vector<HostEvent>& events, bool subtree, u32 filter,
                              std::vector<std::string>& added) {
    std::vector<Record> out;
    std::vector<Record> pending_old;   // rename sources awaiting their target
    const auto emit = [&](u32 action, const std::string& name, bool is_directory) {
        const u32 name_bit = is_directory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME;
        if ((filter & name_bit) == 0) return;
        out.push_back(Record{action, name});
    };
    const auto flush_old = [&] {
        for (const auto& old : pending_old) out.push_back(Record{FILE_ACTION_REMOVED, old.name});
        pending_old.clear();
    };
    for (const auto& event : events) {
        if (event.relative.empty()) continue;   // the watched directory itself
        if (!subtree && event.relative.find('/') != std::string::npos) continue;
        std::string name = event.relative;
        std::replace(name.begin(), name.end(), '/', '\\');
        const u32 name_bit =
            event.is_directory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME;
        const auto was_added = std::find(added.begin(), added.end(), name);
        if (event.renamed) {
            if (!event.exists_now) {
                if (filter & name_bit) pending_old.push_back(Record{FILE_ACTION_RENAMED_OLD_NAME, name});
                if (was_added != added.end()) added.erase(was_added);
            } else if (!pending_old.empty()) {
                out.push_back(pending_old.front());
                pending_old.erase(pending_old.begin());
                emit(FILE_ACTION_RENAMED_NEW_NAME, name, event.is_directory);
                added.push_back(name);
            } else {
                // Moved in from outside the watched tree.
                emit(FILE_ACTION_ADDED, name, event.is_directory);
                added.push_back(name);
            }
            continue;
        }
        flush_old();
        bool just_added = false;
        if (event.created && event.exists_now && was_added == added.end() &&
            event.born_after_start) {
            if (event.removed) emit(FILE_ACTION_REMOVED, name, event.is_directory);
            emit(FILE_ACTION_ADDED, name, event.is_directory);
            added.push_back(name);
            just_added = true;
        } else if (!event.exists_now && (event.removed || event.created)) {
            if (event.created && was_added == added.end() && !event.removed) {
                emit(FILE_ACTION_ADDED, name, event.is_directory);   // created and gone
            }
            emit(FILE_ACTION_REMOVED, name, event.is_directory);
            if (was_added != added.end()) added.erase(std::find(added.begin(), added.end(), name));
            continue;
        }
        if (!event.exists_now) continue;
        // Content changes are size/last-write; metadata changes attributes,
        // security, times. A directory's own metadata changes when entries
        // come and go; Windows does not report that (probe "subfile").
        u32 modify_bits = 0;
        if (event.content_modified) modify_bits |= FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
        if (event.metadata_modified && !just_added && !event.is_directory) {
            modify_bits |= FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY |
                           FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION |
                           FILE_NOTIFY_CHANGE_LAST_WRITE;
        }
        if ((filter & modify_bits) != 0) out.push_back(Record{FILE_ACTION_MODIFIED, name});
    }
    flush_old();
    return out;
}

std::optional<std::vector<u8>> serialize(const std::vector<Record>& records, u32 capacity) {
    std::vector<u8> out;
    for (usize i = 0; i < records.size(); ++i) {
        std::vector<u16> name;
        utf8_to_utf16(records[i].name, name);
        const usize start = out.size();
        const usize length = 12 + name.size() * 2;
        const bool last = i + 1 == records.size();
        const usize next = last ? 0 : (length + 3) & ~usize{3};
        out.resize(start + (last ? length : next), 0);
        const u32 fields[3] = {static_cast<u32>(next), records[i].action,
                               static_cast<u32>(name.size() * 2)};
        std::memcpy(out.data() + start, fields, sizeof(fields));
        std::memcpy(out.data() + start + 12, name.data(), name.size() * 2);
    }
    if (out.size() > capacity) return std::nullopt;
    return out;
}

}  // namespace dirchange

// -----------------------------------------------------------------------------
// Watches
// -----------------------------------------------------------------------------

struct DirectoryChangeService::Watch {
    u32 handle = 0;
    std::string root;   ///< resolved host path
    u32 capacity = 0;   ///< the first call's buffer length
    bool subtree = false;
    u32 filter = 0;
    i64 start_seconds = 0;
    std::vector<dirchange::Record> queued;
    usize queued_bytes = 0;
    bool overflow = false;
    std::vector<std::string> added;
    std::deque<Request> pending;
    struct SyncWaiter {
        bool done = false;
        u32 status = 0;
        u32 bytes = 0;
        std::vector<u8> data;
    };
    std::deque<std::shared_ptr<SyncWaiter>> sync_waiters;
    std::condition_variable sync_cv;
    bool closed = false;
#ifdef __APPLE__
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
#endif
};

DirectoryChangeService::DirectoryChangeService(GuestMemory& memory, Completion completion)
    : m_memory(memory), m_completion(std::move(completion)) {}

DirectoryChangeService::~DirectoryChangeService() {
    std::vector<u32> handles;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& [handle, watch] : m_watches) handles.push_back(handle);
    }
    for (u32 handle : handles) close(handle);
}

#ifdef __APPLE__
namespace {

struct StreamContext {
    DirectoryChangeService* service;
    u32 handle;
    std::string root;
    i64 start_seconds;
};

void stream_callback(ConstFSEventStreamRef, void* info, size_t count, void* paths,
                     const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
    auto* context = static_cast<StreamContext*>(info);
    auto** list = static_cast<char**>(paths);
    std::vector<dirchange::HostEvent> events;
    bool must_rescan = false;
    for (size_t i = 0; i < count; ++i) {
        const FSEventStreamEventFlags f = flags[i];
        if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                 kFSEventStreamEventFlagKernelDropped)) {
            must_rescan = true;
            continue;
        }
        std::string path = list[i];
        if (path.size() < context->root.size() || path.compare(0, context->root.size(), context->root) != 0) {
            continue;
        }
        std::string relative = path.substr(context->root.size());
        if (!relative.empty() && relative.front() != '/') continue;   // a sibling sharing the prefix
        if (!relative.empty()) relative.erase(0, 1);
        dirchange::HostEvent event;
        event.relative = relative;
        event.is_directory = (f & kFSEventStreamEventFlagItemIsDir) != 0;
        event.created = (f & kFSEventStreamEventFlagItemCreated) != 0;
        event.removed = (f & kFSEventStreamEventFlagItemRemoved) != 0;
        event.renamed = (f & kFSEventStreamEventFlagItemRenamed) != 0;
        event.content_modified = (f & kFSEventStreamEventFlagItemModified) != 0;
        event.metadata_modified =
            (f & (kFSEventStreamEventFlagItemInodeMetaMod | kFSEventStreamEventFlagItemChangeOwner |
                  kFSEventStreamEventFlagItemXattrMod | kFSEventStreamEventFlagItemFinderInfoMod)) != 0;
        struct stat st{};
        event.exists_now = ::lstat(path.c_str(), &st) == 0;
        if (event.exists_now) {
            event.is_directory = S_ISDIR(st.st_mode);
            event.born_after_start = st.st_birthtimespec.tv_sec >= context->start_seconds - 1;
        }
        events.push_back(std::move(event));
    }
    if (must_rescan) {
        // The kernel dropped events: Windows' equivalent is an overflowed
        // buffer, which tells the caller to rescan.
        dirchange::HostEvent overflow;
        overflow.relative = "\x01overflow";
        events.push_back(overflow);
    }
    if (!events.empty()) context->service->deliver(context->handle, events);
}

}  // namespace
#endif

std::shared_ptr<DirectoryChangeService::Watch> DirectoryChangeService::watch_for(
        const Request& request, bool& created) {
    created = false;
    std::lock_guard lock(m_mutex);
    auto found = m_watches.find(request.handle);
    if (found != m_watches.end()) return found->second;
    auto watch = std::make_shared<Watch>();
    watch->handle = request.handle;
    char resolved[PATH_MAX] = {};
    watch->root = ::realpath(request.host_directory.c_str(), resolved) ? resolved
                                                                       : request.host_directory;
    while (watch->root.size() > 1 && watch->root.back() == '/') watch->root.pop_back();
    watch->capacity = request.length;
    watch->start_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    m_watches[request.handle] = watch;
    created = true;
    return watch;
}

bool DirectoryChangeService::ensure_stream(const std::shared_ptr<Watch>& watch) {
#ifdef __APPLE__
    {
        std::lock_guard lock(m_mutex);
        if (watch->stream != nullptr || watch->root.empty()) return true;
    }
    auto* context = new StreamContext{this, watch->handle, watch->root, watch->start_seconds};
    FSEventStreamContext stream_context{
        0, context, nullptr,
        [](const void* info) { delete static_cast<const StreamContext*>(info); }, nullptr};
    CFStringRef path = CFStringCreateWithCString(nullptr, watch->root.c_str(), kCFStringEncodingUTF8);
    CFArrayRef paths =
        CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1, &kCFTypeArrayCallBacks);
    FSEventStreamRef stream = FSEventStreamCreate(
        nullptr, &stream_callback, &stream_context, paths, kFSEventStreamEventIdSinceNow, 0.03,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    CFRelease(path);
    if (stream == nullptr) return false;
    dispatch_queue_t queue =
        dispatch_queue_create("vitreo.ReadDirectoryChangesW", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream)) {
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        return false;
    }
    std::lock_guard lock(m_mutex);
    watch->stream = stream;
    watch->queue = queue;
    return true;
#else
    (void)watch;
    return false;
#endif
}

bool DirectoryChangeService::start(const Request& request) {
    bool created = false;
    auto watch = watch_for(request, created);
    if (!ensure_stream(watch)) {
        if (created) {
            std::lock_guard lock(m_mutex);
            m_watches.erase(request.handle);
        }
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        watch->subtree = request.subtree;
        watch->filter = request.filter;
        watch->pending.push_back(request);
    }
    pump(watch);
    return true;
}

u32 DirectoryChangeService::wait(const Request& request, u32& bytes) {
    bool created = false;
    auto watch = watch_for(request, created);
    if (!ensure_stream(watch)) {
        if (created) {
            std::lock_guard lock(m_mutex);
            m_watches.erase(request.handle);
        }
        return 0xC0000001u;   // STATUS_UNSUCCESSFUL
    }
    auto waiter = std::make_shared<Watch::SyncWaiter>();
    Request sync = request;
    sync.overlapped = 0;
    sync.routine = 0;
    {
        std::lock_guard lock(m_mutex);
        watch->subtree = request.subtree;
        watch->filter = request.filter;
        watch->sync_waiters.push_back(waiter);
        watch->pending.push_back(sync);
    }
    pump(watch);
    std::unique_lock lock(m_mutex);
    watch->sync_cv.wait(lock, [&] { return waiter->done; });
    bytes = waiter->bytes;
    lock.unlock();
    if (!waiter->data.empty()) {
        (void)m_memory.write_bytes(request.buffer, waiter->data.data(), waiter->data.size());
    }
    return waiter->status;
}

void DirectoryChangeService::deliver(u32 handle, const std::vector<dirchange::HostEvent>& events) {
    std::shared_ptr<Watch> watch;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_watches.find(handle);
        if (found == m_watches.end() || found->second->closed) return;
        watch = found->second;
        std::vector<dirchange::HostEvent> real;
        for (const auto& event : events) {
            if (event.relative == "\x01overflow") watch->overflow = true;
            else real.push_back(event);
        }
        auto records = dirchange::translate(real, watch->subtree, watch->filter, watch->added);
        for (auto& record : records) {
            const usize size = (12 + record.name.size() * 2 + 3) & ~usize{3};
            watch->queued_bytes += size;
            watch->queued.push_back(std::move(record));
        }
        if (watch->queued_bytes > watch->capacity) {
            watch->overflow = true;
        }
        if (watch->overflow) {
            watch->queued.clear();
            watch->queued_bytes = 0;
        }
    }
    pump(watch);
}

void DirectoryChangeService::pump(const std::shared_ptr<Watch>& watch) {
    for (;;) {
        Request request;
        u32 status = 0;
        std::vector<u8> data;
        {
            std::lock_guard lock(m_mutex);
            if (watch->pending.empty() || (watch->queued.empty() && !watch->overflow)) return;
            request = watch->pending.front();
            watch->pending.pop_front();
            if (watch->overflow) {
                status = dirchange::STATUS_NOTIFY_ENUM_DIR;
            } else if (auto image = dirchange::serialize(watch->queued, request.length)) {
                data = std::move(*image);
            } else {
                status = dirchange::STATUS_NOTIFY_ENUM_DIR;   // measured: too small
            }
            watch->queued.clear();
            watch->queued_bytes = 0;
            watch->overflow = false;
            if (request.overlapped == 0 && !watch->sync_waiters.empty()) {
                auto waiter = watch->sync_waiters.front();
                watch->sync_waiters.pop_front();
                waiter->done = true;
                waiter->status = status;
                waiter->bytes = static_cast<u32>(data.size());
                waiter->data = std::move(data);
                watch->sync_cv.notify_all();
                continue;
            }
        }
        if (!data.empty()) (void)m_memory.write_bytes(request.buffer, data.data(), data.size());
        const u32 bytes = static_cast<u32>(data.size());
        const u32 error = status == 0 ? 0 : dirchange::ERROR_NOTIFY_ENUM_DIR;
        if (request.overlapped != 0 && m_completion.overlapped) {
            m_completion.overlapped(request.handle, bytes, request.overlapped, request.is_64bit,
                                    status, error);
        }
        if (request.routine != 0 && m_completion.routine) {
            m_completion.routine(request.thread_id, request.routine, error, bytes, request.overlapped);
        }
    }
}

usize DirectoryChangeService::cancel(u32 handle, GuestAddr overlapped, u32 thread_id) {
    std::vector<Request> cancelled;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_watches.find(handle);
        if (found == m_watches.end()) return 0;
        auto& pending = found->second->pending;
        for (auto it = pending.begin(); it != pending.end();) {
            const bool match = it->overlapped != 0 &&
                               (overlapped == 0 || it->overlapped == overlapped) &&
                               (thread_id == 0 || it->thread_id == thread_id);
            if (match) {
                cancelled.push_back(*it);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& request : cancelled) {
        if (m_completion.overlapped) {
            m_completion.overlapped(request.handle, 0, request.overlapped, request.is_64bit,
                                    dirchange::STATUS_CANCELLED, dirchange::ERROR_OPERATION_ABORTED);
        }
        if (request.routine != 0 && m_completion.routine) {
            m_completion.routine(request.thread_id, request.routine,
                                 dirchange::ERROR_OPERATION_ABORTED, 0, request.overlapped);
        }
    }
    return cancelled.size();
}

void DirectoryChangeService::close(u32 handle) {
    std::shared_ptr<Watch> watch;
    std::deque<Request> pending;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_watches.find(handle);
        if (found == m_watches.end()) return;
        watch = found->second;
        m_watches.erase(found);
        watch->closed = true;
        pending.swap(watch->pending);
        for (auto& waiter : watch->sync_waiters) {
            waiter->done = true;
            waiter->status = dirchange::STATUS_NOTIFY_CLEANUP;
        }
        watch->sync_waiters.clear();
        watch->sync_cv.notify_all();
    }
#ifdef __APPLE__
    if (watch->stream != nullptr) {
        FSEventStreamStop(watch->stream);
        FSEventStreamInvalidate(watch->stream);
        FSEventStreamRelease(watch->stream);
        // Drain a callback already running on the queue before the watch goes.
        dispatch_sync(watch->queue, ^{});
        watch->stream = nullptr;
    }
#endif
    for (const auto& request : pending) {
        if (request.overlapped != 0 && m_completion.overlapped) {
            m_completion.overlapped(request.handle, 0, request.overlapped, request.is_64bit,
                                    dirchange::STATUS_NOTIFY_CLEANUP, dirchange::ERROR_NOTIFY_CLEANUP);
        }
        if (request.routine != 0 && m_completion.routine) {
            m_completion.routine(request.thread_id, request.routine,
                                 dirchange::ERROR_NOTIFY_CLEANUP, 0, request.overlapped);
        }
    }
}

}  // namespace vitreo::winapi
