// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import SwiftUI

struct AppRowView: View {
    let app: InstalledApp
    @EnvironmentObject var service: VitreoService
    @State private var icon: NSImage?
    @State private var showRemove = false
    @State private var showUninstall = false
    @State private var showProfile = false
    @State private var removeError: String?

    var body: some View {
        HStack(spacing: 14) {
            appIcon
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(app.name).font(.headline)
                        .lineLimit(1)
                        .help(app.name)
                    if app.isFavorite {
                        Image(systemName: "star.fill")
                            .font(.caption)
                            .foregroundStyle(.yellow)
                    }
                }
                Text("\(app.capsuleName) · \(app.architecture.rawValue) · \(app.compatibility.rawValue)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .help("\(app.capsuleName) · \(app.architecture.rawValue) · \(app.compatibility.rawValue)")
                if !app.launchProfile.arguments.isEmpty {
                    Text(app.launchProfile.arguments.joined(separator: " "))
                        .font(.caption.monospaced())
                        .foregroundStyle(.tertiary)
                        .lineLimit(1)
                }
            }
            Spacer()
            statusBadge
            Button(app.status == .running ? "Stop" : "Open") {
                app.status == .running ? service.stopApp(app) : service.launchApp(app)
            }
            .buttonStyle(.bordered)
            .controlSize(.regular)
            .frame(minWidth: 64)
            .help(app.status == .running ? "Stop \(app.name)" : "Open \(app.name)")
            Menu {
                Button {
                    service.toggleFavorite(app)
                } label: {
                    Label(
                        app.isFavorite ? "Remove from Favorites" : "Add to Favorites",
                        systemImage: app.isFavorite ? "star.slash" : "star")
                }
                Button { showProfile = true } label: {
                    Label("Launch Settings…", systemImage: "slider.horizontal.3")
                }
                Menu("Compatibility") {
                    ForEach(CompatibilityStatus.allCases) { status in
                        Button {
                            service.setCompatibility(app, status: status)
                        } label: {
                            if status == app.compatibility {
                                Label(status.rawValue, systemImage: "checkmark")
                            } else {
                                Text(status.rawValue)
                            }
                        }
                    }
                }
                Divider()
                Button { service.revealInFinder(app) } label: {
                    Label("Show in Finder", systemImage: "folder")
                }
                Button { createLauncher() } label: {
                    Label("Create macOS Launcher…", systemImage: "macwindow.badge.plus")
                }
                Divider()
                if app.isPortable {
                    Button(role: .destructive) { showRemove = true } label: {
                        Label("Remove App…", systemImage: "trash")
                    }
                } else if app.uninstallInfo != nil {
                    Button(role: .destructive) { showUninstall = true } label: {
                        Label("Run Uninstaller…", systemImage: "trash")
                    }
                } else {
                    Button(role: .destructive) { showRemove = true } label: {
                        Label("Remove Shortcut…", systemImage: "trash")
                    }
                }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .frame(width: 26)
            .accessibilityLabel("Options for \(app.name)")
        }
        .padding(16)
        .background(.background, in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(.primary.opacity(0.06)))
        .task(id: app.executablePath) {
            let path = app.executablePath
            icon = await Task.detached(priority: .background) {
                extractPEIcon(at: URL(fileURLWithPath: path))
            }.value
        }
        .sheet(isPresented: $showProfile) {
            LaunchProfileSheet(app: app)
                .environmentObject(service)
        }
        .confirmationDialog(
            app.isPortable ? "Remove \(app.name)?" : "Remove this shortcut?",
            isPresented: $showRemove, titleVisibility: .visible
        ) {
            Button(app.isPortable ? "Move App to Trash" : "Remove Shortcut",
                   role: .destructive) {
                removeError = service.removeApp(app)
            }
        } message: {
            Text(app.isPortable
                 ? "The imported app folder and shortcut will be moved to the Trash."
                 : "The application files will remain in the capsule.")
        }
        .confirmationDialog(
            "Uninstall \(app.uninstallInfo?.displayName ?? app.name)?",
            isPresented: $showUninstall, titleVisibility: .visible
        ) {
            Button("Run Uninstaller", role: .destructive) {
                _ = service.uninstallApp(app)
            }
        } message: {
            Text("The exact Windows uninstall command, including its arguments, will run inside this capsule.")
        }
        .alert("App Operation Failed", isPresented: Binding(
            get: { removeError != nil },
            set: { if !$0 { removeError = nil } }
        )) {
            Button("OK", role: .cancel) { removeError = nil }
        } message: {
            Text(removeError ?? "")
        }
    }

