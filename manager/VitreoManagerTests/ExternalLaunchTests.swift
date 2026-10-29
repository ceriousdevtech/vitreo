// SPDX-License-Identifier: LGPL-2.1-only

import Foundation
import XCTest
@testable import VitreoManager

@MainActor
final class EngineResultReportingTests: XCTestCase {
    func testReadsFullWindowsExceptionCode() throws {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("vitreo-result-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: url) }
        try "3221225477\n".write(to: url, atomically: true, encoding: .utf8)

        let result = VitreoService.readEngineResult(from: url, fallback: 5)

        XCTAssertEqual(UInt32(bitPattern: result), 0xC0000005)
        XCTAssertEqual(VitreoService.formatWindowsExitCode(result), "0xC0000005")
    }

    func testFallsBackToNativeStatusWhenResultIsMissing() {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("missing-vitreo-result-\(UUID().uuidString)")

        XCTAssertEqual(VitreoService.readEngineResult(from: url, fallback: 7), 7)
        XCTAssertEqual(VitreoService.formatWindowsExitCode(7), "7")
    }
}

// MARK: - Test doubles

@MainActor
private final class StubRuleStore: ExternalLaunchRuleStoring {
    var policy: ExternalLaunchPolicy = .askEveryTime
    var lastPersistenceError: String?
    private(set) var rules: [String: String] = [:]

    var ruleCount: Int { rules.count }

    func rule(forCanonicalPath path: String) -> ExternalLaunchRule? {
        rules[path].map {
            ExternalLaunchRule(canonicalPath: path, capsuleName: $0)
        }
    }

    func setRule(_ rule: ExternalLaunchRule) {
        rules[rule.canonicalPath] = rule.capsuleName
    }

    func removeRule(forCanonicalPath path: String) {
        rules.removeValue(forKey: path)
    }

    func removeAllRules() { rules.removeAll() }
}

@MainActor
private final class StubHost: ExternalLaunchHosting {
    var capsuleRootDirectory: URL
    var healthyCapsules: [CapsuleInfo] = []
    var configuredDefaultCapsule = "default"
    private(set) var launches: [(url: URL, capsule: String)] = []
    private(set) var failures: [String] = []
    private(set) var refreshCount = 0

    init(capsuleRoot: URL) { capsuleRootDirectory = capsuleRoot }

    func refreshCapsuleState() async { refreshCount += 1 }

    @discardableResult
    func launchExternalExecutable(
        _ executableURL: URL, capsuleName: String
    ) -> UUID? {
        launches.append((executableURL, capsuleName))
        return UUID()
    }

    func reportExternalLaunchFailure(_ message: String) {
        failures.append(message)
    }
}

// MARK: - PE fixtures

private enum PEFixture {
    /// Minimal file whose DOS/PE headers are valid enough to classify.
    static func make(machine: UInt16, peOffset: UInt32 = 0x80) -> Data {
        var data = Data(repeating: 0, count: Int(peOffset) + 0x40)
        data[0] = 0x4D  // 'M'
        data[1] = 0x5A  // 'Z'
        let offset = peOffset.littleEndian
        withUnsafeBytes(of: offset) { bytes in
            for (index, byte) in bytes.enumerated() { data[0x3C + index] = byte }
        }
        let base = Int(peOffset)
        data[base] = 0x50      // 'P'
        data[base + 1] = 0x45  // 'E'
        data[base + 2] = 0x00
        data[base + 3] = 0x00
        let machineLE = machine.littleEndian
        withUnsafeBytes(of: machineLE) { bytes in
            for (index, byte) in bytes.enumerated() {
                data[base + 4 + index] = byte
            }
        }
        return data
    }
}

// MARK: - PE inspection

final class PEInspectorTests: XCTestCase {
    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("vitreo-pe-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func write(_ data: Data, named name: String) throws -> URL {
        let url = root.appendingPathComponent(name)
        try data.write(to: url)
        return url
    }

    func testAcceptsPE32() throws {
        let url = try write(PEFixture.make(machine: 0x014C), named: "app.exe")
        let inspection = try XCTUnwrap(try? PEInspector.inspect(url: url).get())
        XCTAssertEqual(inspection.architecture, .x86)
    }

    func testAcceptsPE32Plus() throws {
        let url = try write(PEFixture.make(machine: 0x8664), named: "app64.exe")
        let inspection = try XCTUnwrap(try? PEInspector.inspect(url: url).get())
        XCTAssertEqual(inspection.architecture, .x64)
    }

    func testRejectsMissingMZSignature() throws {
        var data = PEFixture.make(machine: 0x014C)
        data[0] = 0x00
        let url = try write(data, named: "bogus.exe")
        assertFailure(PEInspector.inspect(url: url), .notAWindowsExecutable)
    }

