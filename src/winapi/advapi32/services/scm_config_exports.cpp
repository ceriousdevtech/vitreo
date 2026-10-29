// SPDX-License-Identifier: LGPL-2.1-only
/// @file scm_config_exports.cpp
/// @brief advapi32 SCM exports: ChangeServiceConfig(2) and
///        QueryServiceConfig(2).

#include "winapi/advapi32/services/scm_internal.hpp"

#include <cstring>

namespace vitreo::winapi {

using namespace scm_detail;
using namespace service_constants;

void register_scm_config_exports(ScmManager& scm, DllExports& exports) {
    // ---- BOOL ChangeServiceConfig{A,W}(SC_HANDLE, type, start, errorControl,
    //          binaryPath, loadOrderGroup, tagId, dependencies, startName,
    //          password, displayName)
    // 11 args, stdcall_bytes=44.
    auto change_config = [&scm](Marshaler& m, bool wide) {
        auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
        if (!name) { m.set_return(0); return; }

        ServiceConfig config;
        config.name          = *name;
        config.service_type  = static_cast<u32>(m.arg(1));
        config.start_type    = static_cast<u32>(m.arg(2));
        config.error_control = static_cast<u32>(m.arg(3));
        if (auto binary = read_guest_string(m, m.arg_ptr(4), wide)) {
            config.image_path = *binary;
        }
        if (auto start_name = read_guest_string(m, m.arg_ptr(8), wide)) {
            config.object_name = *start_name;
        }
        if (auto display = read_guest_string(m, m.arg_ptr(10), wide)) {
            config.display_name = *display;
        }

        u32 error = 0;
        if (!scm.change_config(config, error)) {
            set_last_error(error);
            m.set_return(0);
            return;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"ChangeServiceConfigA", 0,
        [change_config](Marshaler& m) { change_config(m, false); }, 44});
    exports.entries.push_back({"ChangeServiceConfigW", 0,
        [change_config](Marshaler& m) { change_config(m, true); }, 44});

    // ---- BOOL ChangeServiceConfig2{A,W}(SC_HANDLE, DWORD level, LPVOID info)
    // 3 args, stdcall_bytes=12.
    auto change_config2 = [&scm](Marshaler& m, bool wide) {
        auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
        if (!name) { m.set_return(0); return; }

        constexpr u32 SERVICE_CONFIG_DESCRIPTION = 1;
        constexpr u32 SERVICE_CONFIG_FAILURE_ACTIONS = 2;
        constexpr u32 SC_ACTION_RUN_COMMAND = 3;

        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr info = m.arg_ptr(2);
        if (info == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        u32 error = error_codes::ERROR_SUCCESS;
        if (level == SERVICE_CONFIG_DESCRIPTION) {
            auto description_ptr = m.read_ptr(info);
            if (description_ptr.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            std::optional<std::wstring> description;
            if (description_ptr.value() != 0) {
                description = read_guest_string(
                    m, static_cast<GuestAddr>(description_ptr.value()), wide);
                if (!description) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            if (!scm.change_description(*name, description, error)) {
                set_last_error(error);
                m.set_return(0);
                return;
            }
        } else if (level == SERVICE_CONFIG_FAILURE_ACTIONS) {
            const bool is64 = m.cpu_is_64bit();
            const u32 reboot_offset = is64 ? 8u : 4u;
            const u32 command_offset = is64 ? 16u : 8u;
            const u32 count_offset = is64 ? 24u : 12u;
            const u32 actions_offset = is64 ? 32u : 16u;

            auto reset = m.read_u32(info);
            auto reboot_ptr = m.read_ptr(info + reboot_offset);
            auto command_ptr = m.read_ptr(info + command_offset);
            auto count = m.read_u32(info + count_offset);
            auto actions_ptr = m.read_ptr(info + actions_offset);
            if (reset.is_err() || reboot_ptr.is_err() ||
                command_ptr.is_err() || count.is_err() ||
                actions_ptr.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            if (count.value() > 1024) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            ServiceFailureActionsUpdate update;
            update.reset_period_seconds = reset.value();
            if (reboot_ptr.value() != 0) {
                update.reboot_message = read_guest_string(
                    m, static_cast<GuestAddr>(reboot_ptr.value()), wide);
                if (!update.reboot_message) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            if (command_ptr.value() != 0) {
                update.command = read_guest_string(
                    m, static_cast<GuestAddr>(command_ptr.value()), wide);
                if (!update.command) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            if (actions_ptr.value() != 0) {
                std::vector<ServiceFailureAction> actions;
                actions.reserve(count.value());
                for (u32 index = 0; index < count.value(); ++index) {
                    const GuestAddr action_addr =
                        static_cast<GuestAddr>(actions_ptr.value()) +
                        static_cast<GuestAddr>(index) * 8;
                    auto type = m.read_u32(action_addr);
                    auto delay = m.read_u32(action_addr + 4);
                    if (type.is_err() || delay.is_err()) {
                        set_last_error(error_codes::ERROR_NOACCESS);
                        m.set_return(0);
                        return;
                    }
                    if (type.value() > SC_ACTION_RUN_COMMAND) {
                        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                        m.set_return(0);
                        return;
                    }
                    actions.push_back(
                        ServiceFailureAction{type.value(), delay.value()});
                }
                update.actions = std::move(actions);
            }
            if (!scm.change_failure_actions(*name, update, error)) {
                set_last_error(error);
                m.set_return(0);
                return;
            }
        } else {
            set_last_error(error_codes::ERROR_INVALID_LEVEL);
            m.set_return(0);
            return;
        }

        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"ChangeServiceConfig2A", 0,
        [change_config2](Marshaler& m) { change_config2(m, false); }, 12});
    exports.entries.push_back({"ChangeServiceConfig2W", 0,
        [change_config2](Marshaler& m) { change_config2(m, true); }, 12});

    // ---- BOOL QueryServiceConfig{A,W}(SC_HANDLE, LPQUERY_SERVICE_CONFIG,
    //          DWORD cbBufSize, LPDWORD pcbBytesNeeded)
    // 4 args, stdcall_bytes=16.
    //
    // QUERY_SERVICE_CONFIG is three DWORDs followed by six pointers; the
    // strings live in the caller's buffer after the fixed part, which is why
    // the size query matters. Pointer width differs between x86 and x64.
    auto query_config = [&scm](Marshaler& m, bool wide) {
        auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
        if (!name) { m.set_return(0); return; }
        auto config = scm.query_config(*name);
        if (!config) {
            set_last_error(error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
            m.set_return(0);
            return;
        }

        const bool is64 = m.cpu_is_64bit();
        const u32 ptr_size = is64 ? 8u : 4u;
        const u32 char_size = wide ? 2u : 1u;
        // dwServiceType, dwStartType, dwErrorControl, then lpBinaryPathName,
        // lpLoadOrderGroup, dwTagId, lpDependencies, lpServiceStartName,
        // lpDisplayName. dwTagId is a DWORD between the pointers, so on x64 the
        // struct carries padding to keep the following pointer aligned.
        const u32 fixed_size = is64 ? 64u : 36u;

        auto string_bytes = [char_size](const std::wstring& s) -> u32 {
            return static_cast<u32>((s.size() + 1) * char_size);
        };
        const u32 needed = fixed_size + string_bytes(config->image_path) +
                           string_bytes(config->object_name) +
                           string_bytes(config->display_name) +
                           2 * char_size;  // empty load-order group + deps

        const GuestAddr buffer = m.arg_ptr(1);
        const u32 buffer_size = static_cast<u32>(m.arg(2));
        if (const GuestAddr needed_ptr = m.arg_ptr(3); needed_ptr != 0) {
            (void)m.write_u32(needed_ptr, needed);
        }
        if (buffer == 0 || buffer_size < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }

        GuestAddr cursor = buffer + fixed_size;
        auto emit = [&](const std::wstring& value) -> GuestAddr {
            const GuestAddr at = cursor;
            const std::string narrow = to_narrow(value);
            if (wide) {
                (void)m.write_wide_string(at, narrow);
            } else {
                (void)m.write_ansi_string(at, narrow);
            }
            cursor += (value.size() + 1) * char_size;
            return at;
        };

        (void)m.write_u32(buffer + 0, config->service_type);
        (void)m.write_u32(buffer + 4, config->start_type);
        (void)m.write_u32(buffer + 8, config->error_control);

        const GuestAddr binary_at = emit(config->image_path);
        const GuestAddr group_at = emit(std::wstring{});
        const GuestAddr deps_at = emit(std::wstring{});
        const GuestAddr start_at = emit(config->object_name);
        const GuestAddr display_at = emit(config->display_name);

        // LLP64 aligns the first pointer to offset 16. The x86 structure has
        // four-byte alignment and starts its pointer fields at offset 12.
        GuestAddr field = buffer + (is64 ? 16u : 12u);
        auto put_ptr = [&](GuestAddr value) {
            if (is64) {
                (void)m.write_u64(field, value);
            } else {
                (void)m.write_u32(field, static_cast<u32>(value));
            }
            field += ptr_size;
        };
        put_ptr(binary_at);
        put_ptr(group_at);
        (void)m.write_u32(field, 0);            // dwTagId
        field += is64 ? 8u : 4u;                // padded to pointer alignment
        put_ptr(deps_at);
        put_ptr(start_at);
        put_ptr(display_at);

        VITREO_TRACE("[scm] QueryServiceConfig%s '%s' -> image='%s'\n",
                     wide ? "W" : "A", to_narrow(*name).c_str(),
                     to_narrow(config->image_path).c_str());
        m.set_return(1);
    };
    // ---- BOOL QueryServiceConfig2{A,W}(SC_HANDLE, DWORD dwInfoLevel,
    //          LPBYTE lpBuffer, DWORD cbBufSize, LPDWORD pcbBytesNeeded)
    // 5 args, stdcall_bytes=20. Levels, sizes and errors measured on
    // Windows 10 (tools/oracle_probe/service_config2_probe.expected):
    // levels 0, 10 and >12 are ERROR_INVALID_LEVEL with *pcbBytesNeeded
    // untouched; 9 (preferred node) is ERROR_INVALID_PARAMETER with 0; a short
    // buffer is ERROR_INSUFFICIENT_BUFFER with the size.
    auto query_config2 = [&scm](Marshaler& m, bool wide) {
        auto name = resolve_service_arg(scm, static_cast<u32>(m.arg(0)));
        if (!name) { m.set_return(0); return; }
        const u32 level = static_cast<u32>(m.arg(1));
        const GuestAddr buffer = m.arg_ptr(2);
        const u32 buffer_size = m.arg32(3);
        const GuestAddr needed_ptr = m.arg_ptr(4);
        if (level == 0 || level == 10 || level > 12) {
            set_last_error(error_codes::ERROR_INVALID_LEVEL);
            m.set_return(0);
            return;
        }
        if (level == 9) {
            if (needed_ptr != 0) (void)m.write_u32(needed_ptr, 0);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        auto config = scm.query_config2(*name);
        if (!config) {
            set_last_error(error_codes::ERROR_SERVICE_DOES_NOT_EXIST);
            m.set_return(0);
            return;
        }
        const bool is64 = m.cpu_is_64bit();
        const u32 ptr = is64 ? 8u : 4u;
        const u32 char_size = wide ? 2u : 1u;
        // Serialise into a host image with buffer-relative pointers, then
        // rebase them onto the guest buffer.
        std::vector<u8> image;
        std::vector<u32> pointer_fields;
        auto put32 = [&](u32 off, u32 v) { std::memcpy(image.data() + off, &v, 4); };
        auto append_string = [&](const std::wstring& s) -> u32 {
            while (image.size() % char_size) image.push_back(0);
            const u32 at = static_cast<u32>(image.size());
            const std::string narrow = to_narrow(s);
            if (wide) {
                for (wchar_t ch : s) {
                    image.push_back(static_cast<u8>(ch));
                    image.push_back(static_cast<u8>(static_cast<u16>(ch) >> 8));
                }
                image.push_back(0); image.push_back(0);
            } else {
                image.insert(image.end(), narrow.begin(), narrow.end());
                image.push_back(0);
            }
            return at;
        };
        switch (level) {
        case 1: // SERVICE_DESCRIPTION { LPTSTR lpDescription; }
            image.assign(ptr, 0);
            if (config->description) {
                put32(0, append_string(*config->description));
                pointer_fields.push_back(0);
            }
            break;
        case 2: { // SERVICE_FAILURE_ACTIONS
            const u32 reboot_off = ptr, command_off = 2 * ptr,
                      count_off = 3 * ptr, actions_off = is64 ? 32u : 16u;
            image.assign(actions_off + ptr, 0);
            put32(0, config->reset_period);
            put32(count_off, static_cast<u32>(config->actions.size()));
            if (!config->actions.empty()) {
                while (image.size() % 4) image.push_back(0);
                put32(actions_off, static_cast<u32>(image.size()));
                pointer_fields.push_back(actions_off);
                for (const auto& [type, delay] : config->actions) {
                    for (u32 v : {type, delay}) {
                        for (int i = 0; i < 4; ++i) image.push_back(static_cast<u8>(v >> (8 * i)));
                    }
                }
            }
            if (!config->reboot_message.empty()) {
                put32(reboot_off, append_string(config->reboot_message));
                pointer_fields.push_back(reboot_off);
            }
            if (!config->command.empty()) {
                put32(command_off, append_string(config->command));
                pointer_fields.push_back(command_off);
            }
            break;
        }
        case 3: image.assign(4, 0); put32(0, config->delayed_auto_start); break;
        case 4: image.assign(4, 0); put32(0, config->failure_actions_flag); break;
        case 5: image.assign(4, 0); put32(0, config->service_sid_type); break;
        case 6: // SERVICE_REQUIRED_PRIVILEGES_INFO { LPTSTR pmszRequiredPrivileges; }
            image.assign(ptr, 0);
            if (!config->required_privileges.empty()) {
                while (image.size() % char_size) image.push_back(0);
                put32(0, static_cast<u32>(image.size()));
                pointer_fields.push_back(0);
                for (const auto& p : config->required_privileges) (void)append_string(p);
                for (u32 i = 0; i < char_size; ++i) image.push_back(0);
            }
            break;
        case 7: image.assign(4, 0); put32(0, config->preshutdown_timeout); break;
        case 8: // SERVICE_TRIGGER_INFO { DWORD cTriggers; PSERVICE_TRIGGER; PBYTE }
            image.assign(is64 ? 24u : 12u, 0); // no triggers are registered
            break;
        case 11: image.assign(1, 0); break; // undocumented level: one byte, 0
        case 12: image.assign(4, 0); put32(0, config->launch_protected); break;
        default: break;
        }
        const u32 needed = static_cast<u32>(image.size());
        if (needed_ptr != 0) (void)m.write_u32(needed_ptr, needed);
        if (buffer == 0 || buffer_size < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        for (u32 field : pointer_fields) {
            u32 rel = 0;
            std::memcpy(&rel, image.data() + field, 4);
            const u64 absolute = buffer + rel;
            std::memcpy(image.data() + field, &absolute, ptr);
        }
        if (m.memory().write_bytes(buffer, image.data(), image.size()).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    };
    exports.entries.push_back({"QueryServiceConfig2A", 0,
        [query_config2](Marshaler& m) { query_config2(m, false); }, 20});
    exports.entries.push_back({"QueryServiceConfig2W", 0,
        [query_config2](Marshaler& m) { query_config2(m, true); }, 20});
    exports.entries.push_back({"QueryServiceConfigA", 0,
        [query_config](Marshaler& m) { query_config(m, false); }, 16});
    exports.entries.push_back({"QueryServiceConfigW", 0,
        [query_config](Marshaler& m) { query_config(m, true); }, 16});
}

} // namespace vitreo::winapi
