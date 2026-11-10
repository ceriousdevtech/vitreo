// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import Foundation

/// `/usr/local/bin/vitreo` is a symlink into the installed app, so Terminal
/// and the app stay on the same engine. Dragging Vitreo to Applications does
/// not create it. The first launch of an installed copy asks the user, says
/// why, and only on their yes writes the link (which may make macOS ask for
/// an administrator password). The answer is remembered; Settings can add
/// the command later.
enum CommandLineLink {
    static let path = "/usr/local/bin/vitreo"
    static let consentDefaultsKey = "vitreoCLILinkConsent"
    /// Earlier builds remembered only a cancelled password dialog. A user who
    /// cancelled one has already said no.
    static let legacyDeclinedDefaultsKey = "vitreoCLILinkDeclined"

    enum Consent: String {
        case undecided
        case granted
        case declined
    }

    /// What a launch does about the command.
    enum Step: Equatable {
        /// Nothing: no link is needed, or the user said no.
        case none
        /// Ask the user first. `retarget` says an earlier link points at
        /// another copy of Vitreo.
        case ask(retarget: Bool)
        /// Write the link without asking: the user agreed and no password is
        /// needed.
        case install
        /// Leave a non-symlink at the command path alone and say so in the log.
        case reportBlocked
    }

    static func consent(in defaults: UserDefaults) -> Consent {
        if let raw = defaults.string(forKey: consentDefaultsKey),
           let value = Consent(rawValue: raw) {
            return value
        }
        return defaults.bool(forKey: legacyDeclinedDefaultsKey) ? .declined : .undecided
    }

    static func record(_ consent: Consent, in defaults: UserDefaults) {
        defaults.set(consent.rawValue, forKey: consentDefaultsKey)
        defaults.removeObject(forKey: legacyDeclinedDefaultsKey)
    }

    /// The launch-time decision. Nothing is written before the user agrees,
    /// and macOS's password dialog never appears without Vitreo's own
    /// explanation in front of it: a retarget that needs a password is asked
    /// again even after an earlier yes.
    static func launchStep(
        plan: Plan, consent: Consent, needsAdministrator: Bool
    ) -> Step {
        switch plan {
        case .skip, .alreadyLinked:
            return .none
        case .blockedByRegularFile:
            return .reportBlocked
        case .create, .replaceSymlink:
            switch consent {
            case .declined:
                return .none
            case .undecided:
                return .ask(retarget: plan == .replaceSymlink)
            case .granted:
                return needsAdministrator
                    ? .ask(retarget: plan == .replaceSymlink)
                    : .install
            }
        }
    }

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
    /// The command's state for this copy of Vitreo, for Settings.
    enum Status: Equatable {
        /// This is not an installed copy (a build product or the disk image).
        case unavailable
        case linked
        case notLinked
        case blockedByRegularFile
    }

    static func status(bundle: URL = Bundle.main.bundleURL) -> Status {
        switch currentPlan(bundle: bundle) {
        case .skip: return .unavailable
        case .alreadyLinked: return .linked
        case .create, .replaceSymlink: return .notLinked
        case .blockedByRegularFile: return .blockedByRegularFile
        }
    }

    /// Launch of the Manager: ask once, then keep the command pointing at
    /// this copy. Main thread; may block in a modal alert.
    @MainActor
    static func ensure(
        bundle: URL = Bundle.main.bundleURL,
        defaults: UserDefaults = .standard
    ) {
        let plan = currentPlan(bundle: bundle)
        let step = CommandLineLink.launchStep(
            plan: plan,
            consent: CommandLineLink.consent(in: defaults),
            needsAdministrator: needsAdministrator)
        switch step {
        case .none:
            return
        case .reportBlocked:
            NSLog(
                "Vitreo left \(CommandLineLink.path) unchanged because it is not a symlink")
        case .install:
            _ = install(bundle: bundle)
        case .ask(let retarget):
            guard askPermission(retarget: retarget) else {
                CommandLineLink.record(.declined, in: defaults)
                return
            }
            // Cancelling macOS's password dialog after "Add" is also a no;
            // the next launch stays quiet and Settings can still add it.
            CommandLineLink.record(
                install(bundle: bundle) ? .granted : .declined, in: defaults)
        }
    }

    /// Settings ▸ Command Line Tool: the user asked for the command, which is
    /// their consent. Returns an error message, or nil once it is linked.
    @MainActor
    static func installFromSettings(
        bundle: URL = Bundle.main.bundleURL,
        defaults: UserDefaults = .standard
    ) -> String? {
        switch currentPlan(bundle: bundle) {
        case .skip:
            return "Only a copy of Vitreo in Applications can add the command."
        case .alreadyLinked:
            CommandLineLink.record(.granted, in: defaults)
            return nil
        case .blockedByRegularFile:
            return "\(CommandLineLink.path) is a file Vitreo did not create; "
                + "remove it first."
        case .create, .replaceSymlink:
            CommandLineLink.record(.granted, in: defaults)
            return install(bundle: bundle)
                ? nil
                : "The vitreo command was not added."
        }
    }

    private static func currentPlan(bundle: URL) -> CommandLineLink.Plan {
        let engine = engineURL(bundle)
        let inspected = inspectLink()
        return CommandLineLink.plan(
            bundlePath: bundle.path,
            engineExists: FileManager.default.isExecutableFile(atPath: engine.path),
            linkExists: inspected.exists,
            linkIsSymlink: inspected.isSymlink,
            linkDestination: inspected.destination)
    }

    private static func engineURL(_ bundle: URL) -> URL {
        bundle.appendingPathComponent("Contents/Helpers/vitreo")
    }

    /// `/usr/local/bin` writable by this user (Homebrew on Intel makes it so)
    /// needs no password; otherwise macOS asks for one.
    private static var needsAdministrator: Bool {
        !FileManager.default.isWritableFile(atPath: "/usr/local/bin")
    }

    /// Vitreo's own explanation, shown before anything is written or macOS
    /// asks for a password.
    @MainActor
    private static func askPermission(retarget: Bool) -> Bool {
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = retarget
            ? "Update the vitreo command for Terminal?"
            : "Add the vitreo command for Terminal?"
        var reason = retarget
            ? "The vitreo command in \(CommandLineLink.path) points at another "
                + "copy of Vitreo. Updating it makes Terminal use this copy, so "
                + "Terminal and the app run the same engine."
            : "Vitreo can add a vitreo command to \(CommandLineLink.path) so you "
                + "can run Windows programs, mount images and manage capsules "
                + "from Terminal. It is a link to the engine inside this app, so "
                + "Terminal and the app always run the same version."
        if needsAdministrator {
            reason += "\n\n\(CommandLineLink.path) belongs to the system, so "
                + "macOS will ask for an administrator password next."
        }
        reason += "\n\nYou don't need it to use the app. You can add it later "
            + "in Settings \u{25B8} Engine."
        alert.informativeText = reason
        alert.addButton(withTitle: retarget ? "Update Command" : "Add Command")
        alert.addButton(withTitle: "Not Now")
        NSApplication.shared.activate(ignoringOtherApps: true)
        return alert.runModal() == .alertFirstButtonReturn
    }

    /// Writes the link, directly or through macOS's administrator prompt.
    @discardableResult
    private static func install(bundle: URL) -> Bool {
        let engine = engineURL(bundle).path
        if !needsAdministrator {
            do {
                try installLink(to: engine)
                return true
            } catch {
                NSLog("Vitreo could not link \(CommandLineLink.path): \(error.localizedDescription)")
                return false
            }
        }
        return authorizeLink(to: engine)
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
