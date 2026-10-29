// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

/// Why a file cannot be treated as a launchable Windows executable.
///
/// These describe what Finder handed us, not whether the program will run.
/// The runtime remains the authoritative PE loader; this check exists so an
/// obviously wrong file fails immediately with a message a user can act on.
enum PEInspectionError: Error, Equatable {
    case notAFileURL
    case missing
    case notARegularFile
    case unexpectedExtension(String)
    case unreadable
    case notAWindowsExecutable
    case truncatedHeader
    case unsupportedMachine(UInt16)

    var message: String {
        switch self {
        case .notAFileURL:
            return "Only local files can be opened with Vitreo."
        case .missing:
            return "The executable no longer exists."
        case .notARegularFile:
            return "That item is not a regular file."
        case .unexpectedExtension(let ext):
            return ext.isEmpty
                ? "Vitreo opens Windows programs and installer packages."
                : "Vitreo opens Windows programs and installer packages, not .\(ext) files."
        case .unreadable:
            return "The executable could not be read."
        case .notAWindowsExecutable:
            return "That file is not a valid Windows PE executable."
        case .truncatedHeader:
            return "The executable's PE header is incomplete or damaged."
        case .unsupportedMachine(let machine):
            return String(
                format:
                    "Vitreo runs 32-bit and 64-bit x86 executables. "
                    + "This one targets machine type 0x%04X.",
                machine)
        }
    }
}

/// A file that passed inspection.
struct PEInspection: Equatable {
    /// Symlinks resolved and `.`/`..` removed, so capsule classification and
    /// remembered rules always key off one spelling of the path.
    let canonicalURL: URL
    let architecture: InstalledApp.Architecture
}

/// Read-only Windows PE header inspection.
///
/// Reads only the handful of bytes needed to answer "is this an x86 or x64
/// Windows executable?" — a multi-gigabyte installer costs the same as a
/// 4 KB stub.
enum PEInspector {
    private static let machineI386: UInt16 = 0x014C
    private static let machineAMD64: UInt16 = 0x8664

    /// Extensions Explorer treats as installer packages. `.msi` and `.msp`
    /// are Windows Installer packages; the rest are merge modules, transforms,
    /// MSIX/AppX packages, or Windows Update packages.
    private static let installerExtensions: Set<String> = [
        "msi", "msp", "mst", "msm",
        "msix", "msixbundle", "appx", "appxbundle", "appinstaller",
        "msu",
    ]

    /// Validate a file Finder handed us and report what it is.
    static func inspect(url: URL) -> Result<PEInspection, PEInspectionError> {
        guard url.isFileURL else { return .failure(.notAFileURL) }

        let canonical = canonicalize(url)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(
            atPath: canonical.path, isDirectory: &isDirectory) else {
            return .failure(.missing)
        }
        if isDirectory.boolValue { return .failure(.notARegularFile) }

        let attributes = try? FileManager.default.attributesOfItem(
            atPath: canonical.path)
        if let type = attributes?[.type] as? FileAttributeType,
           type != .typeRegular {
            return .failure(.notARegularFile)
        }

        let ext = canonical.pathExtension
        if Self.installerExtensions.contains(ext.lowercased()) {
            // Installer packages are not PE images. The runtime opens an .msi
            // with msiexec /i and an .msp with msiexec /p, and reports the
            // other installer types as unimplemented.
            return .success(PEInspection(
                canonicalURL: canonical, architecture: .unknown))
        }
        guard ext.caseInsensitiveCompare("exe") == .orderedSame else {
            return .failure(.unexpectedExtension(ext.lowercased()))
        }

        return readMachine(at: canonical).map {
            PEInspection(canonicalURL: canonical, architecture: $0)
        }
    }

    /// Best-effort architecture for display purposes; never reports a reason.
    static func architecture(at url: URL) -> InstalledApp.Architecture {
        switch readMachine(at: canonicalize(url)) {
        case .success(let architecture): return architecture
        case .failure: return .unknown
        }
    }

    static func canonicalize(_ url: URL) -> URL {
        URL(fileURLWithPath: url.resolvingSymlinksInPath().path)
            .standardizedFileURL
    }

    private static func readMachine(
        at url: URL
    ) -> Result<InstalledApp.Architecture, PEInspectionError> {
        guard let handle = FileHandle(forReadingAtPath: url.path) else {
            return .failure(.unreadable)
        }
        defer { try? handle.close() }

        guard let dosHeader = try? handle.read(upToCount: 0x40),
              dosHeader.count == 0x40 else {
            return .failure(.notAWindowsExecutable)
        }
        guard dosHeader[0] == 0x4D, dosHeader[1] == 0x5A else {  // "MZ"
            return .failure(.notAWindowsExecutable)
        }

        let peOffset = dosHeader.loadLE32(at: 0x3C)
        // The COFF machine field sits at e_lfanew + 4 (signature) + 0.
        guard peOffset >= 0x40, peOffset < 0x1000_0000,
              let fileSize = try? handle.seekToEnd(),
              UInt64(peOffset) + 6 <= fileSize else {
            return .failure(.truncatedHeader)
        }

        try? handle.seek(toOffset: UInt64(peOffset))
        guard let coff = try? handle.read(upToCount: 6), coff.count == 6 else {
            return .failure(.truncatedHeader)
        }
        guard coff[coff.startIndex] == 0x50,          // "P"
              coff[coff.startIndex + 1] == 0x45,      // "E"
              coff[coff.startIndex + 2] == 0x00,
              coff[coff.startIndex + 3] == 0x00 else {
            return .failure(.notAWindowsExecutable)
        }

        let machine = coff.loadLE16(at: coff.startIndex + 4)
        switch machine {
        case machineI386: return .success(.x86)
        case machineAMD64: return .success(.x64)
        default: return .failure(.unsupportedMachine(machine))
        }
    }
}

private extension Data {
    func loadLE16(at offset: Int) -> UInt16 {
        guard offset + 1 < endIndex else { return 0 }
        return UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func loadLE32(at offset: Int) -> UInt32 {
        guard offset + 3 < endIndex else { return 0 }
        return UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }
}
