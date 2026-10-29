// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_open_start_exports.cpp
/// @brief advapi32 SCM exports: OpenSCManager, EnumServicesStatus, OpenService,
///        CreateService and StartService.

#include "winapi/advapi32/services/scm_internal.hpp"

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

void register_scm_open_start_exports(ScmManager& scm, DllExports& exports) {
    // ---- SC_HANDLE OpenSCManager{A,W}(LPCSTR machine, LPCSTR db, DWORD access)
    // 3 args, stdcall_bytes=12. Remote machines are not modelled; a non-NULL
    // machine name that is not "." or the local host must fail rather than
    // silently manage the local database.
    auto open_sc_manager = [&scm](Marshaler& m, bool wide) {
        auto machine = read_guest_string(m, m.arg_ptr(0), wide);
        if (machine && !machine->empty() && *machine != L"." &&
            machine->rfind(L"\\\\", 0) == 0) {
            VITREO_TRACE("[scm] OpenSCManager: remote machine '%s' unsupported\n",
                         to_narrow(*machine).c_str());
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            m.set_return(0);
            return;
        }
        const u32 handle = scm.open_manager_handle();
        VITREO_TRACE("[scm] OpenSCManager%s -> 0x%x\n", wide ? "W" : "A", handle);
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(handle);
    };
    exports.entries.push_back({"OpenSCManagerA", 0,
        [open_sc_manager](Marshaler& m) { open_sc_manager(m, false); }, 12});
    exports.entries.push_back({"OpenSCManagerW", 0,
        [open_sc_manager](Marshaler& m) { open_sc_manager(m, true); }, 12});

    // ---- BOOL EnumServicesStatusA(SC_HANDLE, DWORD type, DWORD state,
    //          LPENUM_SERVICE_STATUSA, DWORD bytes, LPDWORD needed,
    //          LPDWORD returned, LPDWORD resume)
    // 8 args, stdcall_bytes=32.
    exports.entries.push_back({"EnumServicesStatusA", 0,
        [&scm](Marshaler& m) {
            constexpr u32 SERVICE_ACTIVE = 1;
            constexpr u32 SERVICE_INACTIVE = 2;
            const u32 manager = static_cast<u32>(m.arg(0));
            const u32 requested_type = static_cast<u32>(m.arg(1));
            const u32 requested_state = static_cast<u32>(m.arg(2));
            const GuestAddr buffer = m.arg_ptr(3);
            const u32 buffer_size = static_cast<u32>(m.arg(4));
            const GuestAddr needed_out = m.arg_ptr(5);
            const GuestAddr returned_out = m.arg_ptr(6);
            const GuestAddr resume_out = m.arg_ptr(7);
            if (!scm.is_manager_handle(manager)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            if (requested_type == 0 ||
                (requested_state & ~(SERVICE_ACTIVE | SERVICE_INACTIVE)) != 0 ||
                requested_state == 0 || needed_out == 0 || returned_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            struct Entry {
                std::string name;
                std::string display;
                ServiceStatus status;
            };
            std::vector<Entry> entries;
            for (const auto& config : scm.enumerate_services()) {
                const ServiceStatus status = scm.query_status(config.name);
                const bool active = status.current_state != SERVICE_STOPPED;
                const bool state_matches = active
                    ? (requested_state & SERVICE_ACTIVE) != 0
                    : (requested_state & SERVICE_INACTIVE) != 0;
                if (!state_matches ||
                    (status.service_type & requested_type) == 0) {
                    continue;
                }
                std::string name = to_narrow(config.name);
                std::string display = config.display_name.empty()
                    ? name : to_narrow(config.display_name);
                entries.push_back({std::move(name), std::move(display), status});
            }

            u32 resume = 0;
            if (resume_out != 0) {
                resume = m.read_u32(resume_out).value_or(0);
            }
            if (resume > entries.size()) resume = static_cast<u32>(entries.size());
            const bool is64 = m.cpu_is_64bit();
            const u32 struct_size = is64 ? 48u : 36u;
            const u32 status_offset = is64 ? 16u : 8u;
            auto bytes_for = [&](usize first, usize count) -> u32 {
                usize bytes = count * struct_size;
                for (usize i = 0; i < count; ++i) {
                    bytes += entries[first + i].name.size() + 1;
                    bytes += entries[first + i].display.size() + 1;
                }
                return bytes > UINT32_MAX ? UINT32_MAX : static_cast<u32>(bytes);
            };

            const usize first = resume;
            usize count = 0;
            while (buffer != 0 && first + count < entries.size() &&
                   bytes_for(first, count + 1) <= buffer_size) {
                ++count;
            }
            const u32 remaining_bytes =
                bytes_for(first + count, entries.size() - first - count);
            (void)m.write_u32(needed_out, remaining_bytes);
            (void)m.write_u32(returned_out, static_cast<u32>(count));

            if (buffer != 0 && count != 0) {
                GuestAddr string_cursor =
                    buffer + static_cast<GuestAddr>(count * struct_size);
                auto write_pointer = [&](GuestAddr address, GuestAddr value) {
                    if (is64) (void)m.write_u64(address, value);
                    else (void)m.write_u32(address, static_cast<u32>(value));
                };
                for (usize i = 0; i < count; ++i) {
                    const GuestAddr record =
                        buffer + static_cast<GuestAddr>(i * struct_size);
                    write_pointer(record, string_cursor);
                    (void)m.write_ansi_string(string_cursor,
                                              entries[first + i].name);
                    string_cursor += entries[first + i].name.size() + 1;
                    write_pointer(record + (is64 ? 8u : 4u), string_cursor);
                    (void)m.write_ansi_string(string_cursor,
                                              entries[first + i].display);
                    string_cursor += entries[first + i].display.size() + 1;
                    write_service_status(m, record + status_offset,
                                         entries[first + i].status, false);
                }
            }

            const bool complete = first + count == entries.size();
            if (resume_out != 0) {
                (void)m.write_u32(resume_out, complete
                    ? 0u : static_cast<u32>(first + count));
            }
            if (!complete) {
                set_last_error((buffer == 0 || buffer_size == 0)
                    ? error_codes::ERROR_INSUFFICIENT_BUFFER
                    : error_codes::ERROR_MORE_DATA);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 32});

    // ---- SC_HANDLE OpenService{A,W}(SC_HANDLE scm, LPCSTR name, DWORD access)
    // 3 args, stdcall_bytes=12.
    auto open_service = [&scm](Marshaler& m, bool wide) {
        const u32 manager = static_cast<u32>(m.arg(0));
        if (!scm.is_manager_handle(manager)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        auto name = read_guest_string(m, m.arg_ptr(1), wide);
        if (!name || name->empty()) {
            set_last_error(error_codes::ERROR_INVALID_NAME);
            m.set_return(0);
            return;
        }
        if (!scm.service_exists(*name)) {
            VITREO_TRACE("[scm] OpenService '%s' -> does not exist\n",
                         to_narrow(*name).c_str());
            set_last_error(error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
            m.set_return(0);
            return;
        }
        const u32 handle = scm.open_service_handle(*name);
        VITREO_TRACE("[scm] OpenService%s '%s' -> 0x%x\n", wide ? "W" : "A",
                     to_narrow(*name).c_str(), handle);
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(handle);
    };
    exports.entries.push_back({"OpenServiceA", 0,
        [open_service](Marshaler& m) { open_service(m, false); }, 12});
    exports.entries.push_back({"OpenServiceW", 0,
        [open_service](Marshaler& m) { open_service(m, true); }, 12});

    // ---- SC_HANDLE CreateService{A,W}(scm, name, display, access, type,
    //          start, errorControl, binaryPath, loadOrderGroup, tagId,
    //          dependencies, startName, password)
    // 13 args, stdcall_bytes=52.
    auto create_service = [&scm](Marshaler& m, bool wide) {
        const u32 manager = static_cast<u32>(m.arg(0));
        if (!scm.is_manager_handle(manager)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        auto name = read_guest_string(m, m.arg_ptr(1), wide);
        if (!name || name->empty()) {
            set_last_error(error_codes::ERROR_INVALID_NAME);
            m.set_return(0);
            return;
        }

        ServiceConfig config;
        config.name = *name;
        if (auto display = read_guest_string(m, m.arg_ptr(2), wide)) {
            config.display_name = *display;
        }
        config.service_type  = static_cast<u32>(m.arg(4));
        config.start_type    = static_cast<u32>(m.arg(5));
        config.error_control = static_cast<u32>(m.arg(6));
        if (auto binary = read_guest_string(m, m.arg_ptr(7), wide)) {
            config.image_path = *binary;
        }
        if (auto start_name = read_guest_string(m, m.arg_ptr(11), wide)) {
            config.object_name = *start_name;
        }

        // lpdwTagId is an out parameter for boot/system-start drivers only.
        if (const GuestAddr tag_ptr = m.arg_ptr(9); tag_ptr != 0) {
            (void)m.write_u32(tag_ptr, 0);
        }

        u32 error = 0;
        if (!scm.create_service(config, error)) {
            set_last_error(error);
            m.set_return(0);
            return;
        }
        const u32 handle = scm.open_service_handle(config.name);
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(handle);
    };
    exports.entries.push_back({"CreateServiceA", 0,
        [create_service](Marshaler& m) { create_service(m, false); }, 52});
    exports.entries.push_back({"CreateServiceW", 0,
        [create_service](Marshaler& m) { create_service(m, true); }, 52});

    // ---- BOOL StartService{A,W}(SC_HANDLE, DWORD argc, LPCSTR* argv)
    // 3 args, stdcall_bytes=12.
    auto start_service = [&scm](Marshaler& m, bool wide) {
        auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
        if (!name) {
            m.set_return(0);
            return;
        }

        std::vector<std::wstring> args;
        const u32 argc = static_cast<u32>(m.arg(1));
        const GuestAddr argv = m.arg_ptr(2);
        if (argv != 0 && argc > 0 && argc < 256) {
            const bool is64 = m.cpu_is_64bit();
            for (u32 i = 0; i < argc; ++i) {
                const GuestAddr slot = argv + i * (is64 ? 8u : 4u);
                auto ptr = is64 ? m.read_u64(slot) : [&]() {
                    auto r = m.read_u32(slot);
                    return r.is_ok() ? VResult<u64>::ok(r.value())
                                     : VResult<u64>::err(r.error());
                }();
                if (ptr.is_err() || ptr.value() == 0) continue;
                if (auto arg = read_guest_string(
                        m, static_cast<GuestAddr>(ptr.value()), wide)) {
                    args.push_back(*arg);
                }
            }
        }

        u32 error = 0;
        if (!scm.start_service(*name, args, error)) {
            set_last_error(error);
            m.set_return(0);
            return;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"StartServiceA", 0,
        [start_service](Marshaler& m) { start_service(m, false); }, 12});
    exports.entries.push_back({"StartServiceW", 0,
        [start_service](Marshaler& m) { start_service(m, true); }, 12});
}

} // namespace vitreo::winapi