    func testRejectsTruncatedPEHeader() throws {
        var data = PEFixture.make(machine: 0x014C)
        data = data.prefix(0x50)  // e_lfanew points past end of file
        let url = try write(Data(data), named: "short.exe")
        assertFailure(PEInspector.inspect(url: url), .truncatedHeader)
    }

    func testRejectsUnsupportedMachine() throws {
        let url = try write(
            PEFixture.make(machine: 0x01C4), named: "arm.exe")  // ARMv7
        assertFailure(
            PEInspector.inspect(url: url), .unsupportedMachine(0x01C4))
    }

    func testAcceptsInstallerPackagesWithoutAPeHeader() throws {
        for ext in ["msi", "msp", "msix", "appx", "msu", "mst"] {
            let url = root.appendingPathComponent("setup.\(ext)")
            try Data([0xD0, 0xCF, 0x11, 0xE0]).write(to: url)
            let inspection = try XCTUnwrap(try? PEInspector.inspect(url: url).get())
            XCTAssertEqual(inspection.architecture, .unknown, ext)
        }
    }

    func testRejectsNonExeExtension() throws {
        let url = try write(PEFixture.make(machine: 0x014C), named: "lib.dll")
        assertFailure(
            PEInspector.inspect(url: url), .unexpectedExtension("dll"))
    }

    func testRejectsDirectory() throws {
        let url = root.appendingPathComponent("folder.exe")
        try FileManager.default.createDirectory(
            at: url, withIntermediateDirectories: true)
        assertFailure(PEInspector.inspect(url: url), .notARegularFile)
    }

    func testRejectsMissingFile() {
        assertFailure(
            PEInspector.inspect(url: root.appendingPathComponent("gone.exe")),
            .missing)
    }

    func testCanonicalizesRelativeComponents() throws {
        let url = try write(PEFixture.make(machine: 0x014C), named: "app.exe")
        let indirect = root
            .appendingPathComponent("sub")
            .appendingPathComponent("..")
            .appendingPathComponent("app.exe")
        let inspection = try XCTUnwrap(
            try? PEInspector.inspect(url: indirect).get())
        XCTAssertEqual(
            inspection.canonicalURL.path,
            PEInspector.canonicalize(url).path)
    }

    private func assertFailure(
        _ result: Result<PEInspection, PEInspectionError>,
        _ expected: PEInspectionError,
        file: StaticString = #filePath,
        line: UInt = #line
    ) {
        switch result {
        case .success:
            XCTFail("expected \(expected)", file: file, line: line)
        case .failure(let error):
            XCTAssertEqual(error, expected, file: file, line: line)
        }
    }
}

// MARK: - Capsule classification

final class CapsuleClassifierTests: XCTestCase {
    private let root = URL(fileURLWithPath: "/Users/test/.vitreo/capsules")

    func testInfersOwningCapsule() {
        let url = root.appendingPathComponent(
            "demo/drives/c/Program Files/App/app.exe")
        XCTAssertEqual(
            CapsuleClassifier.owningCapsule(of: url, capsuleRoot: root), "demo")
    }

    func testDoesNotConfuseCapsulesSharingAPrefix() {
        let url = root.appendingPathComponent(
            "demo-old/drives/c/App/app.exe")
        XCTAssertEqual(
            CapsuleClassifier.owningCapsule(of: url, capsuleRoot: root),
            "demo-old")
    }

    func testExternalPathHasNoOwner() {
        let url = URL(fileURLWithPath: "/Users/test/Downloads/setup.exe")
        XCTAssertNil(
            CapsuleClassifier.owningCapsule(of: url, capsuleRoot: root))
    }

    func testNonDriveCapsulePathIsNotAnExecutableRoot() {
        let url = root.appendingPathComponent("demo/registry/system.reg.exe")
        XCTAssertNil(
            CapsuleClassifier.owningCapsule(of: url, capsuleRoot: root))
    }

    func testCapsuleRootItselfIsNotOwned() {
        let url = root.appendingPathComponent("demo/drives/c")
        XCTAssertNil(
            CapsuleClassifier.owningCapsule(of: url, capsuleRoot: root))
    }
}

// MARK: - Event broker

@MainActor
final class MacOpenEventBrokerTests: XCTestCase {
    func testPreservesEventsReceivedBeforeAttach() {
        let broker = MacOpenEventBroker()
        broker.submit(url: URL(fileURLWithPath: "/tmp/a.exe"), source: .launchServices)
        broker.submit(url: URL(fileURLWithPath: "/tmp/b.exe"), source: .launchServices)
        XCTAssertEqual(broker.pendingCount, 2)

        var received: [String] = []
        broker.attach { url, _ in received.append(url.lastPathComponent) }

        XCTAssertEqual(received, ["a.exe", "b.exe"])
        XCTAssertEqual(broker.pendingCount, 0)
    }

