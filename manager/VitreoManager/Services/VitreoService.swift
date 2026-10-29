// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import Combine
import Foundation

@MainActor
final class VitreoService: ObservableObject {
    private struct MacLauncherConfiguration: Encodable {
        let executable: String
        let arguments: [String]
    }

    private enum MacLauncherError: LocalizedError {
        case helperMissing

        var errorDescription: String? {
            "The native Vitreo launcher helper is missing from Vitreo."
        }
    }

    struct RunningAppInfo: Identifiable {
        let id: String
        let app: InstalledApp
        let startedAt: Date
        let pid: pid_t
        let parentPid: pid_t
    }

    private struct AppPreference: Codable {
        var launchProfile = LaunchProfile()
        var compatibility = CompatibilityStatus.untested
        var isFavorite = false
    }

    @Published private(set) var apps: [InstalledApp] = []
    @Published private(set) var capsules: [CapsuleInfo] = []
    @Published private(set) var guestProcesses: [GuestProcessInfo] = []
    @Published private(set) var runningAppInfos: [RunningAppInfo] = []
    @Published private(set) var operations: [OperationRecord] = []
    @Published private(set) var engineStatus = EngineStatus(
        state: .missing, path: "", version: "", message: "Checking engine…")
    @Published private(set) var isRefreshing = false
    @Published private(set) var hasRefreshed = false
    @Published var lastError: String?

    private let fileManager = FileManager.default
    private let cli = VitreoCLIClient()
    private var refreshTimer: Timer?
    private var refreshTask: Task<Void, Never>?
    /// Names and health only, from `CapsuleScanner.probeCapsules`.
    private var capsuleIndex: [CapsuleInfo] = []

    /// True when Vitreo was started only to run a file opened from Finder.
    ///
    /// Set before any window can appear, because SwiftUI builds the
    /// `WindowGroup` window during launch regardless of whether it is shown —
    /// without this the view's appearance would still kick off the full capsule
    /// scan for a UI that is about to be closed.
    var isLauncherMode = false
    private var managedProcesses: [UUID: Process] = [:]
    private var operationAppIds: [UUID: String] = [:]
    private var appStartTimes: [String: Date] = [:]
    private var preferences: [String: AppPreference] = [:]
    private var logPollingTasks: [UUID: Task<Void, Never>] = [:]
    private var logOffsets: [UUID: [String: UInt64]] = [:]

    /// Finder open requests are resolved here rather than in a view, so a cold
    /// launch and a running Manager follow the same path.
    private(set) lazy var externalLaunches = ExternalLaunchCoordinator(
        host: self, ruleStore: ExternalLaunchRuleStore())

    var capsuleRoot: URL {
        fileManager.homeDirectoryForCurrentUser
            .appendingPathComponent(".vitreo/capsules")
    }

    var defaultCapsuleName: String {
        let configured = UserDefaults.standard.string(forKey: "defaultCapsule") ?? "default"
        if capsules.contains(where: { $0.name == configured }) { return configured }
        return capsules.first?.name ?? "default"
    }

    /// Engine shipped beside the app executable. `Contents/MacOS/Vitreo` is
    /// the management app, so the runtime cannot share that name on a
    /// case-insensitive volume.
    private var bundledEnginePath: String {
        Bundle.main.bundleURL
            .appendingPathComponent("Contents/Helpers/vitreo").path
    }

    var vitreoBinaryPath: String {
        // vitrumBinaryOverride and vitruBinaryOverride are settings written
        // by earlier builds. Read them when the current key is unset.
        let override = UserDefaults.standard.string(forKey: "vitreoBinaryOverride")
            ?? UserDefaults.standard.string(forKey: "vitrumBinaryOverride")
            ?? UserDefaults.standard.string(forKey: "vitruBinaryOverride")
            ?? ""
        if !override.isEmpty && fileManager.isExecutableFile(atPath: override) {
            return override
        }
        let developerRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        // The installed app owns the engine. A developer build and a
        // Homebrew copy are used only when that sibling binary is absent.
        let candidates = [
            bundledEnginePath,
            developerRoot.appendingPathComponent("build/src/vitreo").path,
            "/usr/local/bin/vitreo",
            "/opt/homebrew/bin/vitreo",
        ]
        return candidates.first { fileManager.isExecutableFile(atPath: $0) }
            ?? bundledEnginePath
    }

    init() {
        loadPreferences()
        restartAutoRefresh()
    }

    deinit {
        refreshTimer?.invalidate()
        for task in logPollingTasks.values { task.cancel() }
        for process in managedProcesses.values where process.isRunning {
            process.terminate()
        }
    }

