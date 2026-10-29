#pragma once
/// @file winspool.hpp
/// @brief WINSPOOL.DRV API stubs (printer services)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;

class WinspoolManager {
public:
    WinspoolManager() = default;

    void impl_ClosePrinter(Marshaler& m);
    void impl_OpenPrinterA(Marshaler& m);
    void impl_OpenPrinterW(Marshaler& m);
    void impl_DocumentPropertiesA(Marshaler& m);
    void impl_DocumentPropertiesW(Marshaler& m);
};

DllExports create_winspool_thunks(WinspoolManager& mgr);

} // namespace vitreo::winapi
