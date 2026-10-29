// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import Foundation
import XCTest
@testable import VitreoManager

final class CapsuleScannerTests: XCTestCase {
    private var temporaryRoot: URL!

    override func setUpWithError() throws {
        temporaryRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("vitreo-manager-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: temporaryRoot, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: temporaryRoot)
    }

    func testScannerDiscoversLegacyShortcutProfileArchitectureAndMount() throws {
        let capsule = temporaryRoot.appendingPathComponent("demo")
        let executable = capsule.appendingPathComponent(
            "drives/c/Program Files/Demo/demo.exe")
        let desktop = capsule.appendingPathComponent(
            "drives/c/Users/Default/Desktop")
        try FileManager.default.createDirectory(
            at: executable.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: desktop, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: capsule.appendingPathComponent("registry"),
            withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: capsule.appendingPathComponent("mounts"),
            withIntermediateDirectories: true)

        // A minimal but genuine PE32: PEInspector requires both signatures, so
        // an arbitrary file with a plausible machine word is not accepted.
        var pe = Data(repeating: 0, count: 0x100)
        pe[0x00] = 0x4d  // 'M'
        pe[0x01] = 0x5a  // 'Z'
        pe[0x3c] = 0x80  // e_lfanew
        pe[0x80] = 0x50  // 'P'
        pe[0x81] = 0x45  // 'E'
        pe[0x84] = 0x4c  // IMAGE_FILE_MACHINE_I386
        pe[0x85] = 0x01
        try pe.write(to: executable)
        try """
        Vitreo ShellLink
        Target=C:\\Program Files\\Demo\\demo.exe
        Args=--windowed "two words"
        WorkingDir=C:\\Program Files\\Demo

        """.write(
            to: desktop.appendingPathComponent("Demo.lnk"),
            atomically: true, encoding: .utf8)
        try "/tmp/demo.iso\n".write(
            to: capsule.appendingPathComponent("mounts/d.mount"),
            atomically: true, encoding: .utf8)

        let snapshot = CapsuleScanner().scan(root: temporaryRoot)

        XCTAssertEqual(snapshot.capsules.count, 1)
        XCTAssertEqual(snapshot.capsules[0].mounts.first?.drive, "D:")
        XCTAssertEqual(snapshot.apps.count, 1)
        XCTAssertEqual(snapshot.apps[0].name, "Demo")
        XCTAssertEqual(snapshot.apps[0].architecture, .x86)
        XCTAssertEqual(
            snapshot.apps[0].launchProfile.arguments,
            ["--windowed", "two words"])
        XCTAssertEqual(
            snapshot.apps[0].launchProfile.workingDirectory,
            "C:\\Program Files\\Demo")
    }

    func testScannerDiscoversShellLinkWhoseTargetIsInStringData() throws {
        let capsule = temporaryRoot.appendingPathComponent("steam")
        let executable = capsule.appendingPathComponent(
            "drives/c/Program Files/Steam/steam.exe")
        let desktop = capsule.appendingPathComponent(
            "drives/c/Users/Public/Desktop")
        try FileManager.default.createDirectory(
            at: executable.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: desktop, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(
            at: capsule.appendingPathComponent("registry"),
            withIntermediateDirectories: true)
        try Data([0x4D, 0x5A]).write(to: executable)

        // LinkFlags 0x8C: HasName, HasRelativePath, IsUnicode. No LinkInfo.
        // The name string comes first; the target is the relative-path string.
        var lnk = Data(repeating: 0, count: 76)
        lnk[0] = 0x4C
        let clsid: [UInt8] = [
            0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
        ]
        lnk.replaceSubrange(4..<20, with: clsid)
        lnk[20] = 0x8C
        func appendCounted(_ value: String) {
            var count = UInt16(value.utf16.count)
            lnk.append(contentsOf: withUnsafeBytes(of: &count) { Array($0) })
            for unit in value.utf16 {
                var stored = unit.littleEndian
                lnk.append(contentsOf: withUnsafeBytes(of: &stored) { Array($0) })
            }
        }
        appendCounted("Steam")
        appendCounted("C:\\Program Files\\Steam\\steam.exe")
        try lnk.write(to: desktop.appendingPathComponent("Steam.lnk"))

        let snapshot = CapsuleScanner().scan(root: temporaryRoot)

        XCTAssertEqual(snapshot.apps.map(\.name), ["Steam"])
        XCTAssertEqual(
            snapshot.apps[0].windowsExecutablePath,
            "C:\\Program Files\\Steam\\steam.exe")
    }

    func testScannerListsOneAppPerLaunchTargetAcrossShortcutLocations() throws {
        let capsule = temporaryRoot.appendingPathComponent("steam")
        let cDrive = capsule.appendingPathComponent("drives/c")
        let executable = cDrive.appendingPathComponent(
            "Program Files/Steam/steam.exe")
        try FileManager.default.createDirectory(
            at: executable.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try Data([0x4D, 0x5A]).write(to: executable)

        func writeLink(_ relative: String, target: String, args: String = "") throws {
            let url = cDrive.appendingPathComponent(relative)
            try FileManager.default.createDirectory(
                at: url.deletingLastPathComponent(),
                withIntermediateDirectories: true)
            try """
            Vitreo ShellLink
            Target=\(target)
            Args=\(args)
            WorkingDir=C:\\Program Files\\Steam

            """.write(to: url, atomically: true, encoding: .utf8)
        }
        // The desktop copy differs in name and path spelling only.
        try writeLink(
            "Users/Public/Desktop/Steam Client.lnk",
            target: "C:/Program Files/Steam/steam.exe")
        try writeLink(
            "ProgramData/Microsoft/Windows/Start Menu/Programs/Steam/Steam.lnk",
            target: "C:\\Program Files\\Steam\\steam.exe")
        try writeLink(
            "Users/Default/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Steam/Steam.lnk",
            target: "c:\\program files\\steam\\STEAM.EXE")
        // Different arguments are a different launch, so it stays.
        try writeLink(
            "Users/Public/Desktop/Steam Big Picture.lnk",
            target: "C:\\Program Files\\Steam\\steam.exe",
            args: "-bigpicture")

        let snapshot = CapsuleScanner().scan(root: temporaryRoot)

        XCTAssertEqual(snapshot.apps.map(\.name), ["Steam", "Steam Big Picture"])
        XCTAssertTrue(snapshot.apps[0].lnkPath.hasSuffix(
            "ProgramData/Microsoft/Windows/Start Menu/Programs/Steam/Steam.lnk"))
        XCTAssertEqual(snapshot.apps[1].launchProfile.arguments, ["-bigpicture"])
    }

    func testPEIconExtractorFindsResourcesBehindSmallerPrecedingSection() throws {
        // A PE32 whose small `.data` section is followed by `.rsrc`, with a
        // resource directory larger than the gap between them: the resource
        // RVA must be resolved through `.rsrc`, not the section before it.
        let bitmap = try XCTUnwrap(NSBitmapImageRep(
            bitmapDataPlanes: nil, pixelsWide: 16, pixelsHigh: 16,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
            isPlanar: false, colorSpaceName: .deviceRGB,
            bytesPerRow: 0, bitsPerPixel: 0))
        let png = try XCTUnwrap(bitmap.representation(using: .png, properties: [:]))
        XCTAssertLessThan(png.count, 0x200)

        var pe = Data(repeating: 0, count: 0xE00)
        func put16(_ offset: Int, _ value: Int) {
            pe[offset] = UInt8(value & 0xFF)
            pe[offset + 1] = UInt8((value >> 8) & 0xFF)
        }
        func put32(_ offset: Int, _ value: Int) {
            put16(offset, value & 0xFFFF)
            put16(offset + 2, (value >> 16) & 0xFFFF)
        }
        pe[0] = 0x4D; pe[1] = 0x5A
        put32(0x3C, 0x40)
        put32(0x40, 0x4550)               // PE\0\0
        put16(0x44, 0x14C)                // i386
        put16(0x46, 2)                    // NumberOfSections
        put16(0x54, 0xE0)                 // SizeOfOptionalHeader
        let optional = 0x58
        put16(optional, 0x10B)            // PE32
        put32(optional + 96 + 16, 0x2000) // DataDirectory[RESOURCE].VirtualAddress
        put32(optional + 96 + 20, 0x900)  // DataDirectory[RESOURCE].Size
        let sections = optional + 0xE0
        func section(_ index: Int, va: Int, virtualSize: Int, rawSize: Int, raw: Int) {
            let s = sections + index * 40
            put32(s + 8, virtualSize)
            put32(s + 12, va)
            put32(s + 16, rawSize)
            put32(s + 20, raw)
        }
        section(0, va: 0x1800, virtualSize: 0x10, rawSize: 0x200, raw: 0x200)
        section(1, va: 0x2000, virtualSize: 0x900, rawSize: 0xA00, raw: 0x400)

        // type → name → language → IMAGE_RESOURCE_DATA_ENTRY, at file 0x400.
        let rsrc = 0x400
        func directory(_ offset: Int, _ entries: [(id: Int, value: Int)]) {
            put16(rsrc + offset + 14, entries.count)
            for (i, entry) in entries.enumerated() {
                put32(rsrc + offset + 16 + i * 8, entry.id)
                put32(rsrc + offset + 16 + i * 8 + 4, entry.value)
            }
        }
        let subdirectory = 0x8000_0000
        directory(0x00, [(3, subdirectory | 0x30), (14, subdirectory | 0x60)])
        directory(0x30, [(1, subdirectory | 0x48)])
        directory(0x48, [(1033, 0x90)])
        directory(0x60, [(1, subdirectory | 0x78)])
        directory(0x78, [(1033, 0xA0)])
        put32(rsrc + 0x90, 0x2100); put32(rsrc + 0x94, png.count)
        put32(rsrc + 0xA0, 0x2300); put32(rsrc + 0xA4, 6 + 14)
        pe.replaceSubrange((rsrc + 0x100)..<(rsrc + 0x100 + png.count), with: png)
        // GRPICONDIR with one GRPICONDIRENTRY naming RT_ICON 1.
        let group = rsrc + 0x300
        put16(group + 2, 1); put16(group + 4, 1)
        pe[group + 6] = 16; pe[group + 7] = 16
        put16(group + 10, 1); put16(group + 12, 32)
        put32(group + 14, png.count); put16(group + 18, 1)

        let url = temporaryRoot.appendingPathComponent("icon.exe")
        try pe.write(to: url)

        let icon = try XCTUnwrap(extractPEIcon(at: url))
        XCTAssertEqual(icon.size, NSSize(width: 16, height: 16))
    }

    func testGuestProcessDecodesRuntimeJsonContract() throws {
        let json = """
        [{"pid":42,"parent_pid":7,"image_name":"demo.exe",
          "image_path":"C:\\\\Demo\\\\demo.exe","capsule":"test"}]
        """
        let result = try JSONDecoder().decode(
            [GuestProcessInfo].self, from: Data(json.utf8))

        XCTAssertEqual(result.first?.pid, 42)
        XCTAssertEqual(result.first?.parentPid, 7)
        XCTAssertEqual(result.first?.capsule, "test")
    }

    func testMacAppIconWriterCreatesPortableICNSResource() throws {
        let image = NSImage(size: NSSize(width: 64, height: 64))
        image.lockFocus()
        NSColor.systemIndigo.setFill()
        NSBezierPath(
            roundedRect: NSRect(x: 4, y: 4, width: 56, height: 56),
            xRadius: 12, yRadius: 12
        ).fill()
        image.unlockFocus()

        let resources = temporaryRoot.appendingPathComponent("Contents/Resources")
        let output = try writeMacAppIcon(image, to: resources)
        let data = try Data(contentsOf: output)

        XCTAssertEqual(output.lastPathComponent, "AppIcon.icns")
        XCTAssertEqual(String(data: data.prefix(4), encoding: .ascii), "icns")
        XCTAssertFalse(
            try FileManager.default.contentsOfDirectory(atPath: resources.path)
                .contains(where: { $0.hasSuffix(".iconset") }))
    }

    @MainActor
    func testMacLauncherUsesNativeExecutableAndJSONLaunchProfile() throws {
        let executable = temporaryRoot.appendingPathComponent("Demo/demo.exe")
        try FileManager.default.createDirectory(
            at: executable.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try Data([0x4D, 0x5A]).write(to: executable)

        let profile = LaunchProfile(
            arguments: ["--windowed", "two words"],
            workingDirectory: "C:\\Demo",
            environment: ["DEMO_MODE": "test"],
            forceInterpreter: true,
            enableTrace: true)
        let app = InstalledApp(
            name: "Demo",
            capsuleName: "demo",
            architecture: .x86,
            executablePath: executable.path,
            windowsExecutablePath: "C:\\Demo\\demo.exe",
            lnkPath: temporaryRoot.appendingPathComponent("Demo.lnk").path,
            launchProfile: profile)
        let destination = temporaryRoot.appendingPathComponent("Launchers")
        try FileManager.default.createDirectory(
            at: destination, withIntermediateDirectories: true)

        let service = VitreoService()
        try service.createMacLauncher(for: app, in: destination)

        let bundle = destination.appendingPathComponent("Demo.app")
        let helper = bundle.appendingPathComponent(
            "Contents/MacOS/VitreoLauncher")
        let plist = NSDictionary(
            contentsOf: bundle.appendingPathComponent("Contents/Info.plist"))
        let configData = try Data(contentsOf: bundle.appendingPathComponent(
            "Contents/Resources/LauncherConfig.json"))
        let config = try XCTUnwrap(
            JSONSerialization.jsonObject(with: configData) as? [String: Any])
        let arguments = try XCTUnwrap(config["arguments"] as? [String])

        XCTAssertEqual(plist?["CFBundleExecutable"] as? String, "VitreoLauncher")
        XCTAssertTrue(FileManager.default.isExecutableFile(atPath: helper.path))
        XCTAssertEqual(
            Array(try Data(contentsOf: helper).prefix(4)),
            [0xCF, 0xFA, 0xED, 0xFE])
        XCTAssertEqual(config["executable"] as? String, service.vitreoBinaryPath)
        XCTAssertEqual(arguments, [
            "run", "--capsule", "demo", "--app-name", "Demo",
            "--interpreter", "--trace",
            "--guest-cwd", "C:\\Demo",
            "--env", "DEMO_MODE=test",
            executable.path,
            "--", "--windowed", "two words",
        ])
        XCTAssertFalse(
            FileManager.default.fileExists(
                atPath: bundle.appendingPathComponent(
                    "Contents/MacOS/launch").path))
    }
}
