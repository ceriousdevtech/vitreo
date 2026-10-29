// SPDX-License-Identifier: LGPL-2.1-only

import SwiftUI

struct SidebarView: View {
    @Binding var selection: SidebarItem
    @EnvironmentObject var service: VitreoService

    var body: some View {
        VStack(spacing: 0) {
            List(selection: $selection) {
                Section("Library") {
                    sidebarItem(.apps)
                    sidebarItem(.capsules)
                }
                Section("System") {
                    sidebarItem(.activity)
                    sidebarItem(.settings)
                }
            }
            .listStyle(.sidebar)

            Divider()

            // Running apps footer
            runningAppsFooter
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
        }
    }

    private func sidebarItem(_ item: SidebarItem) -> some View {
        Label(item.rawValue, systemImage: item.icon)
            .tag(item)
    }

    private var runningAppsFooter: some View {
        let running = service.apps.filter { $0.status == .running }
        return HStack(spacing: 8) {
            Circle()
                .fill(running.isEmpty ? Color.gray : Color.green)
                .frame(width: 8, height: 8)

            VStack(alignment: .leading, spacing: 1) {
                if running.isEmpty {
                    Text("No apps running")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    Text(running.count == 1
                         ? "1 app running"
                         : "\(running.count) apps running")
                        .font(.caption.bold())
                        .foregroundStyle(.primary)
                    Text(running.map(\.name).joined(separator: ", "))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
            }

            Spacer()

            if !running.isEmpty {
                Button(role: .destructive) {
                    service.stopAll()
                } label: {
                    Text("Stop All")
                        .font(.caption)
                }
                .buttonStyle(.borderless)
            }
        }
    }
}
