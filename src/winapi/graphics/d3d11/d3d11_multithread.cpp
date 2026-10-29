// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_multithread.cpp
/// @brief ID3D11Multithread and ID3D10Multithread.
///
/// Enter / Leave on the device's API lock with the per-thread Enter
/// depth, and SetMultithreadProtected / GetMultithreadProtected.
///
/// Split out of d3d11_device_ext.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// ID3D11Multithread / ID3D10Multithread
// -----------------------------------------------------------------------------

namespace {

/// Enter calls this thread holds without a matching Leave.
int& multithread_enter_depth() {
    thread_local int depth = 0;
    return depth;
}

}  // namespace

void D3D11Manager::impl_MultithreadMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    InterfaceType type{};
    Device* device = find_device(self, &type);
    if (device == nullptr || (type != InterfaceType::DeviceMultithread &&
                              type != InterfaceType::ContextMultithread)) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        // The device's facet QueryInterfaces as the device, the context's as
        // the immediate context, and each counts with its owner (measured).
        m.set_return(query_interface(m, self));
        return;
    case 1:
        m.set_return(add_ref(self));
        return;
    case 2:
        m.set_return(release(self));
        return;
    case 3:
        // Enter: "Enter a device's critical section" (Learn). That is the one
        // lock every call into this device already takes (m_api_mutex), so
        // the measured behaviour follows: while one thread is inside Enter,
        // another thread's CreateBuffer and IAGetVertexBuffers wait (a12
        // refmodel [mt]). The entry thunk holds the recursive lock for this
        // call; taking it once more keeps it held after the call returns,
        // until the matching Leave. It is counted in api_lock_depth so a
        // wait that must give the device lock up (a keyed-mutex
        // AcquireSync) gives up Enter's hold too, and takes it back after.
        //
        // Deliberate difference: with protection off Windows lets another
        // thread's immediate-context calls through during Enter (measured
        // ClearState "ran"); here every call serialises on the one lock, as
        // the device always does. Nothing that is correct on Windows
        // depends on that call running concurrently.
        m_api_mutex.lock();
        ++api_lock_depth();
        ++multithread_enter_depth();
        m.set_return(0);
        return;
    case 4:
        // Leave: a Leave without an Enter on this thread is the
        // application's error (Learn: undefined); it must not release a lock
        // another call of this thread is relying on.
        if (multithread_enter_depth() > 0) {
            --multithread_enter_depth();
            --api_lock_depth();
            m_api_mutex.unlock();
        }
        m.set_return(0);
        return;
    case 5: {
        // SetMultithreadProtected(BOOL): returns the previous value, and any
        // non-zero BOOL is TRUE (measured: Set(5) then Get reads 1). One
        // flag per device, whichever facet sets it (measured).
        const bool previous = device->multithread_protected;
        device->multithread_protected = static_cast<u32>(m.arg(1)) != 0;
        m.set_return(previous ? 1u : 0u);
        return;
    }
    case 6:
        m.set_return(device->multithread_protected ? 1u : 0u);
        return;
    default:
        unsupported(m, "ID3D11Multithread", slot);
        return;
    }
}

}  // namespace vitreo::winapi
