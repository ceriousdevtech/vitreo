/// @file amsi.cpp
/// @brief Implementation of the Antimalware Scan Interface (amsi.dll).

#include "amsi.hpp"

#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

namespace {

constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEInvalidArg = 0x80070057u;

}  // namespace

u64 AmsiManager::create_context() {
    std::lock_guard<std::mutex> lock(mutex_);
    const u64 handle = next_context_;
    next_context_ += 4;
    contexts_.insert(handle);
    return handle;
}

bool AmsiManager::context_valid(u64 context) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return contexts_.find(context) != contexts_.end();
}

void AmsiManager::destroy_context(u64 context) {
    std::lock_guard<std::mutex> lock(mutex_);
    contexts_.erase(context);
}

u64 AmsiManager::create_session() {
    std::lock_guard<std::mutex> lock(mutex_);
    const u64 handle = next_session_;
    next_session_ += 4;
    sessions_.insert(handle);
    return handle;
}

bool AmsiManager::session_valid(u64 session) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.find(session) != sessions_.end();
}

void AmsiManager::destroy_session(u64 session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(session);
}

DllExports create_amsi_thunks(AmsiManager& manager) {
    // Every scan entry point ends the same way: no provider is registered,
    // so nothing is detected. Sharing one writer keeps that single fact in
    // one place rather than repeating a magic constant per export.
    const auto report_not_detected = [](Marshaler& m, GuestAddr result_addr) {
        if (result_addr == 0) {
            m.set_return(kEInvalidArg);
            return;
        }
        if (m.write_u32(result_addr, amsi_result::kNotDetected).is_err()) {
            m.set_return(kEInvalidArg);
            return;
        }
        m.set_return(kSOk);
    };

    DllExports exports;
    exports.entries.reserve(7);

    // HRESULT AmsiInitialize(LPCWSTR appName, HAMSICONTEXT *amsiContext)
    exports.entries.push_back({"AmsiInitialize", 1,
        [&manager](Marshaler& m) {
            const GuestAddr app_name = m.arg_ptr(0);
            const GuestAddr out_context = m.arg_ptr(1);
            if (app_name == 0 || out_context == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            const u64 context = manager.create_context();
            if (m.write_ptr(out_context, context).is_err()) {
                manager.destroy_context(context);
                m.set_return(kEInvalidArg);
                return;
            }
            const auto name = m.read_wide_string(app_name);
            VITREO_TRACE("[amsi] AmsiInitialize app='%s' context=0x%llx\n",
                         name.is_ok() ? name.value().c_str() : "?",
                         static_cast<unsigned long long>(context));
            m.set_return(kSOk);
        }, 8});

    // void AmsiUninitialize(HAMSICONTEXT amsiContext)
    exports.entries.push_back({"AmsiUninitialize", 2,
        [&manager](Marshaler& m) {
            manager.destroy_context(m.arg(0));
            m.set_return(0);
        }, 4});

    // HRESULT AmsiOpenSession(HAMSICONTEXT amsiContext, HAMSISESSION *session)
    exports.entries.push_back({"AmsiOpenSession", 3,
        [&manager](Marshaler& m) {
            const GuestAddr out_session = m.arg_ptr(1);
            if (!manager.context_valid(m.arg(0)) || out_session == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            const u64 session = manager.create_session();
            if (m.write_ptr(out_session, session).is_err()) {
                manager.destroy_session(session);
                m.set_return(kEInvalidArg);
                return;
            }
            m.set_return(kSOk);
        }, 8});

    // void AmsiCloseSession(HAMSICONTEXT amsiContext, HAMSISESSION session)
    exports.entries.push_back({"AmsiCloseSession", 4,
        [&manager](Marshaler& m) {
            manager.destroy_session(m.arg(1));
            m.set_return(0);
        }, 8});

    // HRESULT AmsiScanBuffer(HAMSICONTEXT, PVOID buffer, ULONG length,
    //                        LPCWSTR contentName, HAMSISESSION, AMSI_RESULT*)
    // Windows rejects an empty buffer outright, before any provider runs.
    exports.entries.push_back({"AmsiScanBuffer", 5,
        [&manager, report_not_detected](Marshaler& m) {
            const GuestAddr buffer = m.arg_ptr(1);
            const u32 length = static_cast<u32>(m.arg(2));
            if (!manager.context_valid(m.arg(0)) || buffer == 0 ||
                length == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            report_not_detected(m, m.arg_ptr(5));
        }, 24});

    // HRESULT AmsiScanString(HAMSICONTEXT, LPCWSTR string, LPCWSTR contentName,
    //                        HAMSISESSION, AMSI_RESULT*)
    exports.entries.push_back({"AmsiScanString", 6,
        [&manager, report_not_detected](Marshaler& m) {
            if (!manager.context_valid(m.arg(0)) || m.arg_ptr(1) == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            report_not_detected(m, m.arg_ptr(4));
        }, 20});

    // HRESULT AmsiNotifyOperation(HAMSICONTEXT, PVOID buffer, ULONG length,
    //                             LPCWSTR contentName, AMSI_RESULT*)
    // Unlike the scan calls this one takes no session.
    exports.entries.push_back({"AmsiNotifyOperation", 7,
        [&manager, report_not_detected](Marshaler& m) {
            const GuestAddr buffer = m.arg_ptr(1);
            const u32 length = static_cast<u32>(m.arg(2));
            if (!manager.context_valid(m.arg(0)) || buffer == 0 ||
                length == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            report_not_detected(m, m.arg_ptr(4));
        }, 20});

    return exports;
}

}  // namespace vitreo::winapi
