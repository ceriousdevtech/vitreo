// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import SwiftUI

/// The disclaimer every user accepts before Vitreo runs anything, shown on
/// first launch and kept in Settings ▸ About.
///
/// Acceptance is stored with `version`; raise it when the wording changes in
/// substance so existing users are asked again.
enum LegalTerms {
    static let version = 1
    private static let acceptedKey = "acceptedLegalTermsVersion"

    // The no-warranty and liability terms restate LGPL-2.1 sections 15-16,
    // the license Vitreo ships under; the License button shows them in full.
    static let paragraphs = [
        "THIS SOFTWARE IS PROVIDED \u{201C}AS IS\u{201D}, WITHOUT WARRANTY OF ANY KIND, "
            + "EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF "
            + "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.",
        "IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, "
            + "DAMAGES OR OTHER LIABILITY, INCLUDING LOSS OF DATA, DAMAGE TO YOUR "
            + "COMPUTER, ITS HARDWARE OR OTHER SOFTWARE, OR ACTIONS TAKEN AGAINST YOUR "
            + "ACCOUNTS BY THIRD-PARTY SERVICES, ARISING FROM THE USE OF OR INABILITY TO "
            + "USE THIS SOFTWARE. YOU USE VITREO AT YOUR OWN RISK.",
        "Vitreo is not affiliated with, endorsed by, or sponsored by Microsoft "
            + "Corporation, Apple Inc., Valve Corporation, or any software publisher. "
            + "Microsoft, Windows and DirectX are trademarks of Microsoft Corporation; "
            + "all other trademarks belong to their respective owners.",
        "Vitreo does not include Windows or any Microsoft software. You are "
            + "responsible for holding valid licenses for the software you run with it "
            + "and for following its terms of use. Some online games and anti-cheat "
            + "systems do not permit compatibility layers and may restrict accounts "
            + "that use them.",
    ]

    static var isAccepted: Bool {
        UserDefaults.standard.integer(forKey: acceptedKey) >= version
    }

    static func accept() {
        UserDefaults.standard.set(version, forKey: acceptedKey)
    }

    /// Opens a file or folder the build script copies into Contents/Resources.
    /// False when the bundle does not carry it.
    @discardableResult
    static func openBundled(_ name: String) -> Bool {
        guard let url = Bundle.main.resourceURL?.appendingPathComponent(name),
              FileManager.default.fileExists(atPath: url.path) else {
            return false
        }
        NSWorkspace.shared.open(url)
        return true
    }

    /// Shows the terms in an application-modal window until the user accepts
    /// or quits. Returns true once they are accepted (at once if they already
    /// were). Main thread; blocks in a modal run loop.
    @MainActor
    static func requireAcceptance() -> Bool {
        if isAccepted { return true }
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 560, height: 520),
            styleMask: [.titled],
            backing: .buffered,
            defer: false)
        window.title = "Welcome to Vitreo"
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: LegalTermsView(
            onAccept: { NSApplication.shared.stopModal(withCode: .OK) },
            onQuit: { NSApplication.shared.stopModal(withCode: .cancel) }))
        window.center()
        NSApplication.shared.activate(ignoringOtherApps: true)
        let response = NSApplication.shared.runModal(for: window)
        window.orderOut(nil)
        guard response == .OK else { return false }
        accept()
        return true
    }
}

/// The first-launch terms: the text, an "I have read" checkbox, and Continue
/// enabled only once it is ticked.
struct LegalTermsView: View {
    let onAccept: () -> Void
    let onQuit: () -> Void
    @State private var hasRead = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(spacing: 12) {
                Image(nsImage: NSApplication.shared.applicationIconImage)
                    .resizable()
                    .frame(width: 48, height: 48)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Before you start")
                        .font(.title2.bold())
                    Text("Please read these terms. They are also in Settings \u{25B8} About.")
                        .foregroundStyle(.secondary)
                }
            }

            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    ForEach(LegalTerms.paragraphs, id: \.self) { paragraph in
                        Text(paragraph)
                            .font(.callout)
                            .fixedSize(horizontal: false, vertical: true)
                            .textSelection(.enabled)
                    }
                }
                .padding(14)
            }
            .background(
                RoundedRectangle(cornerRadius: 8).fill(.background))
            .overlay(
                RoundedRectangle(cornerRadius: 8).strokeBorder(.quaternary, lineWidth: 0.5))

            HStack(spacing: 8) {
                Button("License") { LegalTerms.openBundled("LICENSE") }
                Button("Third-Party Notices") { LegalTerms.openBundled("third-party-notices") }
            }
            .controlSize(.small)

            Toggle("I have read and agree to these terms", isOn: $hasRead)
                .toggleStyle(.checkbox)

            HStack {
                Spacer()
                Button("Quit", role: .cancel, action: onQuit)
                    .keyboardShortcut(.cancelAction)
                Button("Continue", action: onAccept)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!hasRead)
            }
        }
        .padding(20)
        .frame(width: 560, height: 520)
    }
}
