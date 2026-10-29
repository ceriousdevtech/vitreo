// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

struct CapsuleSnapshot {
    var capsules: [CapsuleInfo]
    var apps: [InstalledApp]
}

/// Read-only discovery of capsule state. This type intentionally performs no
/// mutations; lifecycle changes remain owned by the Vitreo CLI.
struct CapsuleScanner {
    private let fileManager = FileManager.default

    func scan(root: URL) -> CapsuleSnapshot {
        let capsules = scanCapsules(root: root)
        let apps = capsules.flatMap(scanApps)
            .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        return CapsuleSnapshot(capsules: capsules, apps: apps)
    }

    /// Which capsules exist and are usable — nothing else.
    ///
    /// `scan` walks every file in every capsule to size it and to find
    /// installed apps, which on a machine with installed games costs seconds of
    /// solid I/O. Deciding where a Finder-opened executable should run needs
    /// only names and health, so it uses this instead and stays instant.
    /// `sizeBytes` and `mounts` are left empty; they belong to the UI's scan.
    func probeCapsules(root: URL) -> [CapsuleInfo] {
        guard let entries = try? fileManager.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else { return [] }

        return entries.compactMap { entry -> CapsuleInfo? in
            guard let values = try? entry.resourceValues(forKeys: [.isDirectoryKey]),
                  values.isDirectory == true else { return nil }
            return CapsuleInfo(
                name: entry.lastPathComponent,
                rootPath: entry.path,
                hasCDrive: fileManager.fileExists(
                    atPath: entry.appendingPathComponent("drives/c").path),
                hasRegistry: fileManager.fileExists(
                    atPath: entry.appendingPathComponent("registry").path),
                sizeBytes: 0,
                modifiedAt: nil,
                mounts: [])
        }.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    private func scanCapsules(root: URL) -> [CapsuleInfo] {
        guard let entries = try? fileManager.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey, .contentModificationDateKey],
            options: [.skipsHiddenFiles]
        ) else { return [] }

        return entries.compactMap { entry in
            guard let values = try? entry.resourceValues(
                forKeys: [.isDirectoryKey, .contentModificationDateKey]),
                  values.isDirectory == true else { return nil }
            let cDrive = entry.appendingPathComponent("drives/c")
            let registry = entry.appendingPathComponent("registry")
            return CapsuleInfo(
                name: entry.lastPathComponent,
                rootPath: entry.path,
                hasCDrive: fileManager.fileExists(atPath: cDrive.path),
                hasRegistry: fileManager.fileExists(atPath: registry.path),
                sizeBytes: directorySize(at: entry),
                modifiedAt: values.contentModificationDate,
                mounts: scanMounts(capsuleRoot: entry)
            )
        }.sorted {
            $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    private func directorySize(at root: URL) -> UInt64 {
        guard let enumerator = fileManager.enumerator(
            at: root,
            includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
            options: [.skipsHiddenFiles, .skipsPackageDescendants]
        ) else { return 0 }
        var total: UInt64 = 0
        for case let url as URL in enumerator {
            guard let values = try? url.resourceValues(
                forKeys: [.isRegularFileKey, .fileSizeKey]),
                  values.isRegularFile == true else { continue }
            total += UInt64(values.fileSize ?? 0)
        }
        return total
    }

    private func scanMounts(capsuleRoot: URL) -> [MountedVolume] {
        let mountRoot = capsuleRoot.appendingPathComponent("mounts")
        guard let entries = try? fileManager.contentsOfDirectory(
            at: mountRoot, includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]) else { return [] }
        return entries.compactMap { url in
            guard url.pathExtension.lowercased() == "mount",
                  let source = try? String(contentsOf: url, encoding: .utf8)
                    .trimmingCharacters(in: .whitespacesAndNewlines),
                  !source.isEmpty else { return nil }
            return MountedVolume(
                drive: url.deletingPathExtension().lastPathComponent.uppercased() + ":",
                sourcePath: source)
        }.sorted { $0.drive < $1.drive }
    }

    private struct UninstallRecord {
        let displayName: String
        let uninstallString: String
        let quietUninstallString: String?
        let installLocation: String?
    }

