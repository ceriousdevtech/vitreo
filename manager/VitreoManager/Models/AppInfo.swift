// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

struct LaunchProfile: Codable, Hashable {
    var arguments: [String] = []
    var workingDirectory: String = ""
    var environment: [String: String] = [:]
    var forceInterpreter = false
    var enableTrace = false
}

enum CompatibilityStatus: String, Codable, CaseIterable, Identifiable {
    case untested = "Untested"
    case installs = "Installs"
    case launches = "Launches"
    case partial = "Partially Working"
    case stable = "Stable"

    var id: String { rawValue }
}

/// A launchable Windows shortcut or Manager-created launch profile.
struct InstalledApp: Identifiable, Hashable {
    let id: String
    let name: String
    let capsuleName: String
    let architecture: Architecture
    var status: AppStatus
    let executablePath: String
    let windowsExecutablePath: String
    let lnkPath: String
    var launchProfile: LaunchProfile
    var compatibility: CompatibilityStatus
    var isFavorite: Bool
    let uninstallInfo: UninstallInfo?

    var isPortable: Bool {
        executablePath.localizedCaseInsensitiveContains("/Vitreo/Apps/")
    }

    struct UninstallInfo: Hashable {
        let displayName: String
        let uninstallString: String
        let quietUninstallString: String?
    }

    enum Architecture: String, Codable {
        case x86 = "32-bit"
        case x64 = "64-bit"
        case unknown = "Unknown"
    }

    enum AppStatus: String {
        case running = "Running"
        case stopped = "Stopped"
    }

    init(
        name: String,
        capsuleName: String,
        architecture: Architecture = .unknown,
        status: AppStatus = .stopped,
        executablePath: String,
        windowsExecutablePath: String = "",
        lnkPath: String,
        launchProfile: LaunchProfile = LaunchProfile(),
        compatibility: CompatibilityStatus = .untested,
        isFavorite: Bool = false,
        uninstallInfo: UninstallInfo? = nil
    ) {
        self.id = "\(capsuleName.lowercased())|\(lnkPath.lowercased())"
        self.name = name
        self.capsuleName = capsuleName
        self.architecture = architecture
        self.status = status
        self.executablePath = executablePath
        self.windowsExecutablePath = windowsExecutablePath
        self.lnkPath = lnkPath
        self.launchProfile = launchProfile
        self.compatibility = compatibility
        self.isFavorite = isFavorite
        self.uninstallInfo = uninstallInfo
    }
}

struct MountedVolume: Identifiable, Hashable {
    let drive: String
    let sourcePath: String
    var id: String { drive }
}

struct CapsuleInfo: Identifiable, Hashable {
    let name: String
    let rootPath: String
    let hasCDrive: Bool
    let hasRegistry: Bool
    let sizeBytes: UInt64
    let modifiedAt: Date?
    let mounts: [MountedVolume]

    var id: String { name.lowercased() }
    var isHealthy: Bool { hasCDrive && hasRegistry }
}

struct GuestProcessInfo: Identifiable, Codable, Hashable {
    let pid: Int32
    let parentPid: Int32
    let imageName: String
    let imagePath: String
    let capsule: String

    var id: String { "\(capsule.lowercased())|\(pid)" }

    enum CodingKeys: String, CodingKey {
        case pid
        case parentPid = "parent_pid"
        case imageName = "image_name"
        case imagePath = "image_path"
        case capsule
    }
}

struct OperationRecord: Identifiable, Hashable {
    enum Kind: String {
        case launch = "Launch"
        case install = "Install"
        case uninstall = "Uninstall"
        case capsule = "Capsule"
        case diagnostics = "Diagnostics"
    }

    enum State: String {
        case running = "Running"
        case succeeded = "Succeeded"
        case failed = "Failed"
        case cancelled = "Cancelled"
    }

    let id: UUID
    let kind: Kind
    let title: String
    let capsule: String
    let startedAt: Date
    var finishedAt: Date?
    var state: State
    var exitCode: Int32?
    var output: String
    var logDirectory: String?
}

struct EngineStatus: Equatable {
    enum State {
        case ready
        case missing
        case incompatible
    }

    let state: State
    let path: String
    let version: String
    let message: String
}
