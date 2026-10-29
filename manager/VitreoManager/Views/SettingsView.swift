// SPDX-License-Identifier: LGPL-2.1-only

import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var service: VitreoService

    // MARK: General
    @AppStorage("defaultCapsule") private var defaultCapsule: String = "default"
    @AppStorage("refreshInterval") private var refreshInterval: Double = 5.0

    // MARK: Engine
    @AppStorage("vitreoBinaryOverride") private var binaryOverride: String = ""
    @AppStorage("useInterpreterMode") private var useInterpreterMode: Bool = false
    @AppStorage("enableTrace") private var enableTrace: Bool = false

    // MARK: Finder
    @AppStorage("externalLaunchPolicy")
    private var externalLaunchPolicy: ExternalLaunchPolicy = .askEveryTime

    @State private var showBinaryPicker = false
    @State private var rememberedRules = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            VStack(alignment: .leading, spacing: 4) {
                Text("Settings")
                    .font(.largeTitle.bold())
                Text("Engine and application configuration.")
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 24)
            .padding(.top, 20)
            .padding(.bottom, 20)

            ScrollView {
                VStack(spacing: 20) {
                    generalSection
                    finderSection
                    engineSection
                    diagnosticsSection
                    aboutSection
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 24)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    // MARK: - General Section

    private var generalSection: some View {
        SettingsSection(title: "General") {
            // Default capsule
            SettingsRow(label: "Default Capsule",
                        detail: "Capsule used when installing new apps") {
                if service.capsules.isEmpty {
                    Text("No capsules").foregroundStyle(.tertiary)
                } else {
                    Picker("", selection: $defaultCapsule) {
                        ForEach(service.capsules) { cap in
                            Text(cap.name).tag(cap.name)
                        }
                    }
                    .labelsHidden()
                    .frame(maxWidth: 200)
                }
            }

            Divider().padding(.leading, 140)

            // Refresh interval
            SettingsRow(label: "Refresh Interval",
                        detail: "How often the app list is refreshed") {
                Picker("", selection: $refreshInterval) {
                    Text("2 seconds").tag(2.0)
                    Text("5 seconds").tag(5.0)
                    Text("10 seconds").tag(10.0)
                    Text("30 seconds").tag(30.0)
                    Text("60 seconds").tag(60.0)
                }
                .labelsHidden()
                .frame(maxWidth: 200)
                .onChange(of: refreshInterval) { _, _ in
                    service.restartAutoRefresh()
                }
            }
        }
    }

    // MARK: - Finder Section

    private var finderSection: some View {
        SettingsSection(title: "Opening Files from Finder") {
            SettingsRow(
                label: "External Executables",
                detail: "How Vitreo picks a capsule for an .exe that is not "
                    + "already inside one"
            ) {
                Picker("", selection: $externalLaunchPolicy) {
                    ForEach(ExternalLaunchPolicy.allCases) { policy in
                        Text(policy.title).tag(policy)
                    }
                }
                .labelsHidden()
                .frame(maxWidth: 220)
            }

            Divider().padding(.leading, 140)

            SettingsRow(
                label: "Remembered Capsules",
                detail: "Executables set to always run in a particular capsule"
            ) {
                HStack(spacing: 8) {
                    Text(rememberedRuleSummary).foregroundStyle(.secondary)
                    Button("Clear") {
                        service.externalLaunches.forgetAllRememberedCapsules()
                        rememberedRules = 0
                    }
                    .disabled(rememberedRules == 0)
                }
            }

            Divider().padding(.leading, 140)

            SettingsRow(
                label: "Open With",
                detail: "An executable stored inside a capsule always runs in "
                    + "that capsule"
            ) {
                Text(
                    "Right-click an .exe or a Windows installer "
                    + "(.msi, .msp, .msix, .appx, .msu) in Finder and choose "
                    + "Open With ▸ Vitreo. Use Get Info to make Vitreo the default.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .onAppear { rememberedRules = service.externalLaunches.rememberedRuleCount }
    }

    private var rememberedRuleSummary: String {
        rememberedRules == 1 ? "1 executable" : "\(rememberedRules) executables"
    }

    // MARK: - Engine Section

    private var engineSection: some View {
        SettingsSection(title: "Engine") {
            SettingsRow(label: "Status",
                        detail: service.engineStatus.message) {
                Label(
                    engineStatusText,
                    systemImage: service.engineStatus.state == .ready
                        ? "checkmark.circle.fill"
                        : "exclamationmark.triangle.fill")
                    .foregroundStyle(
                        service.engineStatus.state == .ready ? .green : .orange)
            }

            Divider().padding(.leading, 140)

            // Binary path
            SettingsRow(label: "Vitreo Engine",
                        detail: "Path to the Vitreo engine executable") {
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 8) {
                        Text(resolvedBinaryDisplay)
                            .font(.system(.body, design: .monospaced))
                            .foregroundStyle(binaryOverride.isEmpty ? .secondary : .primary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Button("Choose…") { pickBinary() }
                        if !binaryOverride.isEmpty {
                            Button("Reset") {
                                binaryOverride = ""
                                Task { await service.refresh() }
                            }
                                .foregroundStyle(.red)
                        }
                    }
                    if !binaryOverride.isEmpty && !FileManager.default.isExecutableFile(atPath: binaryOverride) {
                        Label("File not found or not executable", systemImage: "exclamationmark.triangle.fill")
                            .font(.caption)
                            .foregroundStyle(.orange)
                    }
                }
            }

            Divider().padding(.leading, 140)

            // Interpreter mode
            SettingsRow(label: "Interpreter Mode",
                        detail: "Disable JIT — slower but more compatible") {
                Toggle("", isOn: $useInterpreterMode).labelsHidden()
            }

            Divider().padding(.leading, 140)

            // Trace mode
            SettingsRow(label: "API Tracing",
                        detail: "Log every Windows API call (verbose, for debugging)") {
                Toggle("", isOn: $enableTrace).labelsHidden()
            }
        }
    }

    private var diagnosticsSection: some View {
        SettingsSection(title: "Diagnostics") {
            SettingsRow(label: "Diagnostic Bundle",
                        detail: "Export engine, capsule, process, and operation summaries") {
                Button("Export…") { exportDiagnostics() }
            }
            Divider().padding(.leading, 140)
            SettingsRow(label: "Operation History",
                        detail: "Launches and installers retained for this Manager session") {
                Text("\(service.operations.count) operations")
                    .foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - About Section

    private var aboutSection: some View {
        SettingsSection(title: "About") {
            SettingsRow(label: "Version", detail: "") {
                Text(appVersion)
                    .foregroundStyle(.secondary)
            }
            Divider().padding(.leading, 140)
            SettingsRow(label: "Engine", detail: "") {
                Text(service.engineStatus.version.isEmpty
                     ? service.vitreoBinaryPath
                     : service.engineStatus.version)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Divider().padding(.leading, 140)
            SettingsRow(label: "Legal", detail: "") {
                HStack(spacing: 8) {
                    Button("License") { openBundled("LICENSE") }
                    Button("Third-Party Notices") { openBundled("third-party-notices") }
                }
            }
            Divider()
            VStack(alignment: .leading, spacing: 10) {
                ForEach(LegalTerms.paragraphs, id: \.self) { paragraph in
                    Text(paragraph)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
            }
            .padding(16)
        }
    }

    private func openBundled(_ name: String) {
        if !LegalTerms.openBundled(name) {
            service.lastError = "\(name) is missing from the app bundle."
        }
    }

    // MARK: - Helpers

    private var resolvedBinaryDisplay: String {
        binaryOverride.isEmpty ? service.vitreoBinaryPath : binaryOverride
    }

    private var engineStatusText: String {
        switch service.engineStatus.state {
        case .ready: return "Ready"
        case .missing: return "Missing"
        case .incompatible: return "Unavailable"
        }
    }

    private var appVersion: String {
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "—"
        let b = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "—"
        return "\(v) (\(b))"
    }

    private func pickBinary() {
        let panel = NSOpenPanel()
        panel.title = "Select Vitreo Engine"
        panel.message = "Choose the Vitreo engine executable"
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            binaryOverride = url.path
            Task { await service.refresh() }
        }
    }

    private func exportDiagnostics() {
        let panel = NSOpenPanel()
        panel.title = "Choose Diagnostic Export Folder"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try service.exportDiagnostics(to: url)
        } catch {
            service.lastError = error.localizedDescription
        }
    }
}

// MARK: - Layout Components

private struct SettingsSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(title)
                .font(.headline)
                .foregroundStyle(.secondary)
                .padding(.bottom, 8)
                .padding(.leading, 2)

            VStack(spacing: 0) {
                content()
            }
            .background(
                RoundedRectangle(cornerRadius: 10)
                    .fill(.background)
                    .shadow(color: .black.opacity(0.06), radius: 2, y: 1)
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10)
                    .strokeBorder(.quaternary, lineWidth: 0.5)
            )
        }
    }
}

private struct SettingsRow<Control: View>: View {
    let label: String
    let detail: String
    @ViewBuilder let control: () -> Control

    var body: some View {
        HStack(alignment: .center, spacing: 16) {
            VStack(alignment: .trailing, spacing: 2) {
                Text(label)
                    .font(.body)
                if !detail.isEmpty {
                    Text(detail)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
            }
            .frame(width: 130, alignment: .trailing)

            control()
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }
}
