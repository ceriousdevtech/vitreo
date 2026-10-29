// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — macOS management app for the Vitreo Windows runtime engine

import AppKit
import Combine
import SwiftUI

/// Bridges AppKit's file-open events into the Manager.
///
/// SwiftUI's `onOpenURL` is not a reliable delivery path for Launch Services
/// document opens, and a cold launch delivers the event before any scene
/// exists. The delegate therefore owns the service, the broker, and the
/// chooser window, so wiring does not depend on whether a window was ever
/// shown.
@MainActor
final class VitreoAppDelegate: NSObject, NSApplicationDelegate {
    let service = VitreoService()
    let broker = MacOpenEventBroker()

    private let chooser = ExternalLaunchWindowPresenter()
    private var chooserObserver: AnyCancellable?
    private var errorObserver: AnyCancellable?
    private var launchedToOpenFiles = false
    private var didFinishLaunching = false
    private var isLauncherMode = false
    private var idleTimer: Timer?

    func application(_ application: NSApplication, open urls: [URL]) {
        // Launch Services delivers this before `applicationDidFinishLaunching`
        // on a cold open, which is the only point early enough to stop SwiftUI's
        // window — and the capsule scan its appearance triggers — from starting.
        if !didFinishLaunching {
            launchedToOpenFiles = true
            service.isLauncherMode = true
        }
        broker.submit(urls: urls, source: .launchServices)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        didFinishLaunching = true

        // Nothing runs before the terms are accepted: not the Manager window
        // and not a program Finder asked to open (its request waits in the
        // broker until the observers below exist).
        guard LegalTerms.requireAcceptance() else {
            NSApplication.shared.terminate(nil)
            return
        }

        let coordinator = service.externalLaunches
        chooserObserver = coordinator.$activeRequest.sink { [weak self] request in
            guard let self else { return }
            guard let request else {
                self.chooser.dismiss()
                return
            }
            self.chooser.present(
                request,
                capsules: coordinator.offeredCapsules,
                suggestedCapsule: coordinator.suggestedCapsule,
                onRun: { capsule, remember in
                    coordinator.confirm(
                        request, capsuleName: capsule, remember: remember)
                },
                onCancel: { coordinator.cancel(request) })
        }

        // ContentView owns the in-window alert, but a Finder open runs with no
        // window at all. A failed launch must still say so rather than
        // disappearing silently.
        errorObserver = service.$lastError.sink { [weak self] message in
            guard let self, let message, !message.isEmpty,
                  !self.hasVisibleManagerWindow else { return }
            DispatchQueue.main.async { self.reportWithoutWindow(message) }
        }

        broker.onOverflow = { [weak service] ignored in
            service?.lastError =
                "Vitreo opened the first "
                + "\(MacOpenEventBroker.maximumURLsPerEvent) selected files; "
                + "\(ignored) more were ignored."
        }
        broker.attach { [weak service] url, source in
            service?.externalLaunches.open(url: url, source: source)
        }

        if launchedToOpenFiles {
            enterLauncherMode()
        } else {
            // Opening the manager from Applications publishes the command.
            // An .exe launch is a launcher and exits, so it must not ask for
            // an administrator password on the way out.
            CommandLineLinkInstaller.ensure()
        }
    }

    /// Reopening from Finder, Launchpad, or the Dock is a request for the
    /// Manager itself, so restore the full application.
    func applicationShouldHandleReopen(
        _ application: NSApplication, hasVisibleWindows: Bool
    ) -> Bool {
        leaveLauncherMode()
        return true
    }

    /// Opening an `.exe` is a request to run that program, not to manage
    /// capsules. Vitreo has to start to receive the event — it is the Launch
    /// Services handler — but it behaves like a launcher: no window, no Dock
    /// icon, no menu bar, and it exits once the program it was asked to open is
    /// on its own.
    private func enterLauncherMode() {
        isLauncherMode = true
        service.isLauncherMode = true
        NSApplication.shared.setActivationPolicy(.accessory)
        closeManagerWindows()
        // SwiftUI may not have built its window yet at this point; close again
        // once it has, so none is ever left on screen.
        DispatchQueue.main.async { [weak self] in self?.closeManagerWindows() }
        idleTimer?.invalidate()
        idleTimer = Timer.scheduledTimer(
            withTimeInterval: 3, repeats: true
        ) { [weak self] _ in
            Task { @MainActor [weak self] in self?.quitIfIdle() }
        }
    }

    private func leaveLauncherMode() {
        guard isLauncherMode else { return }
        isLauncherMode = false
        service.isLauncherMode = false
        idleTimer?.invalidate()
        idleTimer = nil
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.activate(ignoringOtherApps: true)
        // The window is about to appear with state nothing has scanned yet.
        Task { @MainActor [service] in await service.refresh() }
    }

    /// Quit only when nothing is left to do. A supervised `vitreo` process must
    /// never be cut short, so a running operation keeps the app alive; a guest
    /// that has re-launched into its own app bundle no longer counts as one.
    private func quitIfIdle() {
        guard isLauncherMode, !hasVisibleManagerWindow,
              !service.hasSupervisedProcesses,
              !service.externalLaunches.hasPendingWork,
              !NSApplication.shared.windows.contains(where: {
                  $0.isVisible && ExternalLaunchWindowPresenter.isChooserWindow($0)
              })
        else { return }
        idleTimer?.invalidate()
        idleTimer = nil
        NSApplication.shared.terminate(nil)
    }

    private var hasVisibleManagerWindow: Bool {
        NSApplication.shared.windows.contains {
            $0.isVisible && !ExternalLaunchWindowPresenter.isChooserWindow($0)
        }
    }

    private func reportWithoutWindow(_ message: String) {
        guard service.lastError == message, !hasVisibleManagerWindow else {
            return
        }
        service.lastError = nil
        let alert = NSAlert()
        alert.messageText = "Vitreo"
        alert.informativeText = message
        alert.alertStyle = .warning
        alert.addButton(withTitle: "OK")
        NSApplication.shared.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    private func closeManagerWindows() {
        for window in NSApplication.shared.windows
        where !ExternalLaunchWindowPresenter.isChooserWindow(window) {
            window.close()
        }
    }
}

@main
struct VitreoManagerApp: App {
    @NSApplicationDelegateAdaptor(VitreoAppDelegate.self) private var delegate

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(delegate.service)
        }
        .windowStyle(.titleBar)
        .defaultSize(width: 820, height: 560)
    }
}