    private func scanApps(capsule: CapsuleInfo) -> [InstalledApp] {
        guard capsule.hasCDrive else { return [] }
        let root = URL(fileURLWithPath: capsule.rootPath)
        let cDrive = root.appendingPathComponent("drives/c")
        let uninstallRecords = loadUninstallRegistry(capsuleRoot: root)

        // Installers commonly place the same shortcut on the public desktop,
        // in the all-users Start Menu, and in the profile's Start Menu. They
        // are one app; the Start Menu copy carries the canonical name, so
        // Start Menu locations are scanned before desktops and win.
        var startMenuDirectories = [
            cDrive.appendingPathComponent("ProgramData/Microsoft/Windows/Start Menu/Programs"),
            cDrive.appendingPathComponent("Windows/Start Menu/Programs"),
        ]
        var desktopDirectories = [
            cDrive.appendingPathComponent("Users/Public/Desktop"),
            cDrive.appendingPathComponent("Windows/Desktop"),
        ]

        let users = cDrive.appendingPathComponent("Users")
        if let entries = try? fileManager.contentsOfDirectory(
            at: users,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) {
            for entry in entries {
                guard (try? entry.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true,
                      !["Public", "Default User", "All Users"].contains(entry.lastPathComponent)
                else { continue }
                startMenuDirectories.append(entry.appendingPathComponent(
                    "AppData/Roaming/Microsoft/Windows/Start Menu/Programs"))
                desktopDirectories.append(entry.appendingPathComponent("Desktop"))
            }
        }

        var apps: [InstalledApp] = []
        var seenShortcuts = Set<String>()
        var seenLaunches = Set<String>()
        for directory in startMenuDirectories + desktopDirectories {
            discoverShortcuts(
                in: directory,
                depth: 6,
                capsule: capsule,
                cDrive: cDrive,
                uninstallRecords: uninstallRecords,
                seenShortcuts: &seenShortcuts,
                seenLaunches: &seenLaunches,
                apps: &apps)
        }
        return apps
    }

    private func discoverShortcuts(
        in directory: URL,
        depth: Int,
        capsule: CapsuleInfo,
        cDrive: URL,
        uninstallRecords: [UninstallRecord],
        seenShortcuts: inout Set<String>,
        seenLaunches: inout Set<String>,
        apps: inout [InstalledApp]
    ) {
        guard depth > 0,
              let entries = try? fileManager.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.isDirectoryKey, .isRegularFileKey],
                options: [.skipsHiddenFiles]) else { return }

