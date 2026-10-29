// SPDX-License-Identifier: LGPL-2.1-only
/// @file combase_async.cpp
/// @brief Completed IAsyncOperation / IAsyncInfo shared by Bluetooth, Launcher and KeyCred

#include "winapi/com/combase/combase.hpp"
#include "winapi/com/combase/combase_internal.hpp"

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// Completed IAsyncOperation / IAsyncInfo.
//
// Every operation here resolves before it is handed out, so the returned
// IAsyncOperation is already Completed. WinRT's contract for put_Completed on
// a finished operation is to invoke the handler immediately on the calling
// thread, which is what the guest-call bridge below does.
// ---------------------------------------------------------------------------

GuestAddr CombaseManager::create_completed_async(GuestAddr result,
                                                 bool is_64bit) {
    Object operation{};
    operation.type = ObjectType::AsyncOperation;
    operation.ref_count = 1;
    operation.async_result = result;
    operation.async_status = 1;  // AsyncStatus::Completed
    const GuestAddr op = allocate_object(std::move(operation), is_64bit);
    if (op == 0) return 0;

    Object info{};
    info.type = ObjectType::AsyncInfo;
    info.ref_count = 1;
    info.async_result = result;
    info.async_status = 1;
    info.async_peer = op;
    const GuestAddr info_addr = allocate_object(std::move(info), is_64bit);
    if (info_addr == 0) return 0;
    m_objects.at(op).async_peer = info_addr;
    return op;
}

CombaseManager::Object* CombaseManager::async_arg(Marshaler& m,
                                                  ObjectType type) {
    auto it = m_objects.find(m.arg_ptr(0));
    if (it == m_objects.end() || it->second.type != type) return nullptr;
    return &it->second;
}

GuestAddr CombaseManager::create_completed_async_scalar(u64 value, u32 width,
                                                        bool is_64bit) {
    const GuestAddr op = create_completed_async(0, is_64bit);
    if (op == 0) return 0;
    auto it = m_objects.find(op);
    if (it != m_objects.end()) {
        it->second.async_scalar = value;
        it->second.async_scalar_size = width;
    }
    return op;
}

void CombaseManager::impl_AsyncPutCompleted(Marshaler& m) {
    Object* operation = async_arg(m, ObjectType::AsyncOperation);
    if (operation == nullptr) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr handler = m.arg_ptr(1);
    const GuestAddr self = m.arg_ptr(0);
    const u32 status = operation->async_status;
    if (handler == 0) {
        m.set_return(S_OK);
        return;
    }
    if (!m_guest_call) {
        VITREO_TRACE(
            "[combase] IAsyncOperation.put_Completed cannot invoke the "
            "delegate: no guest-call bridge installed\n");
        m.set_return(E_NOTIMPL);
        return;
    }
    // The operation already finished, so Windows runs the handler inline
    // rather than parking it. IAsyncOperationCompletedHandler::Invoke is
    // vtable slot 3 (IUnknown's three, then the single method).
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr pointer_size = is_64bit ? 8u : 4u;
    const auto vtable = is_64bit ? m_memory.read_u64(handler)
                                 : m_memory.read_u32(handler).map(
                                       [](u32 v) { return static_cast<u64>(v); });
    if (vtable.is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    const GuestAddr invoke_slot =
        static_cast<GuestAddr>(vtable.value()) + 3 * pointer_size;
    const auto invoke = is_64bit
        ? m_memory.read_u64(invoke_slot)
        : m_memory.read_u32(invoke_slot).map(
              [](u32 v) { return static_cast<u64>(v); });
    if (invoke.is_err() || invoke.value() == 0) {
        m.set_return(E_POINTER);
        return;
    }
    const u64 args[3] = {handler, self, status};
    (void)m_guest_call(static_cast<GuestAddr>(invoke.value()), args, 3,
                       is_64bit);
    m.set_return(S_OK);
}

void CombaseManager::impl_AsyncGetCompleted(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    // The handler is invoked and dropped immediately, so none is retained.
    m.set_return(m.write_ptr(out, 0).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_AsyncGetResults(Marshaler& m) {
    Object* operation = async_arg(m, ObjectType::AsyncOperation);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    if (operation == nullptr) {
        (void)m.write_ptr(out, 0);
        m.set_return(E_INVALIDARG);
        return;
    }
    if (operation->async_scalar_size != 0) {
        // IAsyncOperation<T> for a by-value T writes the value itself, not an
        // interface pointer.
        bool ok = false;
        switch (operation->async_scalar_size) {
        case 1: ok = m.write_u8(out, static_cast<u8>(operation->async_scalar)).is_ok(); break;
        case 2: ok = m.write_u16(out, static_cast<u16>(operation->async_scalar)).is_ok(); break;
        case 8: ok = m.write_u64(out, operation->async_scalar).is_ok(); break;
        default: ok = m.write_u32(out, static_cast<u32>(operation->async_scalar)).is_ok(); break;
        }
        m.set_return(ok ? S_OK : E_POINTER);
        return;
    }
    if (operation->async_result != 0) {
        auto it = m_objects.find(operation->async_result);
        if (it != m_objects.end()) ++it->second.ref_count;
    }
    m.set_return(m.write_ptr(out, operation->async_result).is_ok() ? S_OK
                                                                   : E_POINTER);
}

void CombaseManager::impl_AsyncInfoGetId(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(m.write_u32(out, 1u).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_AsyncInfoGetStatus(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    auto it = m_objects.find(m.arg_ptr(0));
    const u32 status =
        it == m_objects.end() ? 1u : it->second.async_status;
    m.set_return(m.write_u32(out, status).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_AsyncInfoGetErrorCode(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(m.write_u32(out, 0u).is_ok() ? S_OK : E_POINTER);
}

void CombaseManager::impl_AsyncInfoCancel(Marshaler& m) {
    // Completed operations ignore Cancel, exactly as Windows does.
    (void)m.arg_ptr(0);
    m.set_return(S_OK);
}

void CombaseManager::impl_AsyncInfoClose(Marshaler& m) {
    (void)m.arg_ptr(0);
    m.set_return(S_OK);
}

} // namespace vitreo::winapi
