// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

/// `/usr/local/bin/vitreo` is a symlink into the installed app. Dragging
/// Vitreo to Applications does not create it; the first launch of that copy
/// does, so Terminal and the app stay on the same engine.
enum CommandLineLink {
    static let path = "/usr/local/bin/vitreo"
    static let declinedDefaultsKey = "vitreoCLILinkDeclined"

    enum Plan: Equatable {
        case skip
        case alreadyLinked
        case create
        case replaceSymlink
        case blockedByRegularFile
    }

    /// The installed product lives in `/Applications` or a user's
    /// `~/Applications`. A build product or a copy still on the disk image
    /// must not take over the command.
    static func isInstalledApplication(_ bundlePath: String) -> Bool {
        let path = URL(fileURLWithPath: bundlePath).standardizedFileURL.path
        if path == "/Applications/Vitreo.app" { return true }
        guard path.hasPrefix("/Users/") else { return false }
        let pieces = path.dropFirst("/Users/".count).split(
            separator: "/", omittingEmptySubsequences: false)
        return pieces.count == 3
            && !pieces[0].isEmpty
            && pieces[1] == "Applications"
            && pieces[2] == "Vitreo.app"
    }

    static func plan(
        bundlePath: String,
        engineExists: Bool,
        linkExists: Bool,
        linkIsSymlink: Bool,
        linkDestination: String?
    ) -> Plan {
        guard engineExists, isInstalledApplication(bundlePath) else {
            return .skip
        }
        guard linkExists else { return .create }
        guard linkIsSymlink else { return .blockedByRegularFile }
        let engine = URL(fileURLWithPath: bundlePath)
            .appendingPathComponent("Contents/Helpers/vitreo")
            .standardizedFileURL.path
        if destinationMatches(linkDestination, engine: engine) {
            return .alreadyLinked
        }
        return .replaceSymlink
    }

    static func destinationMatches(
        _ linkDestination: String?, engine: String
    ) -> Bool {
        guard let linkDestination, !linkDestination.isEmpty else { return false }
        let destination: URL
        if linkDestination.hasPrefix("/") {
            destination = URL(fileURLWithPath: linkDestination)
        } else {
            destination = URL(fileURLWithPath: "/usr/local/bin")
                .appendingPathComponent(linkDestination)
        }
        return destination.standardizedFileURL.path
            == URL(fileURLWithPath: engine).standardizedFileURL.path
    }
}

enum CommandLineLinkInstaller {
    /// Create or retarget the command when this process is an installed
    /// Vitreo. A writable `/usr/local/bin` is updated directly. Otherwise
    /// macOS asks for an administrator password once; cancelling that dialog
    /// is remembered so later launches stay quiet.
    static func ensure(
        bundle: URL = Bundle.main.bundleURL,
        defaults: UserDefaults = .standard
    ) {
        let engine = bundle.appendingPathComponent("Contents/Helpers/vitreo")
        let inspected = inspectLink()
        let decision = CommandLineLink.plan(
            bundlePath: bundle.path,
            engineExists: FileManager.default.isExecutableFile(atPath: engine.path),
            linkExists: inspected.exists,
            linkIsSymlink: inspected.isSymlink,
            linkDestination: inspected.destination)
        switch decision {
        case .skip, .alreadyLinked:
            return
        case .blockedByRegularFile:
            NSLog(
                "Vitreo left \(CommandLineLink.path) unchanged because it is not a symlink")
            return
        case .create, .replaceSymlink:
            if FileManager.default.isWritableFile(atPath: "/usr/local/bin") {
                do {
                    try installLink(to: engine.path)
                    defaults.set(false, forKey: CommandLineLink.declinedDefaultsKey)
                } catch {
                    NSLog("Vitreo could not link \(CommandLineLink.path): \(error.localizedDescription)")
                }
                return
            }
            guard !defaults.bool(forKey: CommandLineLink.declinedDefaultsKey) else {
                return
            }
            if authorizeLink(to: engine.path) {
                defaults.set(false, forKey: CommandLineLink.declinedDefaultsKey)
            } else {
                defaults.set(true, forKey: CommandLineLink.declinedDefaultsKey)
            }
        }
    }

    private static func inspectLink() -> (
        exists: Bool, isSymlink: Bool, destination: String?
    ) {
        var info = stat()
        guard lstat(CommandLineLink.path, &info) == 0 else {
            return (false, false, nil)
        }
        let isSymlink = (info.st_mode & S_IFMT) == S_IFLNK
        let destination = isSymlink
            ? try? FileManager.default.destinationOfSymbolicLink(
                atPath: CommandLineLink.path)
            : nil
        return (true, isSymlink, destination)
    }

    private static func installLink(to engine: String) throws {
        if lstat(CommandLineLink.path, nil) == 0 {
            try FileManager.default.removeItem(atPath: CommandLineLink.path)
        }
        try FileManager.default.createSymbolicLink(
            atPath: CommandLineLink.path, withDestinationPath: engine)
    }

    private static func authorizeLink(to engine: String) -> Bool {
        let command = "mkdir -p /usr/local/bin && ln -sfn "
            + shellQuoted(engine) + " " + shellQuoted(CommandLineLink.path)
        let source = "do shell script \(appleScriptString(command)) "
            + "with administrator privileges"
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", source]
        do {
            try process.run()
            process.waitUntilExit()
            return process.terminationStatus == 0
        } catch {
            NSLog("Vitreo could not ask to link \(CommandLineLink.path): \(error.localizedDescription)")
            return false
        }
    }

    private static func shellQuoted(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func appleScriptString(_ value: String) -> String {
        let escaped = value
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }
}

private func lstat(_ path: String, _ info: UnsafeMutablePointer<stat>?) -> Int32 {
    path.withCString { Darwin.lstat($0, info) }
}
