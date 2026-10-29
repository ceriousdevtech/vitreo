// SPDX-License-Identifier: LGPL-2.1-only

import Foundation
import XCTest
@testable import VitreoManager

/// The Finder launch path resolves capsules with `probeCapsules`, which must
/// stay cheap: a machine with installed games has capsules holding tens of
/// thousands of files, and `scan` walks all of them.
final class CapsuleProbeTests: XCTestCase {
    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("vitreo-probe-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(
            at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private func makeCapsule(
        _ name: String, healthy: Bool = true, files: Int = 0
    ) throws {
        let capsule = root.appendingPathComponent(name)
        try FileManager.default.createDirectory(
            at: capsule.appendingPathComponent("drives/c"),
            withIntermediateDirectories: true)
        if healthy {
            try FileManager.default.createDirectory(
                at: capsule.appendingPathComponent("registry"),
                withIntermediateDirectories: true)
        }
        for index in 0..<files {
            try Data(repeating: 0, count: 64).write(
                to: capsule.appendingPathComponent("drives/c/file\(index).bin"))
        }
    }

    func testReportsNamesAndHealth() throws {
        try makeCapsule("default")
        try makeCapsule("broken", healthy: false)

        let capsules = CapsuleScanner().probeCapsules(root: root)

        XCTAssertEqual(capsules.map(\.name), ["broken", "default"])
        XCTAssertFalse(capsules[0].isHealthy)
        XCTAssertTrue(capsules[1].isHealthy)
    }

    func testDoesNotWalkCapsuleContents() throws {
        try makeCapsule("heavy", files: 250)

        let capsules = CapsuleScanner().probeCapsules(root: root)

        // Sizing is what makes the full scan expensive; the probe must skip it
        // rather than report a size it did not measure.
        XCTAssertEqual(capsules.count, 1)
        XCTAssertEqual(capsules[0].sizeBytes, 0)
        XCTAssertTrue(capsules[0].mounts.isEmpty)
    }

    func testAgreesWithTheFullScanAboutHealth() throws {
        try makeCapsule("default", files: 3)
        try makeCapsule("broken", healthy: false)

        let probed = CapsuleScanner().probeCapsules(root: root)
        let scanned = CapsuleScanner().scan(root: root).capsules

        XCTAssertEqual(
            probed.map { [$0.name: $0.isHealthy] },
            scanned.map { [$0.name: $0.isHealthy] })
    }
}
