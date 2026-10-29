// SPDX-License-Identifier: LGPL-2.1-only

/// @file registry.cpp
/// @brief Capsule registry implementation (REG-001)

#include "capsule/registry/registry.hpp"
#include "core/pe/os_version.hpp"
#include "capsule/registry/font_substitutes.hpp"

#include <codecvt>
#include <cwctype>
#include <cstdio>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

/// @file init_defaults.cpp
/// @brief init_defaults
///
/// Closed cut from registry.cpp — verified with plan_extraction.py.

namespace vitreo::capsule {

// =============================================================================
// init_defaults
// =============================================================================

void Registry::init_defaults() {
    using namespace std::string_literals;

    // Pre-populate HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
    auto parts = split_reg_path(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion");
    RegNode* cv = walk_path(m_hklm.get(), parts, /*create=*/true);
    if (cv) {
        cv->values[L"ProductName"] =
            RegValue{RegType::Sz, {L"Windows 10 Enterprise"s}};
        const std::wstring build = std::to_wstring(pe::kOsBuildNumber);
        cv->values[L"CurrentBuild"] =
            RegValue{RegType::Sz, {build}};
        // Windows publishes both CurrentBuild and CurrentBuildNumber
        // (same string). Version probes query the latter.
        cv->values[L"CurrentBuildNumber"] =
            RegValue{RegType::Sz, {build}};
        cv->values[L"CurrentVersion"] =
            RegValue{RegType::Sz, {L"6.3"s}};
        // Empty service-pack string, so RegQueryValueEx succeeds. A missing
        // CSDVersion is ERROR_FILE_NOT_FOUND, and an installer property bag
        // treats that as a hard failure.
        cv->values[L"CSDVersion"] =
            RegValue{RegType::Sz, {L""s}};
        cv->values[L"CurrentMajorVersionNumber"] =
            RegValue{RegType::Dword, {u32{10}}};
        cv->values[L"CurrentMinorVersionNumber"] =
            RegValue{RegType::Dword, {u32{0}}};
    }

    // The logical font aliases (Learn, "Using MS Shell Dlg and MS Shell Dlg
    // 2": the mappings are stored in this key). GDI, DirectWrite, and
    // applications that perform their own fallback discovery open it even
    // on a fresh installation. The values are Vitreo's own documented alias
    // set (font_substitutes.hpp), the same one GDI's face mapper applies.
    {
        auto font_parts = split_reg_path(
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes");
        RegNode* fonts = walk_path(m_hklm.get(), font_parts, /*create=*/true);
        if (fonts) {
            for (const auto& entry : kFontSubstitutes) {
                fonts->values[std::wstring(entry.alias.begin(), entry.alias.end())] =
                    RegValue{RegType::Sz,
                             {std::wstring(entry.target.begin(), entry.target.end())}};
            }
        }
    }

    // HKLM\SOFTWARE\Microsoft\DirectX: every Windows since XP SP2 records
    // the installed DirectX runtime here as Version "4.09.00.0904" (REG_SZ)
    // and InstalledVersion 00 00 00 09 00 00 00 00 (REG_BINARY), in both
    // registry views (winoracle `reg query`, Windows 11 26200). The DirectX
    // redistributable's dxupdate.dll reads Version before installing and
    // fails the whole setup without it.
    auto dx_parts = split_reg_path(L"SOFTWARE\\Microsoft\\DirectX");
    RegNode* dx = walk_path(m_hklm.get(), dx_parts, /*create=*/true);
    if (dx) {
        dx->values[L"Version"] = RegValue{RegType::Sz, {L"4.09.00.0904"s}};
        dx->values[L"InstalledVersion"] =
            RegValue{RegType::Binary, {std::vector<u8>{0, 0, 0, 9, 0, 0, 0, 0}}};
    }

    // HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion, distinct from the NT
    // key above. Installer property-bag init opens this key.
    // ERROR_FILE_NOT_FOUND here becomes a later procedure exception.
    auto win_cv_parts =
        split_reg_path(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion");
    RegNode* win_cv = walk_path(m_hklm.get(), win_cv_parts, /*create=*/true);
    if (win_cv) {
        win_cv->values[L"ProgramFilesDir"] =
            RegValue{RegType::Sz, {L"C:\\Program Files"s}};
        win_cv->values[L"ProgramFilesDir (x86)"] =
            RegValue{RegType::Sz, {L"C:\\Program Files (x86)"s}};
        win_cv->values[L"CommonFilesDir"] =
            RegValue{RegType::Sz, {L"C:\\Program Files\\Common Files"s}};
        win_cv->values[L"CommonFilesDir (x86)"] =
            RegValue{RegType::Sz,
                     {L"C:\\Program Files (x86)\\Common Files"s}};
        win_cv->values[L"ProgramFilesPath"] =
            RegValue{RegType::ExpandSz, {L"%ProgramFiles%"s}};
        win_cv->values[L"CommonFilesPath"] =
            RegValue{RegType::ExpandSz, {L"%CommonProgramFiles%"s}};
        // 64-bit Windows names the native directories for both views
        // (tests/oracle_data/wow64_registry_seeds.x86.expected).
        win_cv->values[L"ProgramW6432Dir"] =
            RegValue{RegType::Sz, {L"C:\\Program Files"s}};
        win_cv->values[L"CommonW6432Dir"] =
            RegValue{RegType::Sz, {L"C:\\Program Files\\Common Files"s}};
    }

    // Class registrations for the MSXML DOM coclasses. HKEY_CLASSES_ROOT is
    // a merged view over HKLM\Software\Classes and HKCU\Software\Classes,
    // so these must live in the machine class store to be reachable there.
    // ProgID -> CLSID for the MSXML DOM coclasses. MSXML ships with
    // Windows, so these registrations exist on every installation; WiX Burn and
    // other installers resolve them with CLSIDFromProgID(L"Msxml2.DOMDocument")
    // and fail with CO_E_CLASSSTRING when they are absent.
    {
        // Registrations as Windows 11 ships them (HKCR read on winoracle,
        // tests/oracle_data/msxml_dom_registry.expected). Every
        // version-independent and 3.0 coclass lives in msxml3.dll, the 6.0
        // ones in msxml6.dll; 2.6 and 4.0 are not registered at all.
        struct MsxmlProgId {
            const wchar_t* progid;
            const wchar_t* clsid;
            const wchar_t* description;
            const wchar_t* cur_ver;   ///< nullptr when the key has no CurVer
        };
        struct MsxmlClass {
            const wchar_t* clsid;
            const wchar_t* description;
            const wchar_t* progid;
            const wchar_t* version_independent; ///< nullptr when absent
            const wchar_t* server;
        };
        static constexpr wchar_t kDomDocument[]   =
            L"{2933BF90-7B36-11D2-B20E-00C04F983E60}";
        static constexpr wchar_t kFreeThreaded[]  =
            L"{2933BF91-7B36-11D2-B20E-00C04F983E60}";
        static constexpr wchar_t kDomDocument2[]  =
            L"{F6D90F11-9C73-11D3-B32E-00C04F990BB4}";
        static constexpr wchar_t kFreeThreaded2[] =
            L"{F6D90F12-9C73-11D3-B32E-00C04F990BB4}";
        static constexpr wchar_t kDomDocument30[] =
            L"{F5078F32-C551-11D3-89B9-0000F81FE221}";
        static constexpr wchar_t kFreeThreaded30[] =
            L"{F5078F33-C551-11D3-89B9-0000F81FE221}";
        static constexpr wchar_t kDomDocument60[] =
            L"{88D96A05-F192-11D4-A65F-0040963251E5}";
        static constexpr wchar_t kFreeThreaded60[] =
            L"{88D96A06-F192-11D4-A65F-0040963251E5}";
        static constexpr wchar_t kDom[]   = L"XML DOM Document";
        static constexpr wchar_t kFtDom[] = L"Free Threaded XML DOM Document";

        const MsxmlProgId progids[] = {
            {L"Microsoft.XMLDOM", kDomDocument, kDom, L"Microsoft.XMLDOM.1.0"},
            {L"Microsoft.XMLDOM.1.0", kDomDocument, kDom, nullptr},
            {L"Microsoft.FreeThreadedXMLDOM", kFreeThreaded, kFtDom, nullptr},
            {L"Microsoft.FreeThreadedXMLDOM.1.0", kFreeThreaded, kFtDom, nullptr},
            {L"MSXML.DOMDocument", kDomDocument, kDom, L"Microsoft.XMLDOM.1.0"},
            {L"Msxml2.DOMDocument", kDomDocument2, kDom,
             L"Msxml2.DOMDocument.3.0"},
            {L"Msxml2.FreeThreadedDOMDocument", kFreeThreaded2, kFtDom, nullptr},
            {L"Msxml2.DOMDocument.3.0", kDomDocument30,
             L"XML DOM Document 3.0", nullptr},
            {L"Msxml2.FreeThreadedDOMDocument.3.0", kFreeThreaded30,
             L"Free Threaded XML DOM Document 3.0", nullptr},
            {L"Msxml2.DOMDocument.6.0", kDomDocument60,
             L"XML DOM Document 6.0", nullptr},
            {L"Msxml2.FreeThreadedDOMDocument.6.0", kFreeThreaded60,
             L"Free Threaded XML DOM Document 6.0", nullptr},
        };
        const MsxmlClass classes_list[] = {
            {kDomDocument, kDom, L"Microsoft.XMLDOM.1.0", L"Microsoft.XMLDOM",
             L"msxml3.dll"},
            {kFreeThreaded, kFtDom, L"Microsoft.FreeThreadedXMLDOM.1.0",
             L"Microsoft.FreeThreadedXMLDOM", L"msxml3.dll"},
            {kDomDocument2, kDom, L"Msxml2.DOMDocument", L"Msxml2.DOMDocument",
             L"msxml3.dll"},
            {kFreeThreaded2, kFtDom, L"Msxml2.FreeThreadedDOMDocument",
             L"Msxml2.FreeThreadedDOMDocument", L"msxml3.dll"},
            {kDomDocument30, L"XML DOM Document 3.0", L"Msxml2.DOMDocument.3.0",
             L"Msxml2.DOMDocument", L"msxml3.dll"},
            {kFreeThreaded30, L"Free Threaded XML DOM Document 3.0",
             L"Msxml2.FreeThreadedDOMDocument.3.0",
             L"Msxml2.FreeThreadedDOMDocument", L"msxml3.dll"},
            {kDomDocument60, L"XML DOM Document 6.0", L"Msxml2.DOMDocument.6.0",
             nullptr, L"msxml6.dll"},
            {kFreeThreaded60, L"Free Threaded XML DOM Document 6.0",
             L"Msxml2.FreeThreadedDOMDocument.6.0", nullptr, L"msxml6.dll"},
        };

        const std::wstring classes = L"Software\\Classes\\";
        const auto set_default = [&](const std::wstring& path,
                                     const wchar_t* value) {
            RegNode* node = walk_path(m_hklm.get(), split_reg_path(path),
                                      /*create=*/true);
            if (node != nullptr) {
                node->values[L""] = RegValue{RegType::Sz, {std::wstring(value)}};
            }
            return node;
        };
        for (const auto& entry : progids) {
            const std::wstring root = classes + entry.progid;
            (void)set_default(root, entry.description);
            (void)set_default(root + L"\\CLSID", entry.clsid);
            if (entry.cur_ver != nullptr) {
                (void)set_default(root + L"\\CurVer", entry.cur_ver);
            }
        }
        for (const auto& entry : classes_list) {
            const std::wstring root = classes + L"CLSID\\" + entry.clsid;
            (void)set_default(root, entry.description);
            if (RegNode* inproc =
                    set_default(root + L"\\InprocServer32", entry.server)) {
                inproc->values[L"ThreadingModel"] =
                    RegValue{RegType::Sz, {L"Both"s}};
            }
            (void)set_default(root + L"\\ProgID", entry.progid);
            if (entry.version_independent != nullptr) {
                (void)set_default(root + L"\\VersionIndependentProgID",
                                  entry.version_independent);
            }
        }
    }

    // DirectSound capture coclasses (dsound.h): CoCreateInstance hands out an
    // uninitialized object that IDirectSoundCapture8::Initialize /
    // IDirectSoundFullDuplex::Initialize binds to a device. dsound.dll's
    // DllGetClassObject serves them.
    {
        struct DsoundClass {
            const wchar_t* clsid;
            const wchar_t* description;
        };
        const DsoundClass entries[] = {
            {L"{B0210780-89CD-11D0-AF08-00A0C925CD16}", L"DirectSoundCapture Object"},
            {L"{E4BCAC13-7F99-4908-9A8E-74E3BF24B6E1}", L"DirectSoundCapture8 Object"},
            {L"{FEA4300C-7959-4147-B26A-2377B9E7A91D}", L"DirectSoundFullDuplex Object"},
        };
        for (const auto& entry : entries) {
            const std::wstring clsid_root =
                std::wstring(L"Software\\Classes\\CLSID\\") + entry.clsid;
            RegNode* clsid_node = walk_path(
                m_hklm.get(), split_reg_path(clsid_root), /*create=*/true);
            if (clsid_node != nullptr) {
                clsid_node->values[L""] =
                    RegValue{RegType::Sz, {std::wstring(entry.description)}};
            }
            RegNode* inproc = walk_path(
                m_hklm.get(), split_reg_path(clsid_root + L"\\InprocServer32"),
                /*create=*/true);
            if (inproc != nullptr) {
                inproc->values[L""] = RegValue{RegType::Sz, {L"dsound.dll"s}};
                inproc->values[L"ThreadingModel"] = RegValue{RegType::Sz, {L"Both"s}};
            }
        }
    }

    // WIC imaging factory coclasses (wincodec.h CLSID_WICImagingFactory1 /
    // CLSID_WICImagingFactory2): served by windowscodecs.dll's
    // DllGetClassObject, free-threaded ("Both") like every WIC component.
    for (const wchar_t* clsid : {L"{CACAF262-9370-4615-A13B-9F5539DA4C0A}",
                                 L"{317D06E8-5F24-433D-BDF7-79CE68D8ABC2}"}) {
        RegNode* inproc = walk_path(
            m_hklm.get(),
            split_reg_path(std::wstring(L"Software\\Classes\\CLSID\\") + clsid +
                           L"\\InprocServer32"),
            /*create=*/true);
        if (inproc != nullptr) {
            inproc->values[L""] = RegValue{RegType::Sz, {L"windowscodecs.dll"s}};
            inproc->values[L"ThreadingModel"] = RegValue{RegType::Sz, {L"Both"s}};
        }
    }

    // Pre-populate the per-user shell folder paths. Windows always has
    // HKCU\...\Explorer\Shell Folders. Installers read "Programs" and
    // "Desktop" from it to place shortcuts, and abort with
    // "No program start menu found." when the key is missing. Use the
    // capsule-local guest paths under C:\.
    auto seed_shell_folders = [this](RegNode* root) {
        if (!root) return;
        auto sf_parts = split_reg_path(
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders");
        RegNode* sf = walk_path(root, sf_parts, /*create=*/true);
        if (!sf) return;
        auto set = [&](const wchar_t* name, const wchar_t* path) {
            sf->values[name] = RegValue{RegType::Sz, {std::wstring(path)}};
        };
        set(L"Desktop",    L"C:\\Windows\\Desktop");
        set(L"Programs",   L"C:\\Windows\\Start Menu\\Programs");
        set(L"Start Menu", L"C:\\Windows\\Start Menu");
        set(L"Startup",    L"C:\\Windows\\Start Menu\\Programs\\Startup");
        set(L"Personal",   L"C:\\My Documents");
        set(L"Favorites",  L"C:\\Windows\\Favorites");
        set(L"Fonts",      L"C:\\Windows\\Fonts");
        set(L"SendTo",     L"C:\\Windows\\SendTo");
        set(L"Recent",     L"C:\\Windows\\Recent");
        set(L"AppData",    L"C:\\Windows\\Application Data");
        set(L"Templates",  L"C:\\Windows\\ShellNew");
    };
    seed_shell_folders(m_hkcu.get());

    // Windows publishes per-user and per-machine Run/RunOnce keys as the
    // standard registration points for logon applications.  Installers open
    // these keys without creating them before writing their startup value.
    for (const wchar_t* relative : {
             L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
             L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce"}) {
        auto run_parts = split_reg_path(relative);
        (void)walk_path(m_hkcu.get(), run_parts, /*create=*/true);
        (void)walk_path(m_hklm.get(), run_parts, /*create=*/true);
    }

    // Common (all-users) shell folders under HKLM. PathGetSpecialFolder and
    // SHGetFolderPath fall back to these.
    {
        auto sf_parts = split_reg_path(
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders");
        RegNode* sf = walk_path(m_hklm.get(), sf_parts, /*create=*/true);
        if (sf) {
            auto set = [&](const wchar_t* name, const wchar_t* path) {
                sf->values[name] = RegValue{RegType::Sz, {std::wstring(path)}};
            };
            set(L"Common Desktop",
                L"C:\\Users\\Public\\Desktop");
            set(L"Common Start Menu",
                L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu");
            set(L"Common Programs",
                L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs");
            set(L"Common Startup",
                L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
            set(L"Common AppData", L"C:\\ProgramData");
        }
    }

    // Name resolution requires
    // HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters to exist so
    // RegOpenKey(KEY_NOTIFY) and RegNotifyChangeKeyValue can start. If the
    // watch fails, that guest publishes an empty DNS configuration and name
    // resolution never starts.
    {
        auto seed_svc = [this](const wchar_t* relative) {
            auto parts = split_reg_path(relative);
            return walk_path(m_hklm.get(), parts, /*create=*/true);
        };
        RegNode* tcpip = seed_svc(
            L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters");
        if (tcpip) {
            // Space-separated list. The same guest also reads adapters via
            // GetAdaptersAddresses. Keep a reachable public resolver as a
            // fallback NameServer for registry-based readers.
            if (tcpip->values.find(L"NameServer") == tcpip->values.end()) {
                tcpip->values[L"NameServer"] =
                    RegValue{RegType::Sz, {L"1.1.1.1"s}};
            }
            if (tcpip->values.find(L"Domain") == tcpip->values.end()) {
                tcpip->values[L"Domain"] = RegValue{RegType::Sz, {L""s}};
            }
            if (tcpip->values.find(L"SearchList") == tcpip->values.end()) {
                tcpip->values[L"SearchList"] = RegValue{RegType::Sz, {L""s}};
            }
        }
        (void)seed_svc(
            L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters");
        (void)seed_svc(
            L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters");
    }

    // HKLM\HARDWARE\DEVICEMAP\Scsi. Windows always publishes ATAPI/SCSI
    // CD-ROM targets here. CD authentication checks open
    // "HARDWARE\Devicemap\SCSI\SCSI Port 0" after IOCTL_SCSI_GET_ADDRESS and
    // refuse authentication when the map is missing. Use a plain ATAPI
    // identifier. Guest protectors blacklist virtual-machine firmware names.
    {
        auto port_parts = split_reg_path(L"HARDWARE\\DEVICEMAP\\Scsi\\Scsi Port 0");
        RegNode* port = walk_path(m_hklm.get(), port_parts, /*create=*/true);
        if (port) {
            port->values[L"Driver"] = RegValue{RegType::Sz, {L"atapi"s}};
            port->values[L"FirstBus"] = RegValue{RegType::Dword, {u32{0}}};
            port->values[L"MaxInitiatorBus"] =
                RegValue{RegType::Dword, {u32{0}}};
            // Optional port resources some CD checks query after Driver.
            port->values[L"Interrupt"] = RegValue{RegType::Dword, {u32{11}}};
            port->values[L"IOAddress"] =
                RegValue{RegType::Dword, {u32{0x1F0}}};
        }
        auto lun_parts = split_reg_path(
            L"HARDWARE\\DEVICEMAP\\Scsi\\Scsi Port 0\\Scsi Bus 0\\"
            L"Target Id 0\\Logical Unit Id 0");
        RegNode* lun = walk_path(m_hklm.get(), lun_parts, /*create=*/true);
        if (lun) {
            // Inquiry-style Identifier: 8-char vendor + 16-char product + rev.
            lun->values[L"Identifier"] = RegValue{
                RegType::Sz, {L"LITEON  DVDRW SHW-160P6LSHS0P"s}};
            lun->values[L"Type"] =
                RegValue{RegType::Sz, {L"CdRomPeripheral"s}};
            lun->values[L"DeviceName"] =
                RegValue{RegType::Sz, {L"CdRom0"s}};
        }
        // Companion service key referenced via the port Driver value.
        auto atapi_parts =
            split_reg_path(L"SYSTEM\\CurrentControlSet\\Services\\atapi");
        RegNode* atapi = walk_path(m_hklm.get(), atapi_parts, /*create=*/true);
        if (atapi) {
            atapi->values[L"Type"] = RegValue{RegType::Dword, {u32{1}}};
            atapi->values[L"Start"] = RegValue{RegType::Dword, {u32{0}}};
            atapi->values[L"ErrorControl"] =
                RegValue{RegType::Dword, {u32{1}}};
            atapi->values[L"Group"] =
                RegValue{RegType::Sz, {L"SCSI Miniport"s}};
            atapi->values[L"ImagePath"] = RegValue{
                RegType::ExpandSz,
                {L"System32\\drivers\\atapi.sys"s}};
            atapi->values[L"DisplayName"] =
                RegValue{RegType::Sz, {L"Standard IDE/ESDI Hard Disk Controller"s}};
        }
    }

    // The Windows Installer service (Learn "Windows Installer Service"):
    // msiserver, a demand-start own-process service running msiexec.exe /V
    // as LocalSystem. Clients (MsiInstallProduct, msiexec /i) start it
    // through the SCM. The ImagePath is written expanded: this SCM does not
    // expand REG_EXPAND_SZ image paths (Windows stores
    // "%systemroot%\system32\msiexec.exe /V").
    {
        RegNode* msiserver = walk_path(
            m_hklm.get(), split_reg_path(L"SYSTEM\\CurrentControlSet\\Services\\msiserver"),
            /*create=*/true);
        if (msiserver != nullptr) {
            msiserver->values[L"Type"] = RegValue{RegType::Dword, {u32{0x10}}};
            msiserver->values[L"Start"] = RegValue{RegType::Dword, {u32{3}}};
            msiserver->values[L"ErrorControl"] = RegValue{RegType::Dword, {u32{1}}};
            msiserver->values[L"ImagePath"] = RegValue{
                RegType::ExpandSz, {L"C:\\Windows\\system32\\msiexec.exe /V"s}};
            msiserver->values[L"ObjectName"] = RegValue{RegType::Sz, {L"LocalSystem"s}};
            msiserver->values[L"DisplayName"] = RegValue{RegType::Sz, {L"Windows Installer"s}};
        }
    }

    // The 32-bit registry view (Learn, "Registry Redirector"). 64-bit
    // Windows ships every key below in both views: the redirected ones as a
    // second copy under SOFTWARE\WOW6432Node, the 32-bit COM classes under
    // SOFTWARE\Classes\WOW6432Node\CLSID, measured in
    // tests/oracle_data/wow64_registry_seeds.x86.expected. Shared keys
    // (FontSubstitutes, the ProgIDs, HKCU\Software, SYSTEM) have one copy.
    {
        const auto mirror = [](const auto& self, const RegNode* from, RegNode* to) -> void {
            for (const auto& [name, value] : from->values) to->values[name] = value;
            for (const auto& [name, child] : from->children) {
                auto& target = to->children[name];
                if (!target) target = std::make_unique<RegNode>(child->name);
                self(self, child.get(), target.get());
            }
        };
        const auto mirror_path = [&](const wchar_t* native, const wchar_t* wow) {
            RegNode* from = walk_path(m_hklm.get(), split_reg_path(native), /*create=*/false);
            RegNode* to = walk_path(m_hklm.get(), split_reg_path(wow), /*create=*/true);
            if (from != nullptr && to != nullptr) mirror(mirror, from, to);
            return to;
        };
        (void)mirror_path(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion");
        // FontSubstitutes is shared: the 32-bit view opens the native key.
        if (RegNode* nt32 = walk_path(m_hklm.get(),
                                      split_reg_path(L"SOFTWARE\\WOW6432Node\\Microsoft\\"
                                                     L"Windows NT\\CurrentVersion"),
                                      /*create=*/false)) {
            nt32->children.erase(L"FontSubstitutes");
        }
        (void)mirror_path(L"SOFTWARE\\Microsoft\\DirectX",
                          L"SOFTWARE\\WOW6432Node\\Microsoft\\DirectX");
        if (RegNode* cv32 = walk_path(m_hklm.get(),
                                      split_reg_path(L"SOFTWARE\\WOW6432Node\\Microsoft\\"
                                                     L"Windows\\CurrentVersion"),
                                      /*create=*/true)) {
            // The 32-bit view's ProgramFilesDir and CommonFilesDir name the
            // x86 directories; CommonFilesPath is a Vitreo-only native seed.
            cv32->values[L"ProgramFilesDir"] =
                RegValue{RegType::Sz, {L"C:\\Program Files (x86)"s}};
            cv32->values[L"CommonFilesDir"] =
                RegValue{RegType::Sz, {L"C:\\Program Files (x86)\\Common Files"s}};
            cv32->values[L"ProgramFilesDir (x86)"] =
                RegValue{RegType::Sz, {L"C:\\Program Files (x86)"s}};
            cv32->values[L"CommonFilesDir (x86)"] =
                RegValue{RegType::Sz, {L"C:\\Program Files (x86)\\Common Files"s}};
            cv32->values[L"ProgramFilesPath"] =
                RegValue{RegType::ExpandSz, {L"%ProgramFiles%"s}};
            cv32->values[L"ProgramW6432Dir"] =
                RegValue{RegType::Sz, {L"C:\\Program Files"s}};
            cv32->values[L"CommonW6432Dir"] =
                RegValue{RegType::Sz, {L"C:\\Program Files\\Common Files"s}};
        }
        for (const wchar_t* key : {L"Run", L"RunOnce", L"Explorer\\Shell Folders"}) {
            const std::wstring native =
                std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\") + key;
            const std::wstring wow =
                std::wstring(L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\") + key;
            (void)mirror_path(native.c_str(), wow.c_str());
        }
        // HKLM\SOFTWARE\Microsoft\.NETFramework\InstallRoot: the directory
        // the OS .NET shim (mscoree.dll, an OS component since Windows 8:
        // netfx-mscoree_dll 10.0.26100 in WinSxS, "donoteverremove") and the
        // runtimes it loads (mscoreei.dll, clr.dll) find every Framework
        // version under. Windows ships it per view (wow64_registry_seeds
        // .x86.expected: Framework64\ native, Framework\ in WOW6432Node);
        // the .NET Framework 4 redistributable does not write it
        // (.docs/contracts/mscoree.md). Vitreo provides that OS shim
        // (src/winapi/system/mscoree), so the value is part of the OS here
        // too. Nothing else of .NET is seeded: the Framework itself, its
        // Policy and NDP keys, belong to whatever installs it.
        if (RegNode* net = walk_path(m_hklm.get(), split_reg_path(L"SOFTWARE\\Microsoft\\.NETFramework"),
                                     /*create=*/true)) {
            net->values[L"InstallRoot"] =
                RegValue{RegType::Sz, {L"C:\\Windows\\Microsoft.NET\\Framework64\\"s}};
        }
        if (RegNode* net32 = walk_path(m_hklm.get(),
                                       split_reg_path(L"SOFTWARE\\WOW6432Node\\Microsoft\\.NETFramework"),
                                       /*create=*/true)) {
            net32->values[L"InstallRoot"] =
                RegValue{RegType::Sz, {L"C:\\Windows\\Microsoft.NET\\Framework\\"s}};
        }

        // Every OS COM class seeded above is registered for both bitnesses
        // (msxml3/msxml6, dsound, windowscodecs: InprocServer32 in
        // Classes\CLSID and Classes\WOW6432Node\CLSID). The bare server
        // names resolve to the caller's own System32 or SysWOW64 copy.
        (void)mirror_path(L"SOFTWARE\\Classes\\CLSID",
                          L"SOFTWARE\\Classes\\WOW6432Node\\CLSID");
    }

    // HKLM\HARDWARE\DESCRIPTION is published from the host hardware at every
    // process start (winapi::publish_hardware_description); like the rest of
    // HKLM\HARDWARE it is volatile and never saved.
}

} // namespace vitreo::capsule
