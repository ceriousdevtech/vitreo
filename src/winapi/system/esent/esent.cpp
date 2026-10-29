/// @file esent.cpp
/// @brief Honest ESENT.dll JET_API surface: instance lifecycle without a fake engine.

#include "esent.hpp"

#include "winapi/core/marshaler.hpp"

#include <memory>

namespace vitreo::winapi {
namespace {

void set_jet_err(Marshaler& m, i32 err) {
    m.set_return(static_cast<u32>(static_cast<i32>(err)));
}

std::optional<std::string> read_optional_wide(Marshaler& m, GuestAddr addr) {
    if (addr == 0) return std::nullopt;
    auto text = m.read_wide_string(addr);
    if (text.is_err()) return std::nullopt;
    return text.value();
}

}  // namespace

bool EsentManager::is_string_param(u32 paramid) {
    switch (paramid) {
        case jet_param::SystemPath:
        case jet_param::TempPath:
        case jet_param::LogFilePath:
        case jet_param::BaseName:
        case jet_param::EventSource:
        case jet_param::Recovery:
            return true;
        default:
            return false;
    }
}

bool EsentManager::is_integer_param(u32 paramid) {
    switch (paramid) {
        case jet_param::MaxSessions:
        case jet_param::MaxOpenTables:
        case jet_param::MaxCursors:
        case jet_param::MaxVerPages:
        case jet_param::MaxTemporaryTables:
        case jet_param::LogFileSize:
        case jet_param::CircularLog:
        case jet_param::CacheSizeMax:
        case jet_param::StopFlushThreshold:
        case jet_param::DatabasePageSize:
        case jet_param::CreatePathIfNotExist:
        case jet_param::MaxInstances:
            return true;
        default:
            return false;
    }
}

u32 EsentManager::max_instances() const {
    auto it = global_int_params_.find(jet_param::MaxInstances);
    if (it != global_int_params_.end()) {
        return static_cast<u32>(it->second);
    }
    return 16;  // Learn default for JET_paramMaxInstances
}

i32 EsentManager::create_instance(u64* instance, std::string name) {
    if (instance == nullptr) return jet_err::InvalidParameter;
    if (instances_.size() >= max_instances()) {
        return jet_err::TooManyInstances;
    }
    if (instance_names_.count(name) != 0) {
        return jet_err::InstanceNameInUse;
    }
    const u64 handle = next_handle_++;
    Instance created;
    created.name = name;
    instances_.emplace(handle, std::move(created));
    instance_names_.insert(std::move(name));
    *instance = handle;
    return jet_err::Success;
}

i32 EsentManager::set_system_parameter(
    std::optional<u64> instance, u64 sesid, u32 paramid, u64 lparam,
    std::optional<std::string> sz) {
    if (sesid != 0) return jet_err::InvalidSesid;
    if (!is_string_param(paramid) && !is_integer_param(paramid)) {
        return jet_err::InvalidParameter;
    }
    if (paramid == jet_param::MaxInstances) {
        if (instance.has_value()) return jet_err::InvalidParameter;
        if (lparam < 1 || lparam > 1024) return jet_err::InvalidParameter;
    }
    Instance* target = nullptr;
    if (instance.has_value()) {
        auto it = instances_.find(*instance);
        if (it == instances_.end()) return jet_err::InvalidInstance;
        target = &it->second;
    }
    if (is_string_param(paramid)) {
        const std::string value = sz.value_or(std::string{});
        if (target != nullptr) {
            target->string_params[paramid] = value;
        } else {
            global_string_params_[paramid] = value;
        }
        return jet_err::Success;
    }
    if (target != nullptr) {
        target->int_params[paramid] = lparam;
    } else {
        global_int_params_[paramid] = lparam;
    }
    return jet_err::Success;
}

i32 EsentManager::init(std::optional<u64> instance) {
    if (instance.has_value()) {
        if (instances_.find(*instance) == instances_.end()) {
            return jet_err::InvalidInstance;
        }
    }
    // Learn: JetInit performs crash recovery and creates checkpoint/log files.
    // Vitreo has no ESE recovery engine; claiming success would be fake.
    return jet_err::FeatureNotAvailable;
}

i32 EsentManager::term(u64 instance) {
    auto it = instances_.find(instance);
    if (it == instances_.end()) return jet_err::InvalidParameter;
    instance_names_.erase(it->second.name);
    instances_.erase(it);
    return jet_err::Success;
}

i32 EsentManager::begin_session(u64 instance) {
    if (instances_.find(instance) == instances_.end()) {
        return jet_err::InvalidInstance;
    }
    return jet_err::NotInitialized;
}

i32 EsentManager::session_api(u64 sesid) const {
    (void)sesid;
    return jet_err::NotInitialized;
}

std::optional<u64> EsentManager::global_int_param(u32 paramid) const {
    auto it = global_int_params_.find(paramid);
    if (it == global_int_params_.end()) return std::nullopt;
    return it->second;
}

DllExports create_esent_thunks(EsentManager& manager) {
    DllExports exports;
    auto* mgr = &manager;

    exports.entries.push_back(
        {"JetCreateInstanceW", 0,
         [mgr](Marshaler& m) {
             const GuestAddr pinstance = m.arg_ptr(0);
             if (pinstance == 0) {
                 set_jet_err(m, jet_err::InvalidParameter);
                 return;
             }
             const auto name = read_optional_wide(m, m.arg_ptr(1));
             u64 handle = 0;
             const i32 err = mgr->create_instance(
                 &handle, name.value_or(std::string{}));
             if (err == jet_err::Success) {
                 if (m.write_ptr(pinstance, handle).is_err()) {
                     (void)mgr->term(handle);
                     set_jet_err(m, jet_err::InvalidParameter);
                     return;
                 }
             }
             set_jet_err(m, err);
         },
         8});

    exports.entries.push_back(
        {"JetSetSystemParameterW", 0,
         [mgr](Marshaler& m) {
             const GuestAddr pinstance = m.arg_ptr(0);
             const u64 sesid = m.arg(1);
             const u32 paramid = static_cast<u32>(m.arg(2));
             const u64 lparam = m.arg(3);
             auto sz = read_optional_wide(m, m.arg_ptr(4));
             std::optional<u64> instance;
             if (pinstance != 0) {
                 auto handle = m.read_ptr(pinstance);
                 if (handle.is_err()) {
                     set_jet_err(m, jet_err::InvalidParameter);
                     return;
                 }
                 if (handle.value() != 0) instance = handle.value();
             }
             set_jet_err(
                 m, mgr->set_system_parameter(
                        instance, sesid, paramid, lparam, std::move(sz)));
         },
         20});

    exports.entries.push_back(
        {"JetInit", 0,
         [mgr](Marshaler& m) {
             const GuestAddr pinstance = m.arg_ptr(0);
             std::optional<u64> instance;
             if (pinstance != 0) {
                 auto handle = m.read_ptr(pinstance);
                 if (handle.is_err()) {
                     set_jet_err(m, jet_err::InvalidParameter);
                     return;
                 }
                 if (handle.value() != 0) instance = handle.value();
             }
             set_jet_err(m, mgr->init(instance));
         },
         4});

    exports.entries.push_back(
        {"JetTerm", 0,
         [mgr](Marshaler& m) { set_jet_err(m, mgr->term(m.arg(0))); }, 4});

    exports.entries.push_back(
        {"JetBeginSessionW", 0,
         [mgr](Marshaler& m) {
             const u64 instance = m.arg(0);
             const GuestAddr psesid = m.arg_ptr(1);
             if (psesid == 0) {
                 set_jet_err(m, jet_err::InvalidParameter);
                 return;
             }
             set_jet_err(m, mgr->begin_session(instance));
         },
         16});

    auto session_fail = [mgr](Marshaler& m) {
        set_jet_err(m, mgr->session_api(m.arg(0)));
    };

    exports.entries.push_back(
        {"JetAttachDatabase2W", 0, session_fail, 16});
    exports.entries.push_back(
        {"JetOpenDatabaseW", 0, session_fail, 20});
    exports.entries.push_back(
        {"JetOpenTableW", 0, session_fail, 28});
    exports.entries.push_back(
        {"JetGetTableColumnInfoW", 0, session_fail, 24});
    exports.entries.push_back(
        {"JetRetrieveColumn", 0, session_fail, 32});
    exports.entries.push_back(
        {"JetMove", 0, session_fail, 16});
    exports.entries.push_back(
        {"JetCloseTable", 0, session_fail, 8});
    return exports;
}

DllExports create_esent_thunks() {
    static auto manager = std::make_shared<EsentManager>();
    return create_esent_thunks(*manager);
}

}  // namespace vitreo::winapi
