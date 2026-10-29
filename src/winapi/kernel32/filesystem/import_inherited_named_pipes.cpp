/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/filesystem/file_thunks_internal.hpp"
#include "winapi/kernel32/filesystem/file_profile_internal.hpp"
#include "winapi/kernel32/filesystem/file_path_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <unistd.h>

/// @file import_inherited_named_pipes.cpp
/// @brief FileManager::import_inherited_named_pipes
///
/// Closed method cut from file.cpp.

namespace vitreo::winapi {

bool FileManager::import_inherited_named_pipes(std::string_view descriptors) {
    usize offset = 0;
    while (offset < descriptors.size()) {
        const usize end = descriptors.find(';', offset);
        const std::string_view item = descriptors.substr(
            offset, end == std::string_view::npos ? descriptors.size() - offset
                                                   : end - offset);
        const usize first = item.find(',');
        const usize second = first == std::string_view::npos
                                 ? std::string_view::npos
                                 : item.find(',', first + 1);
        const usize third = second == std::string_view::npos
                                ? std::string_view::npos
                                : item.find(',', second + 1);
        // Every refusal names the descriptor and the step: the child exits
        // before any guest code runs, so this line is all a caller sees.
        const auto fail = [&item](const char* why) {
            std::fprintf(stderr,
                         "[ipc] inherited pipe import failed (%s): '%.*s'\n",
                         why, static_cast<int>(item.size()), item.data());
            return false;
        };
        if (first != 1 || second == std::string_view::npos ||
            third == std::string_view::npos) {
            return fail("malformed descriptor");
        }
        const Handle handle = static_cast<Handle>(
            std::stoul(std::string(item.substr(first + 1, second - first - 1))));
        const int fd = std::stoi(
            std::string(item.substr(second + 1, third - second - 1)));

        if (item[0] == 'F') {
            // F,<handle>,<fd>,<access>,<flags>
            const usize fourth = item.find(',', third + 1);
            if (fourth == std::string_view::npos) {
                return fail("malformed file descriptor");
            }
            const auto access = static_cast<u32>(std::stoul(
                std::string(item.substr(third + 1, fourth - third - 1))));
            const auto flags = static_cast<u32>(
                std::stoul(std::string(item.substr(fourth + 1))));
            auto adopted = m_vfs.adopt_native_fd(
                fd, to_access_mode(access), capsule::ShareMode::All,
                "inherited");
            if (adopted.is_err()) {
                ::close(fd);
                return fail("file fd not adoptable");
            }
            if (!m_handles.allocate_at(handle, HandleType::File, nullptr,
                                       flags, access)) {
                (void)m_vfs.close(adopted.value());
                return fail("file handle value already in use");
            }
            {
                std::lock_guard<std::mutex> lock(m_file_path_mutex);
                m_vfs_handles[handle] = adopted.value();
            }
            ipc::ipc_log(
                "inherited file as h=0x%x fd=%d access=0x%x",
                static_cast<unsigned>(handle), fd, access);
            if (end == std::string_view::npos) break;
            offset = end + 1;
            continue;
        }

        auto ep = std::make_shared<PipeEndpoint>();
        if (item[0] == 'N') {
            ep->readable = true;
            ep->writable = true;
            ep->win_name = std::string(
                decode_pipe_state(item.substr(third + 1), *ep));
            ep->sock = ipc::UnixPipe::adopt_server(fd, ep->win_name);
        } else if (item[0] == 'A') {
            const usize fourth = item.find(',', third + 1);
            if (fourth == std::string_view::npos) {
                return fail("malformed connected-pipe descriptor");
            }
            ep->readable = item.substr(third + 1, fourth - third - 1) == "1";
            // A connected endpoint that was opened by name carries it as an
            // optional trailing field.
            const usize fifth = item.find(',', fourth + 1);
            if (fifth == std::string_view::npos) {
                ep->writable = item.substr(fourth + 1) == "1";
            } else {
                ep->writable =
                    item.substr(fourth + 1, fifth - fourth - 1) == "1";
                ep->win_name = std::string(
                    decode_pipe_state(item.substr(fifth + 1), *ep));
            }
            ep->conn = ipc::UnixPipe::adopt_connected(fd);
        } else {
            return fail("unknown descriptor kind");
        }
        auto* holder = new PipeEndpointHolder(ep);
        if (!ep->sock && !ep->conn) {
            delete holder;
            return fail("pipe fd not adoptable");
        }
        if (!m_handles.allocate_at(handle, HandleType::Pipe, holder,
                                   HandleFlags::Inherit)) {
            delete holder;
            return fail("pipe handle value already in use");
        }
        // Report the endpoint's real kind. Logging every 'A' descriptor as
        // "anonymous" hid the fact that a named-pipe client had been inherited.
        const char* kind_text =
            ep->kind == PipeKind::NamedClient  ? "named-client"
            : ep->kind == PipeKind::NamedServer ? "named-server"
                                                : "anonymous";
        ipc::ipc_log(
            "inherited %s pipe as h=0x%x fd=%d name='%s' type=%s readmode=%s "
            "blocking=%d r=%d w=%d",
            kind_text, static_cast<unsigned>(handle), fd,
            ep->win_name.c_str(),
            ep->pipe_type == PipeDataMode::Message ? "message" : "byte",
            ep->read_mode == PipeDataMode::Message ? "message" : "byte",
            ep->blocking ? 1 : 0, ep->readable ? 1 : 0, ep->writable ? 1 : 0);
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return true;
}

} // namespace vitreo::winapi