    private var appIcon: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10)
                .fill(icon == nil ? Color.indigo.gradient : Color.gray.opacity(0.12).gradient)
                .frame(width: 46, height: 46)
            if let icon {
                Image(nsImage: icon)
                    .resizable()
                    .interpolation(.high)
                    .scaledToFit()
                    .frame(width: 34, height: 34)
            } else {
                Image(systemName: "window.casement")
                    .font(.title2)
                    .foregroundStyle(.white)
            }
        }
    }

    private var statusBadge: some View {
        HStack(spacing: 5) {
            Circle()
                .fill(app.status == .running ? .green : .gray)
                .frame(width: 8, height: 8)
            Text(app.status.rawValue).font(.caption)
        }
        .foregroundStyle(app.status == .running ? .green : .secondary)
    }

    private func createLauncher() {
        let panel = NSOpenPanel()
        panel.title = "Choose a Folder for the Launcher"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let destination = panel.url else { return }
        do {
            try service.createMacLauncher(for: app, in: destination)
        } catch {
            removeError = error.localizedDescription
        }
    }
}

private struct LaunchProfileSheet: View {
    let app: InstalledApp
    @EnvironmentObject var service: VitreoService
    @Environment(\.dismiss) private var dismiss
    @State private var arguments: String
    @State private var workingDirectory: String
    @State private var environment: String
    @State private var interpreter: Bool
    @State private var trace: Bool

    init(app: InstalledApp) {
        self.app = app
        _arguments = State(initialValue: app.launchProfile.arguments.joined(separator: " "))
        _workingDirectory = State(initialValue: app.launchProfile.workingDirectory)
        _environment = State(initialValue: app.launchProfile.environment
            .sorted(by: { $0.key < $1.key })
            .map { "\($0.key)=\($0.value)" }.joined(separator: "\n"))
        _interpreter = State(initialValue: app.launchProfile.forceInterpreter)
        _trace = State(initialValue: app.launchProfile.enableTrace)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Launch Settings").font(.title2.bold())
            Text(app.windowsExecutablePath)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
            Form {
                TextField("Arguments", text: $arguments)
                TextField("Working directory", text: $workingDirectory)
                VStack(alignment: .leading) {
                    Text("Environment overrides (one NAME=VALUE per line)")
                    TextEditor(text: $environment)
                        .font(.body.monospaced())
                        .frame(height: 100)
                        .border(.quaternary)
                }
                Toggle("Force interpreter mode", isOn: $interpreter)
                Toggle("Enable API tracing", isOn: $trace)
            }
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                Button("Save") {
                    service.updateLaunchProfile(
                        app,
                        profile: LaunchProfile(
                            arguments: splitArguments(arguments),
                            workingDirectory: workingDirectory,
                            environment: parseEnvironment(environment),
                            forceInterpreter: interpreter,
                            enableTrace: trace))
                    dismiss()
                }
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(24)
        .frame(width: 560)
    }

    private func splitArguments(_ value: String) -> [String] {
        var values: [String] = []
        var current = ""
        var quoted = false
        for character in value {
            if character == "\"" { quoted.toggle() }
            else if character.isWhitespace && !quoted {
                if !current.isEmpty {
                    values.append(current)
                    current = ""
                }
            } else { current.append(character) }
        }
        if !current.isEmpty { values.append(current) }
        return values
    }

    private func parseEnvironment(_ value: String) -> [String: String] {
        var result: [String: String] = [:]
        for line in value.components(separatedBy: .newlines) {
            guard let separator = line.firstIndex(of: "=") else { continue }
            let name = String(line[..<separator]).trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty else { continue }
            result[name] = String(line[line.index(after: separator)...])
        }
        return result
    }
}
