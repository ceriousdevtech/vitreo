// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_launcher.cpp
/// @brief Windows.Foundation.Uri and Windows.System.Launcher

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

#include <cstdio>
#include <string>
#include <thread>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace vitreo::winapi {

namespace {

// Same host open ShellExecute uses. WinRT activation lives in combase, which
// does not link the windowing module, so the spawn is repeated here.
bool open_host_url(const std::string& url) {
    if (url.empty()) return false;
    std::thread([url] {
        std::string arg = url;
        char* argv[] = {const_cast<char*>("/usr/bin/open"), arg.data(), nullptr};
        pid_t pid = 0;
        const int rc = posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr,
                                   argv, environ);
        std::fprintf(stderr, "[vitreo] LaunchUriAsync '%s' -> %s\n",
                     url.c_str(), rc == 0 ? "spawned" : "FAILED");
        if (rc == 0) {
            int status = 0;
            (void)waitpid(pid, &status, 0);
        }
    }).detach();
    return true;
}

} // namespace

void CombaseManager::impl_WinrtNotImplemented(Marshaler& m) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED combase!Launcher/Uri method\n");
    }
    m.set_return(E_NOTIMPL);
}

void CombaseManager::impl_UriCreate(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    auto text = read_hstring(m.arg_ptr(1), m.cpu_is_64bit());
    if (!text.has_value() || text->empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    Object object{};
    object.type = ObjectType::Uri;
    object.ref_count = 1;
    object.values.push_back(std::move(*text));
    const GuestAddr uri = allocate_object(std::move(object), m.cpu_is_64bit());
    if (uri == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, uri).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_UriGetText(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    auto it = m_objects.find(m.arg_ptr(0));
    if (out == 0 || it == m_objects.end() ||
        it->second.type != ObjectType::Uri || it->second.values.empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr text =
        create_hstring(it->second.values[0], m.cpu_is_64bit());
    if (text == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, text).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_LauncherLaunchUri(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(out, 0);
    auto uri = m_objects.find(m.arg_ptr(1));
    if (uri == m_objects.end() || uri->second.type != ObjectType::Uri ||
        uri->second.values.empty()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    std::string url;
    url.reserve(uri->second.values[0].size());
    for (char16_t unit : uri->second.values[0]) {
        if (unit < 128) url.push_back(static_cast<char>(unit));
    }
    const bool web = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    const bool opened = web && open_host_url(url);
    if (!web) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED Windows.System.Launcher."
                     "LaunchUriAsync scheme '%s'\n",
                     url.c_str());
    }
    const GuestAddr op = create_completed_async_scalar(
        opened ? 1 : 0, 1, m.cpu_is_64bit());
    if (op == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m.set_return(m.write_ptr(out, op).is_ok() ? S_OK : E_POINTER);
}

} // namespace vitreo::winapi
