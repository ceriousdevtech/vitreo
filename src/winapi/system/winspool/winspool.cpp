#include "winspool.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include <cstdio>

namespace vitreo::winapi {

void WinspoolManager::impl_ClosePrinter(Marshaler& m) {
    // The capsule has no printers, so OpenPrinter never hands out a handle
    // and every handle here is invalid: FALSE with ERROR_INVALID_HANDLE
    // (Learn ClosePrinter), not a success for a printer that never existed.
    (void)m.arg(0); // hPrinter
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0);
}

void WinspoolManager::impl_OpenPrinterA(Marshaler& m) {
    impl_OpenPrinterW(m);
}

void WinspoolManager::impl_OpenPrinterW(Marshaler& m) {
    // BOOL OpenPrinterW(LPWSTR pPrinterName, LPHANDLE phPrinter,
    //                   LPPRINTER_DEFAULTS pDefault)
    // Capsule has no printers: fail honestly, but honor out-param contracts.
    (void)m.arg(0); // pPrinterName
    const GuestAddr ph_printer = m.arg_ptr(1);
    (void)m.arg(2); // pDefault
    if (ph_printer == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    (void)m.write_ptr(ph_printer, 0);
    VITREO_TRACE("[winspool] OpenPrinterW -> FALSE (no printers)\n");
    set_last_error(error_codes::ERROR_INVALID_PRINTER_NAME);
    m.set_return(0); // FALSE
}

void WinspoolManager::impl_DocumentPropertiesA(Marshaler& m) {
    impl_DocumentPropertiesW(m);
}

void WinspoolManager::impl_DocumentPropertiesW(Marshaler& m) {
    for (int i = 0; i < 6; i++) (void)m.arg(i);
    VITREO_TRACE("[winspool] DocumentPropertiesW -> -1 (error)\n");
    m.set_return(static_cast<u64>(static_cast<u32>(-1)));
}

DllExports create_winspool_thunks(WinspoolManager& mgr) {
    DllExports exports;
    exports.entries.reserve(9);

    exports.entries.push_back(ExportEntry{
        "ClosePrinter", 0,
        [&mgr](Marshaler& m) { mgr.impl_ClosePrinter(m); },
        4
    });

    exports.entries.push_back(ExportEntry{
        "OpenPrinterA", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenPrinterA(m); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "OpenPrinterW", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenPrinterW(m); },
        12
    });

    exports.entries.push_back(ExportEntry{
        "DocumentPropertiesA", 0,
        [&mgr](Marshaler& m) { mgr.impl_DocumentPropertiesA(m); },
        24
    });

    exports.entries.push_back(ExportEntry{
        "DocumentPropertiesW", 0,
        [&mgr](Marshaler& m) { mgr.impl_DocumentPropertiesW(m); },
        24
    });

    // ------------------------------------------------------------------------
    // Wave 6: no-printers stubs
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "EnumPrintersW", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 5; ++i) (void)m.arg(i);
            GuestAddr needed = m.arg_ptr(5);
            GuestAddr returned = m.arg_ptr(6);
            if (needed) (void)m.write_u32(needed, 0);
            if (returned) (void)m.write_u32(returned, 0);
            m.set_return(1);
        }, 28 });

    // Learn: EnumPorts enumerates print ports; may succeed with zero ports.
    // Empty success matches EnumPrintersW — pcbNeeded/pcReturned = 0, TRUE.
    auto enum_ports = [](Marshaler& m) {
        (void)m.arg(0); // pName (NULL = local)
        (void)m.arg(1); // Level (1 or 2)
        (void)m.arg(2); // pPorts
        (void)m.arg(3); // cbBuf
        GuestAddr needed = m.arg_ptr(4);
        GuestAddr returned = m.arg_ptr(5);
        if (needed) (void)m.write_u32(needed, 0);
        if (returned) (void)m.write_u32(returned, 0);
        m.set_return(1); // TRUE
    };
    exports.entries.push_back(ExportEntry{"EnumPortsA", 0, enum_ports, 24});
    exports.entries.push_back(ExportEntry{"EnumPortsW", 0, enum_ports, 24});
    exports.entries.push_back(ExportEntry{ "DeviceCapabilitiesW", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 5; ++i) (void)m.arg(i);
            m.set_return(static_cast<u64>(static_cast<u32>(-1)));
        }, 20 });
    exports.entries.push_back(ExportEntry{ "GetPrinterDriverW", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 5; ++i) (void)m.arg(i);
            GuestAddr needed = m.arg_ptr(5);
            if (needed) (void)m.write_u32(needed, 0);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 24 });
    exports.entries.push_back(ExportEntry{ "GetPrinterW", 0,
        [](Marshaler& m){
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            GuestAddr needed = m.arg_ptr(4);
            if (needed) (void)m.write_u32(needed, 0);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
        }, 20 });
    exports.entries.push_back(ExportEntry{ "GetDefaultPrinterW", 203,
        [](Marshaler& m){
            (void)m.arg_ptr(0);
            GuestAddr sz = m.arg_ptr(1);
            if (sz) (void)m.write_u32(sz, 1);
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            m.set_return(0);
        }, 8 });

    return exports;
}

} // namespace vitreo::winapi