    /// Refresh capsule, app, engine, and process state.
    ///
    /// Concurrent callers join the in-flight refresh rather than returning
    /// early. Returning early would be a lie to anyone who awaited this to
    /// *have* fresh state: a Finder open arriving during the startup refresh
    /// would see zero capsules and report that none exist.
    func refresh() async {
        if let inFlight = refreshTask {
            await inFlight.value
            return
        }
        isRefreshing = true
        defer {
            isRefreshing = false
            hasRefreshed = true
        }
        let task = Task { @MainActor [weak self] in
            guard let self else { return }
            await self.performRefresh()
        }
        refreshTask = task
        await task.value
        refreshTask = nil
    }

    private func performRefresh() async {
        let root = capsuleRoot
        let snapshot = await Task.detached(priority: .utility) {
            CapsuleScanner().scan(root: root)
        }.value
        capsules = snapshot.capsules
        apps = snapshot.apps.map(applyingPreference)

        async let status = cli.engineStatus(binary: vitreoBinaryPath)
        async let processes = cli.listProcesses(
            binary: vitreoBinaryPath, capsules: snapshot.capsules.map(\.name))
        engineStatus = await status
        guestProcesses = await processes
        rebuildRuntimeState()
    }

    func restartAutoRefresh() {
        refreshTimer?.invalidate()
        let configured = UserDefaults.standard.double(forKey: "refreshInterval")
        let interval = configured > 0 ? configured : 5
        refreshTimer = Timer.scheduledTimer(
            withTimeInterval: interval, repeats: true
        ) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self, !self.isLauncherMode, self.hasVisibleUI else {
                    return
                }
                await self.refresh()
            }
        }
    }

    /// A Finder open leaves the Manager running with no window at all. Rescanning
    /// every capsule on a timer — which walks whole installed-game trees to size
    /// them — for a UI nobody is looking at would burn a core indefinitely, so
    /// polling pauses until a window is on screen again.
    private var hasVisibleUI: Bool {
        NSApplication.shared.windows.contains {
            $0.isVisible && !ExternalLaunchWindowPresenter.isChooserWindow($0)
        }
    }

    private func rebuildRuntimeState() {
        let runningPaths = Set(guestProcesses.map {
            normalizeWindowsPath($0.imagePath)
        })
        for index in apps.indices {
            let app = apps[index]
            let direct = runningPaths.contains(
                normalizeWindowsPath(app.windowsExecutablePath))
            let byName = guestProcesses.contains {
                $0.capsule == app.capsuleName &&
                $0.imageName.caseInsensitiveCompare(
                    URL(fileURLWithPath: app.executablePath).lastPathComponent) == .orderedSame
            }
            apps[index].status = direct || byName ? .running : .stopped
        }

        runningAppInfos = guestProcesses.compactMap { process in
            guard let app = apps.first(where: {
                $0.capsuleName == process.capsule &&
                (normalizeWindowsPath($0.windowsExecutablePath) ==
                    normalizeWindowsPath(process.imagePath) ||
                 URL(fileURLWithPath: $0.executablePath).lastPathComponent
                    .caseInsensitiveCompare(process.imageName) == .orderedSame)
            }) else { return nil }
            return RunningAppInfo(
                id: process.id,
                app: app,
                startedAt: appStartTimes[app.id] ?? Date(),
                pid: pid_t(process.pid),
                parentPid: pid_t(process.parentPid))
        }
    }

    // MARK: - App preferences

    func updateLaunchProfile(_ app: InstalledApp, profile: LaunchProfile) {
        var preference = preferences[app.id] ?? AppPreference()
        preference.launchProfile = profile
        preferences[app.id] = preference
        savePreferences()
        if let index = apps.firstIndex(where: { $0.id == app.id }) {
            apps[index].launchProfile = profile
        }
    }

    func setCompatibility(_ app: InstalledApp, status: CompatibilityStatus) {
        var preference = preferences[app.id] ?? AppPreference()
        preference.compatibility = status
        preferences[app.id] = preference
        savePreferences()
        if let index = apps.firstIndex(where: { $0.id == app.id }) {
            apps[index].compatibility = status
        }
    }

    func toggleFavorite(_ app: InstalledApp) {
        var preference = preferences[app.id] ?? AppPreference()
        preference.isFavorite.toggle()
        preferences[app.id] = preference
        savePreferences()
        if let index = apps.firstIndex(where: { $0.id == app.id }) {
            apps[index].isFavorite = preference.isFavorite
        }
    }

    private func applyingPreference(_ app: InstalledApp) -> InstalledApp {
        guard let preference = preferences[app.id] else { return app }
        var updated = app
        updated.launchProfile = preference.launchProfile
        updated.compatibility = preference.compatibility
        updated.isFavorite = preference.isFavorite
        return updated
    }

    private func loadPreferences() {
        guard let data = UserDefaults.standard.data(forKey: "appPreferences"),
              let decoded = try? JSONDecoder().decode(
                [String: AppPreference].self, from: data) else { return }
        preferences = decoded
    }

    private func savePreferences() {
        guard let data = try? JSONEncoder().encode(preferences) else { return }
        UserDefaults.standard.set(data, forKey: "appPreferences")
    }

    // MARK: - Launch and operation lifecycle

    func launchApp(_ app: InstalledApp) {
        let globalInterpreter = UserDefaults.standard.bool(forKey: "useInterpreterMode")
        let globalTrace = UserDefaults.standard.bool(forKey: "enableTrace")
        var arguments = ["run", "--capsule", app.capsuleName, "--app-name", app.name]
        if globalInterpreter || app.launchProfile.forceInterpreter {
            arguments.append("--interpreter")
        }
        if globalTrace || app.launchProfile.enableTrace { arguments.append("--trace") }
        if !app.launchProfile.workingDirectory.isEmpty {
            arguments += ["--guest-cwd", app.launchProfile.workingDirectory]
        }
        for (name, value) in app.launchProfile.environment.sorted(by: { $0.key < $1.key }) {
            arguments += ["--env", "\(name)=\(value)"]
        }
        arguments.append(app.executablePath)
        if !app.launchProfile.arguments.isEmpty {
            arguments.append("--")
            arguments.append(contentsOf: app.launchProfile.arguments)
        }

        guard let operation = startManagedProcess(
            kind: .launch,
            title: app.name,
            capsule: app.capsuleName,
            arguments: arguments,
            appId: app.id
        ) else { return }
        appStartTimes[app.id] = Date()
        operationAppIds[operation] = app.id
        if let index = apps.firstIndex(where: { $0.id == app.id }) {
            apps[index].status = .running
        }
    }

    /// Run an executable that Finder handed to Vitreo.
    ///
    /// This is deliberately not `startInstaller`: opening an `.exe` is no
    /// evidence that an installation was intended, and mislabelling it would
    /// misreport the operation in Activity. No launch profile is applied
    /// either — an arbitrary external file has no stored arguments,
    /// environment, or working directory.
    @discardableResult
    func launchExternalExecutable(
        _ executableURL: URL, capsuleName: String
    ) -> UUID? {
        // Capsule health is rechecked here, not only where it was chosen: the
        // chooser may have been on screen while the capsule was renamed,
        // exported, or trashed.
        guard healthyCapsules.contains(where: { $0.name == capsuleName }) else {
            lastError = "The capsule “\(capsuleName)” is missing or incomplete."
            return nil
        }

        let appName = executableURL.deletingPathExtension().lastPathComponent
        var arguments = ["run", "--capsule", capsuleName, "--app-name", appName]
        if UserDefaults.standard.bool(forKey: "useInterpreterMode") {
            arguments.append("--interpreter")
        }
        if UserDefaults.standard.bool(forKey: "enableTrace") {
            arguments.append("--trace")
        }
        arguments.append(executableURL.path)

        return startManagedProcess(
            kind: .launch,
            title: appName,
            capsule: capsuleName,
            arguments: arguments)
    }

    @discardableResult
    func startInstaller(
        exeURL: URL,
        capsuleName: String,
        arguments installerArguments: [String] = []
    ) -> UUID? {
        var arguments = [
            "run", "--capsule", capsuleName,
            "--app-name", exeURL.deletingPathExtension().lastPathComponent,
            exeURL.path,
        ]
        if !installerArguments.isEmpty {
            arguments.append("--")
            arguments.append(contentsOf: installerArguments)
        }
        return startManagedProcess(
            kind: .install,
            title: "Install \(exeURL.lastPathComponent)",
            capsule: capsuleName,
            arguments: arguments)
    }

    @discardableResult
    func uninstallApp(_ app: InstalledApp) -> UUID? {
        guard let command = app.uninstallInfo?.uninstallString,
              let parsed = parseWindowsCommand(command),
              let hostExecutable = windowsPathToHost(
                parsed.executable, capsuleName: app.capsuleName)
        else {
            lastError = "The application does not provide a usable uninstall command."
            return nil
        }
        var arguments = [
            "run", "--capsule", app.capsuleName,
            "--app-name", "Uninstall \(app.name)", hostExecutable,
        ]
        if !parsed.arguments.isEmpty {
            arguments.append("--")
            arguments.append(contentsOf: parsed.arguments)
        }
        return startManagedProcess(
            kind: .uninstall,
            title: "Uninstall \(app.name)",
            capsule: app.capsuleName,
            arguments: arguments)
    }

    private func startManagedProcess(
        kind: OperationRecord.Kind,
        title: String,
        capsule: String,
        arguments: [String],
        appId: String? = nil
    ) -> UUID? {
        guard fileManager.isExecutableFile(atPath: vitreoBinaryPath) else {
            lastError = "Vitreo engine not found. Select a valid binary in Settings."
            return nil
        }

        let id = UUID()
        let logDirectory = makeLogDirectory(
            capsule: capsule, operation: id, title: title)
        let process = Process()
        let pipe = Pipe()
        let resultFile = logDirectory.appendingPathComponent("result-code.txt")
        process.executableURL = URL(fileURLWithPath: vitreoBinaryPath)
        process.arguments = arguments
        process.standardOutput = pipe
        process.standardError = pipe
        process.environment = ProcessInfo.processInfo.environment.merging(
            [
                "VITREO_LOG_DIR": logDirectory.path,
                "VITREO_RESULT_FILE": resultFile.path,
            ],
            uniquingKeysWith: { _, override in override })

        operations.insert(OperationRecord(
            id: id, kind: kind, title: title, capsule: capsule,
            startedAt: Date(), state: .running, output: "",
            logDirectory: logDirectory.path), at: 0)
        if let appId { operationAppIds[id] = appId }

        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else {
                // Empty means the writer closed. Foundation keeps reporting the
                // descriptor readable forever at EOF, so leaving the handler
                // installed spins a core on empty reads until the app quits.
                handle.readabilityHandler = nil
                return
            }
            guard let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor [weak self] in self?.appendOutput(text, operation: id) }
        }
        process.terminationHandler = { [weak self] process in
            pipe.fileHandleForReading.readabilityHandler = nil
            let remainder = pipe.fileHandleForReading.readDataToEndOfFile()
            Task { @MainActor [weak self] in
                guard let self else { return }
                if let text = String(data: remainder, encoding: .utf8), !text.isEmpty {
                    self.appendOutput(text, operation: id)
                }
                await self.drainOperationLogs(
                    operation: id, directory: logDirectory)
                self.finishOperation(
                    id,
                    exitCode: Self.readEngineResult(
                        from: resultFile,
                        fallback: process.terminationStatus))
                await self.refresh()
            }
        }

        do {
            try process.run()
            managedProcesses[id] = process
            startLogPolling(operation: id, directory: logDirectory)
            return id
        } catch {
            pipe.fileHandleForReading.readabilityHandler = nil
            finishOperation(id, exitCode: -1, launchError: error.localizedDescription)
            return nil
        }
    }

    private func appendOutput(_ text: String, operation id: UUID) {
        guard let index = operations.firstIndex(where: { $0.id == id }) else { return }
        operations[index].output += text
        if operations[index].output.count > 500_000 {
            operations[index].output.removeFirst(operations[index].output.count - 500_000)
        }
    }

    private func startLogPolling(operation id: UUID, directory: URL) {
        logOffsets[id] = [:]
        logPollingTasks[id] = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(500))
                guard let self else { return }
                await self.drainOperationLogs(operation: id, directory: directory)
                if self.managedProcesses[id] == nil { return }
            }
        }
    }

    private func drainOperationLogs(operation id: UUID, directory: URL) async {
        let knownOffsets = logOffsets[id] ?? [:]
        let chunks = await Task.detached(priority: .utility) {
            let fileManager = FileManager.default
            guard let files = try? fileManager.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: [.fileSizeKey],
                options: [.skipsHiddenFiles])
            else { return [(String, UInt64, String)]() }

            return files
                .filter { $0.pathExtension == "log" }
                .sorted { $0.lastPathComponent < $1.lastPathComponent }
                .compactMap { file -> (String, UInt64, String)? in
                    let offset = knownOffsets[file.path] ?? 0
                    guard let handle = try? FileHandle(forReadingFrom: file) else {
                        return nil
                    }
                    defer { try? handle.close() }
                    do {
                        try handle.seek(toOffset: offset)
                        let data = try handle.readToEnd() ?? Data()
                        let next = try handle.offset()
                        guard !data.isEmpty else {
                            return (file.path, next, "")
                        }
                        let text = String(decoding: data, as: UTF8.self)
                        return (
                            file.path, next,
                            "[\(file.lastPathComponent)]\n\(text)")
                    } catch {
                        return nil
                    }
                }
        }.value

        for (path, offset, text) in chunks {
            logOffsets[id, default: [:]][path] = offset
            if !text.isEmpty { appendOutput(text, operation: id) }
        }
    }

    private func finishOperation(
        _ id: UUID, exitCode: Int32, launchError: String? = nil
    ) {
        logPollingTasks.removeValue(forKey: id)?.cancel()
        logOffsets.removeValue(forKey: id)
        managedProcesses.removeValue(forKey: id)
        guard let index = operations.firstIndex(where: { $0.id == id }) else { return }
        operations[index].finishedAt = Date()
        operations[index].exitCode = exitCode
        if operations[index].state != .cancelled {
            operations[index].state = exitCode == 0 ? .succeeded : .failed
        }
        if let launchError {
            operations[index].output += "\n\(launchError)\n"
            lastError = launchError
        } else if exitCode != 0 {
            lastError = "\(operations[index].title) exited with code "
                + "\(Self.formatWindowsExitCode(exitCode))."
        }
        if let appId = operationAppIds.removeValue(forKey: id) {
            appStartTimes.removeValue(forKey: appId)
        }
    }

    /// Read the complete 32-bit Windows result emitted by the engine. A native
    /// process status is only an eight-bit fallback and cannot carry NTSTATUS.
    static func readEngineResult(from url: URL, fallback: Int32) -> Int32 {
        guard let text = try? String(contentsOf: url, encoding: .utf8),
              let value = UInt32(text.trimmingCharacters(in: .whitespacesAndNewlines))
        else { return fallback }
        return Int32(bitPattern: value)
    }

    static func formatWindowsExitCode(_ value: Int32) -> String {
        if value < 0 {
            return String(format: "0x%08X", UInt32(bitPattern: value))
        }
        return String(value)
    }

    func cancelOperation(_ id: UUID) {
        guard let process = managedProcesses[id], process.isRunning else { return }
        if let index = operations.firstIndex(where: { $0.id == id }) {
            operations[index].state = .cancelled
        }
        process.terminate()
    }

    func stopApp(_ app: InstalledApp) {
        for (operation, appId) in operationAppIds where appId == app.id {
            cancelOperation(operation)
        }
        let matching = guestProcesses.filter {
            $0.capsule == app.capsuleName &&
            (normalizeWindowsPath($0.imagePath) ==
                normalizeWindowsPath(app.windowsExecutablePath) ||
             $0.imageName.caseInsensitiveCompare(
                URL(fileURLWithPath: app.executablePath).lastPathComponent) == .orderedSame)
        }
        for process in matching {
            Task {
                let result = try? await cli.run(
                    binary: vitreoBinaryPath,
                    arguments: [
                        "process", "stop", String(process.pid),
                        "--capsule", process.capsule,
                    ])
                if let result, result.exitCode != 0 {
                    lastError = result.combinedOutput
                }
                await refresh()
            }
        }
    }

    func stopProcess(_ process: GuestProcessInfo) {
        Task {
            do {
                let result = try await cli.run(
                    binary: vitreoBinaryPath,
                    arguments: [
                        "process", "stop", String(process.pid),
                        "--capsule", process.capsule,
                    ])
                if result.exitCode != 0 { lastError = result.combinedOutput }
            } catch {
                lastError = error.localizedDescription
            }
            await refresh()
        }
    }

    func stopAll(capsule: String? = nil) {
        for id in managedProcesses.keys { cancelOperation(id) }
        let targets = capsule.map { [$0] } ?? capsules.map(\.name)
        Task {
            for target in targets {
                _ = try? await cli.run(
                    binary: vitreoBinaryPath,
                    arguments: ["process", "stop-all", "--capsule", target])
            }
            await refresh()
        }
    }

    // MARK: - Capsule lifecycle

    func createCapsule(name: String) async -> String? {
        await capsuleCommand(["capsule", "create", name])
    }

    func renameCapsule(_ capsule: CapsuleInfo, to name: String) async -> String? {
        guard !guestProcesses.contains(where: { $0.capsule == capsule.name }) else {
            return "Stop all processes in this capsule before renaming it."
        }
        let error = await capsuleCommand(["capsule", "rename", capsule.name, name])
        if error == nil,
           UserDefaults.standard.string(forKey: "defaultCapsule") == capsule.name {
            UserDefaults.standard.set(name, forKey: "defaultCapsule")
        }
        return error
    }

    func cloneCapsule(_ capsule: CapsuleInfo, to name: String) async -> String? {
        guard !guestProcesses.contains(where: { $0.capsule == capsule.name }) else {
            return "Stop all processes in this capsule before cloning it."
        }
        return await capsuleCommand(["capsule", "clone", capsule.name, name])
    }

    func exportCapsule(_ capsule: CapsuleInfo, to destination: URL) async -> String? {
        guard !guestProcesses.contains(where: { $0.capsule == capsule.name }) else {
            return "Stop all processes in this capsule before exporting it."
        }
        return await capsuleCommand([
            "capsule", "export", capsule.name, destination.path,
        ])
    }

    func importCapsule(from source: URL, name: String) async -> String? {
        await capsuleCommand(["capsule", "import", source.path, name])
    }

    func validateCapsule(_ capsule: CapsuleInfo) async -> String? {
        do {
            let result = try await cli.run(
                binary: vitreoBinaryPath,
                arguments: ["capsule", "validate", capsule.name])
            return result.exitCode == 0 ? nil : result.combinedOutput
        } catch {
            return error.localizedDescription
        }
    }

    private func capsuleCommand(_ arguments: [String]) async -> String? {
        do {
            let result = try await cli.run(
                binary: vitreoBinaryPath, arguments: arguments)
            if result.exitCode != 0 { return result.combinedOutput }
            await refresh()
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    func removeCapsule(_ capsule: CapsuleInfo) async -> String? {
        guard !guestProcesses.contains(where: { $0.capsule == capsule.name }) else {
            return "Stop all processes in this capsule before removing it."
        }
        return await capsuleCommand(["capsule", "trash", capsule.name])
    }

    func mountImage(
        _ image: URL, in capsule: CapsuleInfo, drive: String? = nil
    ) async -> String? {
        var arguments = ["mount", image.path, "--capsule", capsule.name]
        if let drive, !drive.isEmpty {
            arguments += ["--drive", drive]
        }
        return await capsuleCommand(arguments)
    }

    func unmount(_ mount: MountedVolume, in capsule: CapsuleInfo) async -> String? {
        await capsuleCommand([
            "unmount", mount.drive, "--capsule", capsule.name,
        ])
    }

    // MARK: - Portable apps and removal

    func addPortableApp(
        folderURL: URL,
        mainExe: String,
        appName: String,
        capsuleName: String,
        replaceExisting: Bool = false
    ) throws {
        let folderName = folderURL.lastPathComponent
        let destination = capsuleRoot
            .appendingPathComponent(capsuleName)
            .appendingPathComponent("drives/c/Vitreo/Apps")
            .appendingPathComponent(folderName)
        if fileManager.fileExists(atPath: destination.path) {
            guard replaceExisting else {
                throw CocoaError(.fileWriteFileExists)
            }
            try fileManager.trashItem(at: destination, resultingItemURL: nil)
        }
        try fileManager.createDirectory(
            at: destination.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try fileManager.copyItem(at: folderURL, to: destination)

        let relativeExe = mainExe.replacingOccurrences(of: "/", with: "\\")
        let windowsRoot = "C:\\Vitreo\\Apps\\\(folderName)"
        let desktop = capsuleRoot
            .appendingPathComponent(capsuleName)
            .appendingPathComponent("drives/c/Users/Default/Desktop")
        try fileManager.createDirectory(at: desktop, withIntermediateDirectories: true)
        let payload = """
        Vitreo ShellLink
        Target=\(windowsRoot)\\\(relativeExe)
        Args=
        WorkingDir=\(windowsRoot)

        """
        try payload.write(
            to: desktop.appendingPathComponent("\(appName).lnk"),
            atomically: true, encoding: .utf8)
        Task { await refresh() }
    }

    func removeApp(_ app: InstalledApp) -> String? {
        guard app.status == .stopped else { return "Stop the application before removing it." }
        do {
            if app.isPortable {
                let marker = "/drives/c/Vitreo/Apps/"
                guard let range = app.executablePath.range(
                    of: marker, options: [.caseInsensitive]) else {
                    return "Portable application path is invalid."
                }
                let suffix = app.executablePath[range.upperBound...]
                guard let first = suffix.split(separator: "/").first else {
                    return "Portable application path is invalid."
                }
                let folder = String(app.executablePath[..<range.upperBound])
                    + first
                try fileManager.trashItem(
                    at: URL(fileURLWithPath: folder), resultingItemURL: nil)
            }
            if fileManager.fileExists(atPath: app.lnkPath) {
                try fileManager.trashItem(
                    at: URL(fileURLWithPath: app.lnkPath), resultingItemURL: nil)
            }
            apps.removeAll { $0.id == app.id }
            preferences.removeValue(forKey: app.id)
            savePreferences()
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    // MARK: - Utilities

    func revealInFinder(_ app: InstalledApp) {
        NSWorkspace.shared.selectFile(
            app.executablePath,
            inFileViewerRootedAtPath:
                URL(fileURLWithPath: app.executablePath)
                    .deletingLastPathComponent().path)
    }

    func revealCapsuleInFinder(_ capsule: CapsuleInfo) {
        NSWorkspace.shared.selectFile(
            nil, inFileViewerRootedAtPath: capsule.rootPath)
    }

    func revealOperationLogs(_ operation: OperationRecord) {
        guard let path = operation.logDirectory else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: path))
    }

    func exportDiagnostics(to destination: URL) throws {
        let folder = destination.appendingPathComponent(
            "Vitreo-Diagnostics-\(diagnosticTimestamp())")
        try fileManager.createDirectory(at: folder, withIntermediateDirectories: true)
        let summary: [String: Any] = [
            "generated_at": ISO8601DateFormatter().string(from: Date()),
            "manager_version": Bundle.main.object(
                forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown",
            "engine_path": engineStatus.path,
            "engine_version": engineStatus.version,
            "capsules": capsules.map {
                [
                    "name": $0.name,
                    "healthy": $0.isHealthy,
                    "size_bytes": $0.sizeBytes,
                    "mounts": $0.mounts.map {
                        ["drive": $0.drive, "source": $0.sourcePath]
                    },
                ]
            },
            "processes": guestProcesses.map {
                [
                    "pid": $0.pid,
                    "parent_pid": $0.parentPid,
                    "image_name": $0.imageName,
                    "image_path": $0.imagePath,
                    "capsule": $0.capsule,
                ]
            },
        ]
        let data = try JSONSerialization.data(
            withJSONObject: summary, options: [.prettyPrinted, .sortedKeys])
        try data.write(to: folder.appendingPathComponent("summary.json"))
        for operation in operations.prefix(20) {
            let safeName = operation.title
                .replacingOccurrences(of: "/", with: "-")
                .replacingOccurrences(of: ":", with: "-")
            try operation.output.write(
                to: folder.appendingPathComponent(
                    "\(operation.startedAt.timeIntervalSince1970)-\(safeName).log"),
                atomically: true, encoding: .utf8)
        }
        operations.insert(OperationRecord(
            id: UUID(), kind: .diagnostics, title: "Export Diagnostics",
            capsule: "", startedAt: Date(), finishedAt: Date(),
            state: .succeeded, exitCode: 0, output: folder.path), at: 0)
        NSWorkspace.shared.open(folder)
    }

    func createMacLauncher(for app: InstalledApp, in directory: URL) throws {
        let bundle = directory.appendingPathComponent("\(app.name).app")
        let contents = bundle.appendingPathComponent("Contents")
        let macOS = contents.appendingPathComponent("MacOS")
        let resources = contents.appendingPathComponent("Resources")
        try fileManager.createDirectory(at: macOS, withIntermediateDirectories: true)
        try fileManager.createDirectory(
            at: resources, withIntermediateDirectories: true)
        guard let bundledHelper = Bundle.main.url(
            forResource: "VitreoLauncher", withExtension: nil) else {
            throw MacLauncherError.helperMissing
        }
        let executable = macOS.appendingPathComponent("VitreoLauncher")
        if fileManager.fileExists(atPath: executable.path) {
            try fileManager.removeItem(at: executable)
        }
        try fileManager.copyItem(at: bundledHelper, to: executable)
        try fileManager.setAttributes(
            [.posixPermissions: 0o755], ofItemAtPath: executable.path)
        let legacyScript = macOS.appendingPathComponent("launch")
        if fileManager.fileExists(atPath: legacyScript.path) {
            try fileManager.removeItem(at: legacyScript)
        }

        let configuration = MacLauncherConfiguration(
            executable: vitreoBinaryPath,
            arguments: buildLaunchArguments(for: app))
        let configurationData = try JSONEncoder().encode(configuration)
        try configurationData.write(
            to: resources.appendingPathComponent("LauncherConfig.json"),
            options: .atomic)
        var iconPlistEntry = ""
        if let icon = extractPEIcon(
            at: URL(fileURLWithPath: app.executablePath)) {
            try writeMacAppIcon(
                icon, to: resources, fileManager: fileManager)
            iconPlistEntry = """
            <key>CFBundleIconFile</key><string>AppIcon.icns</string>
            """
        }
        let plist = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" \
        "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0"><dict>
        <key>CFBundleExecutable</key><string>VitreoLauncher</string>
        <key>CFBundleIdentifier</key><string>dev.vitreo.launcher.\(stableSlug(app.id))</string>
        <key>CFBundleName</key><string>\(xmlEscape(app.name))</string>
        <key>CFBundleDisplayName</key><string>\(xmlEscape(app.name))</string>
        <key>CFBundlePackageType</key><string>APPL</string>
        \(iconPlistEntry)
        </dict></plist>
        """
        try plist.write(
            to: contents.appendingPathComponent("Info.plist"),
            atomically: true, encoding: .utf8)
    }

    private func makeLogDirectory(
        capsule: String, operation: UUID, title: String
    ) -> URL {
        let url = capsuleRoot.appendingPathComponent(capsule)
            .appendingPathComponent("logs")
            .appendingPathComponent(
                "\(diagnosticTimestamp())-\(stableSlug(title))-\(operation.uuidString.prefix(8))")
        try? fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private func parseWindowsCommand(
        _ command: String
    ) -> (executable: String, arguments: [String])? {
        let values = splitWindowsCommandLine(command)
        guard let executable = values.first else { return nil }
        return (executable, Array(values.dropFirst()))
    }

    private func splitWindowsCommandLine(_ value: String) -> [String] {
        var values: [String] = []
        var current = ""
        var quoted = false
        for character in value {
            if character == "\"" {
                quoted.toggle()
            } else if character.isWhitespace && !quoted {
                if !current.isEmpty {
                    values.append(current)
                    current = ""
                }
            } else {
                current.append(character)
            }
        }
        if !current.isEmpty { values.append(current) }
        return values
    }

    private func windowsPathToHost(
        _ windowsPath: String, capsuleName: String
    ) -> String? {
        let clean = windowsPath
            .trimmingCharacters(in: CharacterSet(charactersIn: "\" "))
        guard clean.count >= 3, clean.prefix(2).uppercased() == "C:" else { return nil }
        let relative = clean.dropFirst(2)
            .replacingOccurrences(of: "\\", with: "/")
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !relative.components(separatedBy: "/").contains("..") else { return nil }
        return capsuleRoot.appendingPathComponent(capsuleName)
            .appendingPathComponent("drives/c")
            .appendingPathComponent(relative).path
    }

    private func normalizeWindowsPath(_ value: String) -> String {
        value.replacingOccurrences(of: "/", with: "\\")
            .trimmingCharacters(in: CharacterSet(charactersIn: "\\\" "))
            .lowercased()
    }

    private func buildLaunchArguments(for app: InstalledApp) -> [String] {
        var values = ["run", "--capsule", app.capsuleName, "--app-name", app.name]
        if app.launchProfile.forceInterpreter { values.append("--interpreter") }
        if app.launchProfile.enableTrace { values.append("--trace") }
        if !app.launchProfile.workingDirectory.isEmpty {
            values += ["--guest-cwd", app.launchProfile.workingDirectory]
        }
        for (name, value) in app.launchProfile.environment.sorted(
            by: { $0.key < $1.key }) {
            values += ["--env", "\(name)=\(value)"]
        }
        values.append(app.executablePath)
        if !app.launchProfile.arguments.isEmpty {
            values.append("--")
            values.append(contentsOf: app.launchProfile.arguments)
        }
        return values
    }

    private func stableSlug(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics
        let slug = value.unicodeScalars.map {
            allowed.contains($0) ? Character(String($0).lowercased()) : "-"
        }
        return String(slug).replacingOccurrences(of: "--", with: "-")
    }

    private func xmlEscape(_ value: String) -> String {
        value.replacingOccurrences(of: "&", with: "&amp;")
            .replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
            .replacingOccurrences(of: "\"", with: "&quot;")
            .replacingOccurrences(of: "'", with: "&apos;")
    }

    private func diagnosticTimestamp() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.string(from: Date())
    }
}

// MARK: - Finder open requests

extension VitreoService: ExternalLaunchHosting {
    var capsuleRootDirectory: URL { capsuleRoot }

    /// Sourced from the cheap probe when one has run, so a Finder open never
    /// waits on — or triggers — the UI's full capsule scan.
    var healthyCapsules: [CapsuleInfo] {
        (capsuleIndex.isEmpty ? capsules : capsuleIndex).filter(\.isHealthy)
    }

    /// The capsule the user configured, without `defaultCapsuleName`'s
    /// fall back to "whatever exists". A Finder open must fall through to the
    /// chooser when the configured default is gone rather than silently
    /// running in an unrelated capsule.
    var configuredDefaultCapsule: String {
        UserDefaults.standard.string(forKey: "defaultCapsule") ?? "default"
    }

    /// True while a `vitreo` process started by this Manager is still running.
    /// Quitting with one alive would cut the guest short, so callers use this
    /// before deciding the app has nothing left to do.
    var hasSupervisedProcesses: Bool {
        managedProcesses.values.contains { $0.isRunning }
    }

    func refreshCapsuleState() async {
        let root = capsuleRoot
        capsuleIndex = await Task.detached(priority: .userInitiated) {
            CapsuleScanner().probeCapsules(root: root)
        }.value
    }

    func reportExternalLaunchFailure(_ message: String) {
        lastError = message
    }
}