    func testDeliversLaterEventsImmediately() {
        let broker = MacOpenEventBroker()
        var received: [String] = []
        broker.attach { url, _ in received.append(url.lastPathComponent) }
        broker.submit(url: URL(fileURLWithPath: "/tmp/c.exe"), source: .launchServices)
        XCTAssertEqual(received, ["c.exe"])
    }

    func testBoundsASingleEventAndReportsTheRemainder() {
        let broker = MacOpenEventBroker()
        var ignored = 0
        broker.onOverflow = { ignored = $0 }
        let urls = (0..<40).map {
            URL(fileURLWithPath: "/tmp/app\($0).exe")
        }
        broker.submit(urls: urls, source: .launchServices)
        XCTAssertEqual(broker.pendingCount, MacOpenEventBroker.maximumURLsPerEvent)
        XCTAssertEqual(ignored, 40 - MacOpenEventBroker.maximumURLsPerEvent)
    }
}

// MARK: - Selection policy

@MainActor
final class ExternalLaunchCoordinatorTests: XCTestCase {
    private var root: URL!
    private var capsuleRoot: URL!
    private var host: StubHost!
    private var store: StubRuleStore!
    private var coordinator: ExternalLaunchCoordinator!

    override func setUp() async throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("vitreo-launch-tests-\(UUID().uuidString)")
        capsuleRoot = root.appendingPathComponent("capsules")
        try FileManager.default.createDirectory(
            at: capsuleRoot, withIntermediateDirectories: true)
        host = StubHost(capsuleRoot: capsuleRoot)
        host.healthyCapsules = [capsule("default"), capsule("games")]
        store = StubRuleStore()
        coordinator = ExternalLaunchCoordinator(host: host, ruleStore: store)
    }

    override func tearDown() async throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func capsule(_ name: String) -> CapsuleInfo {
        CapsuleInfo(
            name: name,
            rootPath: capsuleRoot.appendingPathComponent(name).path,
            hasCDrive: true, hasRegistry: true, sizeBytes: 0,
            modifiedAt: nil, mounts: [])
    }

    @discardableResult
    private func makeExecutable(
        at relativePath: String, machine: UInt16 = 0x014C
    ) throws -> URL {
        let url = root.appendingPathComponent(relativePath)
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try PEFixture.make(machine: machine).write(to: url)
        return url
    }

    /// Drive the coordinator's async resolution to completion.
    private func settle() async {
        for _ in 0..<20 {
            await Task.yield()
            try? await Task.sleep(nanoseconds: 5_000_000)
            if !host.launches.isEmpty || !host.failures.isEmpty
                || coordinator.activeRequest != nil {
                // Give any queued follow-up work one more turn.
                await Task.yield()
                return
            }
        }
    }

    func testAsksWhenPolicyIsAskEveryTime() async throws {
        let exe = try makeExecutable(at: "Downloads/setup.exe")
        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNotNil(coordinator.activeRequest)
        XCTAssertTrue(host.launches.isEmpty)
        XCTAssertEqual(coordinator.suggestedCapsule, "default")
    }

    func testUsesDefaultCapsuleWhenConfigured() async throws {
        store.policy = .useDefaultCapsule
        let exe = try makeExecutable(at: "Downloads/setup.exe")
        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNil(coordinator.activeRequest)
        XCTAssertEqual(host.launches.count, 1)
        XCTAssertEqual(host.launches.first?.capsule, "default")
    }

    func testFallsBackToChooserWhenDefaultCapsuleIsInvalid() async throws {
        store.policy = .useDefaultCapsule
        host.configuredDefaultCapsule = "deleted"
        let exe = try makeExecutable(at: "Downloads/setup.exe")
        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNotNil(coordinator.activeRequest)
        XCTAssertTrue(host.launches.isEmpty)
    }

    func testResolvesRememberedRule() async throws {
        let exe = try makeExecutable(at: "Downloads/setup.exe")
        store.setRule(ExternalLaunchRule(
            canonicalPath: PEInspector.canonicalize(exe).path,
            capsuleName: "games"))

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertEqual(host.launches.first?.capsule, "games")
    }

    func testStaleRememberedRuleIsDiscardedAndAsksAgain() async throws {
        let exe = try makeExecutable(at: "Downloads/setup.exe")
        let path = PEInspector.canonicalize(exe).path
        store.setRule(ExternalLaunchRule(
            canonicalPath: path, capsuleName: "removed"))

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNotNil(coordinator.activeRequest)
        XCTAssertNil(store.rule(forCanonicalPath: path))
    }