        for entry in entries {
            let values = try? entry.resourceValues(
                forKeys: [.isDirectoryKey, .isRegularFileKey])
            if values?.isDirectory == true {
                discoverShortcuts(
                    in: entry, depth: depth - 1, capsule: capsule, cDrive: cDrive,
                    uninstallRecords: uninstallRecords,
                    seenShortcuts: &seenShortcuts, seenLaunches: &seenLaunches,
                    apps: &apps)
                continue
            }
            guard values?.isRegularFile == true,
                  entry.pathExtension.lowercased() == "lnk",
                  seenShortcuts.insert(entry.path.lowercased()).inserted,
                  let shortcut = parseShortcut(at: entry),
                  shortcut.target.lowercased().hasSuffix(".exe"),
                  let hostPath = windowsPathToHost(shortcut.target, cDrive: cDrive),
                  fileManager.fileExists(atPath: hostPath),
                  seenLaunches.insert(launchKey(shortcut)).inserted
            else { continue }

            let uninstall = matchUninstall(
                records: uninstallRecords,
                executableWindowsPath: shortcut.target,
                displayName: entry.deletingPathExtension().lastPathComponent)
            apps.append(InstalledApp(
                name: entry.deletingPathExtension().lastPathComponent,
                capsuleName: capsule.name,
                architecture: PEInspector.architecture(
                    at: URL(fileURLWithPath: hostPath)),
                executablePath: hostPath,
                windowsExecutablePath: shortcut.target,
                lnkPath: entry.path,
                launchProfile: LaunchProfile(
                    arguments: shortcut.arguments,
                    workingDirectory: shortcut.workingDirectory)
                ,
                uninstallInfo: uninstall.map {
                    InstalledApp.UninstallInfo(
                        displayName: $0.displayName,
                        uninstallString: $0.uninstallString,
                        quietUninstallString: $0.quietUninstallString)
                }))
        }
    }

    /// Two shortcuts are the same app when they start the same executable
    /// with the same arguments; the shortcut's file name and location are
    /// presentation only.
    private func launchKey(_ shortcut: Shortcut) -> String {
        ([normalizeWindowsPath(shortcut.target)] + shortcut.arguments)
            .joined(separator: "\0")
    }

    private func loadUninstallRegistry(capsuleRoot: URL) -> [UninstallRecord] {
        let url = capsuleRoot.appendingPathComponent("registry/HKLM.reg")
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        let prefixes = [
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\",
            "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\",
        ]
        var records: [UninstallRecord] = []
        var inside = false
        var values: [String: String] = [:]

        func flush() {
            guard inside, let name = values["DisplayName"],
                  let command = values["UninstallString"] else { return }
            records.append(UninstallRecord(
                displayName: name,
                uninstallString: command,
                quietUninstallString: values["QuietUninstallString"],
                installLocation: values["InstallLocation"]))
        }

        for rawLine in text.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("["), line.hasSuffix("]") {
                flush()
                values.removeAll(keepingCapacity: true)
                let escaped = String(line.dropFirst().dropLast())
                let key = registryUnescape(escaped)
                inside = prefixes.contains { key.hasPrefix($0) && key.count > $0.count }
                continue
            }
            guard inside, line.hasPrefix("\""),
                  let separator = line.range(of: "\"=") else { continue }
            let name = registryUnescape(String(line[line.index(after: line.startIndex)..<separator.lowerBound]))
            let encoded = String(line[separator.upperBound...])
            if encoded.hasPrefix("sz:") {
                values[name] = registryUnescape(String(encoded.dropFirst(3)))
            } else if encoded.hasPrefix("expand_sz:") {
                values[name] = registryUnescape(String(encoded.dropFirst(10)))
            }
        }
        flush()
        return records
    }

    private func registryUnescape(_ value: String) -> String {
        var result = ""
        var escaped = false
        for character in value {
            if escaped {
                switch character {
                case "n": result.append("\n")
                case "r": result.append("\r")
                case "t": result.append("\t")
                case "0": result.append("\0")
                default: result.append(character)
                }
                escaped = false
            } else if character == "\\" {
                escaped = true
            } else {
                result.append(character)
            }
        }
        if escaped { result.append("\\") }
        return result
    }

    private func matchUninstall(
        records: [UninstallRecord],
        executableWindowsPath: String,
        displayName: String
    ) -> UninstallRecord? {
        let executable = normalizeWindowsPath(executableWindowsPath)
        let locationMatches = records.compactMap { record -> (Int, UninstallRecord)? in
            guard let location = record.installLocation else { return nil }
            let normalized = normalizeWindowsPath(location)
            guard !normalized.isEmpty,
                  executable == normalized || executable.hasPrefix(normalized + "\\")
            else { return nil }
            return (normalized.count, record)
        }
        if let best = locationMatches.max(by: { $0.0 < $1.0 }) { return best.1 }
        return records.first {
            displayName.localizedCaseInsensitiveContains($0.displayName) ||
            $0.displayName.localizedCaseInsensitiveContains(displayName)
        }
    }

    private func normalizeWindowsPath(_ value: String) -> String {
        value.trimmingCharacters(in: CharacterSet(charactersIn: "\" "))
            .replacingOccurrences(of: "/", with: "\\")
            .trimmingCharacters(in: CharacterSet(charactersIn: "\\"))
            .lowercased()
    }

    private func windowsPathToHost(_ path: String, cDrive: URL) -> String? {
        let clean = path.trimmingCharacters(in: CharacterSet(charactersIn: "\" \0"))
        guard clean.count >= 3 else { return nil }
        let drive = clean.prefix(2).uppercased()
        guard drive == "C:" else { return nil }
        let relative = clean.dropFirst(2)
            .replacingOccurrences(of: "\\", with: "/")
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !relative.components(separatedBy: "/").contains("..") else { return nil }
        return cDrive.appendingPathComponent(relative).path
    }

    private struct Shortcut {
        var target: String
        var arguments: [String]
        var workingDirectory: String
    }

    private func parseShortcut(at url: URL) -> Shortcut? {
        guard let data = try? Data(contentsOf: url), !data.isEmpty else { return nil }
        if let text = String(data: data, encoding: .utf8),
           text.hasPrefix("Vitreo ShellLink") {
            var fields: [String: String] = [:]
            for line in text.components(separatedBy: .newlines) {
                guard let separator = line.firstIndex(of: "=") else { continue }
                fields[String(line[..<separator])] = String(line[line.index(after: separator)...])
            }
            guard let target = fields["Target"], !target.isEmpty else { return nil }
            return Shortcut(
                target: target,
                arguments: splitWindowsCommandLine(fields["Args"] ?? ""),
                workingDirectory: fields["WorkingDir"] ?? "")
        }
        guard data.count >= 76, data.loadLE32(at: 0) == 0x4c else { return nil }
        let clsid: [UInt8] = [
            0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
        ]
        guard Array(data[4..<20]) == clsid else { return nil }
        let flags = data.loadLE32(at: 20)
        let unicode = flags & 0x80 != 0
        var offset = 76
        // HasLinkTargetIDList. The list size includes its own length field.
        if flags & 0x1 != 0 {
            guard offset + 2 <= data.count else { return nil }
            offset += 2 + Int(data.loadLE16(at: offset))
        }
        var linkTarget: String?
        // HasLinkInfo. A local path lives inside that block.
        if flags & 0x2 != 0 {
            guard offset + 28 <= data.count else { return nil }
            let infoSize = Int(data.loadLE32(at: offset))
            let headerSize = Int(data.loadLE32(at: offset + 4))
            let infoFlags = data.loadLE32(at: offset + 8)
            guard infoSize > 0, offset + infoSize <= data.count else { return nil }
            if infoFlags & 0x1 != 0 {
                if headerSize >= 0x24, offset + 32 <= data.count {
                    let unicodeOffset = Int(data.loadLE32(at: offset + 28))
                    linkTarget = data.readUTF16LEString(
                        at: offset + unicodeOffset, limit: offset + infoSize)
                }
                if linkTarget == nil {
                    let ansiOffset = Int(data.loadLE32(at: offset + 16))
                    linkTarget = data.readAnsiString(
                        at: offset + ansiOffset, limit: offset + infoSize)
                }
            }
            offset += infoSize
        }

        // StringData follows, in flag order. Installers often put the full
        // target in RELATIVE_PATH and omit LinkInfo entirely.
        func readCountedString() -> String? {
            guard offset + 2 <= data.count else { return nil }
            let count = Int(data.loadLE16(at: offset))
            offset += 2
            if unicode {
                let byteCount = count * 2
                guard offset + byteCount <= data.count else { return nil }
                var units: [UInt16] = []
                units.reserveCapacity(count)
                for index in stride(from: offset, to: offset + byteCount, by: 2) {
                    units.append(UInt16(data[index]) | (UInt16(data[index + 1]) << 8))
                }
                offset += byteCount
                return String(decoding: units, as: UTF16.self)
            }
            guard offset + count <= data.count else { return nil }
            let bytes = Data(data[offset..<(offset + count)])
            offset += count
            return String(data: bytes, encoding: .windowsCP1252)
                ?? String(data: bytes, encoding: .isoLatin1)
        }

        if flags & 0x4 != 0 { _ = readCountedString() }          // NAME_STRING
        let relative = flags & 0x8 != 0 ? readCountedString() : nil
        let working = flags & 0x10 != 0 ? (readCountedString() ?? "") : ""
        let arguments = flags & 0x20 != 0 ? (readCountedString() ?? "") : ""
        let target = (linkTarget?.isEmpty == false) ? linkTarget : relative
        guard let target, !target.isEmpty else { return nil }
        return Shortcut(
            target: target,
            arguments: splitWindowsCommandLine(arguments),
            workingDirectory: working)
    }

    private func splitWindowsCommandLine(_ value: String) -> [String] {
        var result: [String] = []
        var current = ""
        var quoted = false
        var index = value.startIndex
        while index < value.endIndex {
            let ch = value[index]
            if ch == "\"" {
                quoted.toggle()
            } else if ch.isWhitespace && !quoted {
                if !current.isEmpty {
                    result.append(current)
                    current = ""
                }
            } else {
                current.append(ch)
            }
            index = value.index(after: index)
        }
        if !current.isEmpty { result.append(current) }
        return result
    }

}

private extension Data {
    func loadLE16(at offset: Int) -> UInt16 {
        guard offset + 2 <= count else { return 0 }
        return UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func loadLE32(at offset: Int) -> UInt32 {
        guard offset + 4 <= count else { return 0 }
        return UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }

    func readAnsiString(at offset: Int, limit: Int) -> String? {
        guard offset >= 0, offset < Swift.min(limit, count) else { return nil }
        var bytes: [UInt8] = []
        for index in offset..<Swift.min(limit, count) {
            if self[index] == 0 { break }
            bytes.append(self[index])
        }
        return String(bytes: bytes, encoding: .windowsCP1252)
            ?? String(bytes: bytes, encoding: .isoLatin1)
    }

    func readUTF16LEString(at offset: Int, limit: Int) -> String? {
        guard offset >= 0, offset + 2 <= Swift.min(limit, count) else { return nil }
        var units: [UInt16] = []
        var index = offset
        while index + 1 < Swift.min(limit, count) {
            let unit = UInt16(self[index]) | (UInt16(self[index + 1]) << 8)
            if unit == 0 { break }
            units.append(unit)
            index += 2
        }
        return String(decoding: units, as: UTF16.self)
    }
}
