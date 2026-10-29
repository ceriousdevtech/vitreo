// SPDX-License-Identifier: LGPL-2.1-only

import SwiftUI

enum SidebarItem: String, CaseIterable, Identifiable {
    case apps = "Apps"
    case capsules = "Capsules"
    case activity = "Activity"
    case settings = "Settings"

    var id: String { rawValue }

    var icon: String {
        switch self {
        case .apps:     return "square.grid.2x2"
        case .capsules: return "externaldrive"
        case .activity: return "chart.xyaxis.line"
        case .settings: return "gearshape"
        }
    }
}

struct ContentView: View {
    @EnvironmentObject var service: VitreoService
    @State private var selection: SidebarItem = .apps
    @State private var showHelp = false

    var body: some View {
        NavigationSplitView {
            SidebarView(selection: $selection)
                .navigationSplitViewColumnWidth(min: 180, ideal: 210, max: 260)
        } detail: {
            detailView
        }
        .navigationTitle(selection.rawValue)
        .navigationSplitViewStyle(.balanced)
        .frame(minWidth: 700, minHeight: 450)
        // The full capsule scan is driven by the UI appearing, not by service
        // construction: opening an .exe from Finder shows no window and must
        // not pay for scanning every capsule. SwiftUI still builds this view
        // during a launcher-mode launch, so the flag is checked here too.
        .task {
            guard !service.isLauncherMode else { return }
            await service.refresh()
        }
        .toolbar {
            ToolbarItemGroup(placement: .automatic) {
                Button(action: {
                    Task { await service.refresh() }
                }) {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(service.isRefreshing)
                .help("Refresh library")
                .accessibilityLabel("Refresh library")
                Button(action: { showHelp = true }) {
                    Image(systemName: "questionmark.circle")
                }
                .help("Vitreo Help")
                .accessibilityLabel("Vitreo Help")
            }
        }
        .sheet(isPresented: $showHelp) {
            HelpView()
        }
        .alert("Vitreo", isPresented: Binding(
            get: { service.lastError != nil },
            set: { if !$0 { service.lastError = nil } }
        )) {
            Button("OK", role: .cancel) { service.lastError = nil }
        } message: {
            Text(service.lastError ?? "")
        }
    }

    @ViewBuilder
    private var detailView: some View {
        switch selection {
        case .apps:
            AppListView()
        case .capsules:
            CapsuleListView()
        case .activity:
            ActivityView()
        case .settings:
            SettingsView()
        }
    }
}

private struct HelpView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Image("VitreoMark")
                    .resizable()
                    .frame(width: 44, height: 44)
                VStack(alignment: .leading) {
                    Text("Vitreo").font(.title.bold())
                    Text("Manage isolated Windows applications on macOS.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            GroupBox("Getting Started") {
                Text("Create or select a capsule, then use Add App to run an installer or import a portable application. Launches and installer logs appear under Activity.")
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
            GroupBox("Troubleshooting") {
                Text("Use Activity → History & Logs for console output. Settings can force interpreter mode, enable API tracing, verify the engine, and export a diagnostic bundle.")
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
        }
        .padding(24)
        .frame(width: 560)
    }
}
