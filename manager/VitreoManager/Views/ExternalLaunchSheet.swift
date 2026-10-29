// SPDX-License-Identifier: LGPL-2.1-only

import SwiftUI

/// Shows the capsule chooser in its own window.
///
/// A sheet would need the Manager's main window, but opening an `.exe` from
/// Finder deliberately leaves that window closed. A standalone window keeps the
/// prompt identical whether or not the Manager UI is on screen.
@MainActor
final class ExternalLaunchWindowPresenter: NSObject, NSWindowDelegate {
    private static let identifier = NSUserInterfaceItemIdentifier(
        "VitreoExternalLaunchChooser")

    private var window: NSWindow?
    private var presentedRequestID: UUID?
    private var cancelHandler: (() -> Void)?

    static func isChooserWindow(_ window: NSWindow) -> Bool {
        window.identifier == identifier
    }

    func present(
        _ request: ExternalLaunchRequest,
        capsules: [CapsuleInfo],
        suggestedCapsule: String,
        onRun: @escaping (String, Bool) -> Void,
        onCancel: @escaping () -> Void
    ) {
        guard presentedRequestID != request.id else { return }
        dismiss()

        let view = ExternalLaunchSheet(
            request: request,
            capsules: capsules,
            suggestedCapsule: suggestedCapsule,
            onRun: onRun,
            onCancel: onCancel)
        let window = NSWindow(
            contentViewController: NSHostingController(rootView: view))
        window.title = "Run with Vitreo"
        window.styleMask = [.titled, .closable]
        window.identifier = Self.identifier
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()

        self.window = window
        presentedRequestID = request.id
        // Closing the window is a decision not to run anything.
        cancelHandler = onCancel

        window.makeKeyAndOrderFront(nil)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }

    func dismiss() {
        cancelHandler = nil
        presentedRequestID = nil
        window?.delegate = nil
        window?.close()
        window = nil
    }

    func windowWillClose(_ notification: Notification) {
        guard let cancel = cancelHandler else { return }
        cancelHandler = nil
        presentedRequestID = nil
        window = nil
        cancel()
    }
}

/// Asks which capsule an executable opened from Finder should run in.
struct ExternalLaunchSheet: View {
    let request: ExternalLaunchRequest
    let capsules: [CapsuleInfo]
    let suggestedCapsule: String
    let onRun: (String, Bool) -> Void
    let onCancel: () -> Void

    @State private var selectedCapsule = ""
    @State private var rememberChoice = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider()
            body(for: request)
            Divider()
            footer
        }
        .frame(width: 520)
        .onAppear {
            if selectedCapsule.isEmpty { selectedCapsule = suggestedCapsule }
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            Image("VitreoMark")
                .resizable()
                .frame(width: 40, height: 40)
            VStack(alignment: .leading, spacing: 2) {
                Text("Run with Vitreo").font(.title2.bold())
                Text(request.displayName)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
        }
        .padding(24)
    }

    @ViewBuilder
    private func body(for request: ExternalLaunchRequest) -> some View {
        VStack(alignment: .leading, spacing: 16) {
            LabeledContent("Location") {
                Text(
                    request.canonicalURL.deletingLastPathComponent()
                        .path(percentEncoded: false))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .truncationMode(.middle)
                    .textSelection(.enabled)
            }
            LabeledContent("Architecture") {
                Text(request.architecture.rawValue)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            Picker("Capsule", selection: $selectedCapsule) {
                ForEach(capsules) { capsule in
                    Text(capsule.name).tag(capsule.name)
                }
            }
            Toggle(
                "Always run this executable in this capsule",
                isOn: $rememberChoice)
            Label(
                "Vitreo will make this executable's folder available to the "
                + "selected capsule while the application is running.",
                systemImage: "info.circle")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(24)
    }

    private var footer: some View {
        HStack {
            Spacer()
            Button("Cancel", role: .cancel) { onCancel() }
                .keyboardShortcut(.cancelAction)
            Button("Run") { onRun(selectedCapsule, rememberChoice) }
                .keyboardShortcut(.defaultAction)
                .disabled(selectedCapsule.isEmpty)
        }
        .padding(.horizontal, 24)
        .padding(.vertical, 16)
    }
}
