/// @file bluetooth_host_iobluetooth.mm
/// @brief macOS host radio backend for the Bluetooth contract.
///
/// Deliberately avoids CoreBluetooth. macOS TCC does not fail a Bluetooth
/// call made without user consent — it *terminates the process*, from a
/// dispatch worker, uncatchably (__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__).
/// +[IOBluetoothHostController defaultController] routes through
/// CoreBluetooth and trips exactly that, so a guest merely enumerating
/// Bluetooth would take the emulator down with it.
///
/// The IORegistry answers the questions a guest actually asks — is there a
/// controller, is it powered — without any privacy gate. The BD_ADDR is not
/// reachable that way, so it is reported as unknown rather than invented.

#include "hardware/contracts/bluetooth_host.hpp"

#include <cstdio>

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>

namespace vitreo::hardware {
namespace {

/// Radio power lives in com.apple.Bluetooth's preferences. Reading the
/// preference domain is plain file I/O — unlike every IOBluetooth entry point
/// that reports power, which goes through the CoreBluetooth coordinator and
/// therefore through TCC.
bool controller_powered(bool& known) {
    known = false;
    CFTypeRef value = CFPreferencesCopyValue(
        CFSTR("ControllerPowerState"), CFSTR("com.apple.Bluetooth"),
        kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (value == nullptr) return false;
    bool powered = false;
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        int state = 0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType,
                             &state)) {
            powered = state != 0;
            known = true;
        }
    } else if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        powered = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
        known = true;
    }
    CFRelease(value);
    return powered;
}

/// The kernel publishes IOBluetoothHCIController once a radio is attached.
/// "BluetoothTransportConnected" distinguishes a live controller from a
/// stale match.
bool controller_present(bool& transport_connected) {
    transport_connected = false;
    const io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOBluetoothHCIController"));
    if (service == IO_OBJECT_NULL) return false;

    CFTypeRef connected = IORegistryEntryCreateCFProperty(
        service, CFSTR("BluetoothTransportConnected"), kCFAllocatorDefault, 0);
    if (connected != nullptr) {
        if (CFGetTypeID(connected) == CFBooleanGetTypeID()) {
            transport_connected =
                CFBooleanGetValue(static_cast<CFBooleanRef>(connected));
        }
        CFRelease(connected);
    }
    IOObjectRelease(service);
    return true;
}

class IOKitBluetoothHost final : public BluetoothHost {
public:
    [[nodiscard]] bool query_default_adapter(
        BluetoothAdapterState& out) override {
        @autoreleasepool {
            out = BluetoothAdapterState{};
            bool transport_connected = false;
            if (!controller_present(transport_connected)) {
                // No radio on this Mac. Windows reports the same by
                // completing GetDefaultAsync with a null adapter.
                return true;
            }
            out.present = true;
            bool power_known = false;
            const bool powered = controller_powered(power_known);
            // With no readable preference the transport being up is the
            // strongest ungated statement available.
            out.powered = power_known ? powered : transport_connected;
            // Apple has shipped no Classic-only or LE-only controller.
            out.classic_supported = true;
            out.low_energy_supported = true;
            // BluetoothAddress needs CoreBluetooth, which cannot be touched
            // without risking the TCC process kill. Report "unknown" rather
            // than a fabricated address.
            out.address = 0;
            out.device_id = "\\\\?\\BTHENUM#Vitreo#HostController";
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[bluetooth] host controller present (transport=%d, "
                    "powered=%d, power_known=%d); BD_ADDR withheld — reading "
                    "it needs CoreBluetooth, whose TCC denial kills the "
                    "process\n",
                    transport_connected ? 1 : 0, out.powered ? 1 : 0,
                    power_known ? 1 : 0);
            }
            return true;
        }
    }
};

} // namespace

std::unique_ptr<BluetoothHost> make_platform_bluetooth_host() {
    return std::make_unique<IOKitBluetoothHost>();
}

} // namespace vitreo::hardware