    func testExecutableInsideACapsuleAlwaysUsesThatCapsule() async throws {
        let exe = try makeExecutable(
            at: "capsules/games/drives/c/Program Files/Game/game.exe")
        store.policy = .useDefaultCapsule

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNil(coordinator.activeRequest)
        XCTAssertEqual(host.launches.first?.capsule, "games")
    }

    func testOwnedExecutableInUnhealthyCapsuleFailsWithoutChooser() async throws {
        let exe = try makeExecutable(
            at: "capsules/broken/drives/c/App/app.exe")

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNil(coordinator.activeRequest)
        XCTAssertTrue(host.launches.isEmpty)
        XCTAssertEqual(host.failures.count, 1)
        XCTAssertTrue(host.failures[0].contains("broken"))
    }

    func testRunWithVitreoServiceAlwaysAsks() async throws {
        store.policy = .useDefaultCapsule
        let exe = try makeExecutable(at: "Downloads/setup.exe")

        coordinator.open(url: exe, source: .runWithVitreoService)
        await settle()

        XCTAssertNotNil(coordinator.activeRequest)
        XCTAssertTrue(host.launches.isEmpty)
    }

    func testNoHealthyCapsuleReportsInsteadOfCreatingOne() async throws {
        host.healthyCapsules = []
        let exe = try makeExecutable(at: "Downloads/setup.exe")

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertNil(coordinator.activeRequest)
        XCTAssertTrue(host.launches.isEmpty)
        XCTAssertEqual(host.failures.count, 1)
    }

    func testInvalidExecutableNeverLaunches() async throws {
        let url = root.appendingPathComponent("Downloads/notes.exe")
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try Data("not a PE".utf8).write(to: url)

        coordinator.open(url: url, source: .launchServices)
        await settle()

        XCTAssertTrue(host.launches.isEmpty)
        XCTAssertNil(coordinator.activeRequest)
        XCTAssertEqual(host.failures.count, 1)
    }

    func testConfirmRemembersCapsuleAndLaunches() async throws {
        let exe = try makeExecutable(at: "Downloads/setup.exe")
        coordinator.open(url: exe, source: .launchServices)
        await settle()
        let request = try XCTUnwrap(coordinator.activeRequest)

        coordinator.confirm(request, capsuleName: "games", remember: true)

        XCTAssertNil(coordinator.activeRequest)
        XCTAssertEqual(host.launches.first?.capsule, "games")
        XCTAssertEqual(
            store.rule(forCanonicalPath: request.canonicalURL.path)?.capsuleName,
            "games")
    }

    func testCancelStartsNothingAndAdvancesTheQueue() async throws {
        let first = try makeExecutable(at: "Downloads/one.exe")
        let second = try makeExecutable(at: "Downloads/two.exe")
        coordinator.open(url: first, source: .launchServices)
        coordinator.open(url: second, source: .launchServices)
        for _ in 0..<40 {
            await Task.yield()
            try? await Task.sleep(nanoseconds: 5_000_000)
        }

        let active = try XCTUnwrap(coordinator.activeRequest)
        XCTAssertEqual(active.displayName, "one.exe")

        coordinator.cancel(active)
        XCTAssertTrue(host.launches.isEmpty)
        XCTAssertEqual(coordinator.activeRequest?.displayName, "two.exe")
    }

    func testOnlyOneChooserIsEverActive() async throws {
        for index in 0..<3 {
            _ = try makeExecutable(at: "Downloads/app\(index).exe")
        }
        for index in 0..<3 {
            coordinator.open(
                url: root.appendingPathComponent("Downloads/app\(index).exe"),
                source: .launchServices)
        }
        for _ in 0..<60 {
            await Task.yield()
            try? await Task.sleep(nanoseconds: 5_000_000)
        }

        XCTAssertNotNil(coordinator.activeRequest)
        XCTAssertEqual(coordinator.activeRequest?.displayName, "app0.exe")
    }

    func testResolutionRefreshesCapsuleStateBeforeDeciding() async throws {
        store.policy = .useDefaultCapsule
        let exe = try makeExecutable(at: "Downloads/setup.exe")

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertEqual(host.refreshCount, 1)
        XCTAssertEqual(host.launches.count, 1)
    }

    func testUnicodeAndSpacedPathsSurviveResolution() async throws {
        store.policy = .useDefaultCapsule
        let exe = try makeExecutable(at: "My Games/Æther Ω/set up.exe")

        coordinator.open(url: exe, source: .launchServices)
        await settle()

        XCTAssertEqual(
            host.launches.first?.url.path, PEInspector.canonicalize(exe).path)
    }
}
