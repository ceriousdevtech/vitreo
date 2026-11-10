// SPDX-License-Identifier: LGPL-2.1-only

import XCTest
@testable import VitreoManager

final class CommandLineLinkTests: XCTestCase {
    func testInstalledApplicationsAreTheOnesTerminalShouldFollow() {
        XCTAssertTrue(CommandLineLink.isInstalledApplication(
            "/Applications/Vitreo.app"))
        XCTAssertTrue(CommandLineLink.isInstalledApplication(
            "/Users/ada/Applications/Vitreo.app"))
        XCTAssertFalse(CommandLineLink.isInstalledApplication(
            "/Volumes/Vitreo/Vitreo.app"))
        XCTAssertFalse(CommandLineLink.isInstalledApplication(
            "/tmp/Vitreo.app"))
        XCTAssertFalse(CommandLineLink.isInstalledApplication(
            "/Users/ada/Applications/Other/Vitreo.app"))
    }

    func testFirstLaunchOfAnInstalledCopyCreatesTheLink() {
        XCTAssertEqual(plan(bundle: "/Applications/Vitreo.app", linkExists: false), .create)
    }

    func testAnExistingLinkToThisAppIsLeftAlone() {
        XCTAssertEqual(
            plan(
                bundle: "/Users/ada/Applications/Vitreo.app",
                linkExists: true,
                linkIsSymlink: true,
                linkDestination: "/Users/ada/Applications/Vitreo.app/Contents/Helpers/vitreo"),
            .alreadyLinked)
    }

    func testALinkToAnotherCopyIsRetargeted() {
        XCTAssertEqual(
            plan(
                bundle: "/Applications/Vitreo.app",
                linkExists: true,
                linkIsSymlink: true,
                linkDestination: "/Users/ada/Applications/Vitreo.app/Contents/Helpers/vitreo"),
            .replaceSymlink)
    }

    func testARegularFileAtTheCommandPathIsNotReplaced() {
        XCTAssertEqual(
            plan(
                bundle: "/Applications/Vitreo.app",
                linkExists: true,
                linkIsSymlink: false),
            .blockedByRegularFile)
    }

    func testADiskImageOrBuildCopyDoesNotClaimTheCommand() {
        XCTAssertEqual(
            plan(bundle: "/Volumes/Vitreo/Vitreo.app", linkExists: false),
            .skip)
        XCTAssertEqual(
            plan(
                bundle: "/Users/ada/src/manager/build/release/Vitreo.app",
                linkExists: false),
            .skip)
    }

    func testFirstLaunchAsksBeforeWritingAnything() {
        XCTAssertEqual(
            CommandLineLink.launchStep(
                plan: .create, consent: .undecided, needsAdministrator: true),
            .ask(retarget: false))
        XCTAssertEqual(
            CommandLineLink.launchStep(
                plan: .create, consent: .undecided, needsAdministrator: false),
            .ask(retarget: false))
    }

    func testADeclinedCommandStaysQuiet() {
        XCTAssertEqual(
            CommandLineLink.launchStep(
                plan: .create, consent: .declined, needsAdministrator: true),
            .none)
        XCTAssertEqual(
            CommandLineLink.launchStep(
                plan: .replaceSymlink, consent: .declined, needsAdministrator: false),
            .none)
    }

    func testAnAgreedRetargetNeedingAPasswordIsExplainedAgain() {
        XCTAssertEqual(
            CommandLineLink.launchStep(
                plan: .replaceSymlink, consent: .granted, needsAdministrator: true),
            .ask(retarget: true))
        XCTAssertEqual(
            CommandLineLink.launchStep(
                plan: .replaceSymlink, consent: .granted, needsAdministrator: false),
            .install)
    }

    func testNoLinkNeededMeansNoQuestion() {
        for consent in [CommandLineLink.Consent.undecided, .granted, .declined] {
            XCTAssertEqual(
                CommandLineLink.launchStep(
                    plan: .alreadyLinked, consent: consent, needsAdministrator: true),
                .none)
            XCTAssertEqual(
                CommandLineLink.launchStep(
                    plan: .skip, consent: consent, needsAdministrator: true),
                .none)
        }
    }

    func testAnEarlierCancelledPasswordDialogCountsAsNo() {
        let defaults = UserDefaults(suiteName: "CommandLineLinkTests-\(UUID())")!
        XCTAssertEqual(CommandLineLink.consent(in: defaults), .undecided)
        defaults.set(true, forKey: CommandLineLink.legacyDeclinedDefaultsKey)
        XCTAssertEqual(CommandLineLink.consent(in: defaults), .declined)
        CommandLineLink.record(.granted, in: defaults)
        XCTAssertEqual(CommandLineLink.consent(in: defaults), .granted)
        XCTAssertNil(defaults.object(forKey: CommandLineLink.legacyDeclinedDefaultsKey))
    }

    private func plan(
        bundle: String,
        linkExists: Bool,
        linkIsSymlink: Bool = false,
        linkDestination: String? = nil
    ) -> CommandLineLink.Plan {
        CommandLineLink.plan(
            bundlePath: bundle,
            engineExists: true,
            linkExists: linkExists,
            linkIsSymlink: linkIsSymlink,
            linkDestination: linkDestination)
    }
}
