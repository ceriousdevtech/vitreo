// SPDX-License-Identifier: LGPL-2.1-only

import SwiftUI

struct AppListView: View {
    @EnvironmentObject var service: VitreoService
    @State private var addAppMode: AddAppSheet.Mode? = nil
    @State private var searchText = ""
    @State private var favoritesOnly = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            HStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Apps")
                        .font(.largeTitle.bold())
                    Text("Your Windows applications, at home on Mac.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Toggle(isOn: $favoritesOnly) {
                    Image(systemName: favoritesOnly ? "star.fill" : "star")
                }
                .toggleStyle(.button)
                .help("Show favorites only")
                .accessibilityLabel("Favorites only")
                installButton
            }
            .padding(.horizontal, 24)
            .padding(.top, 20)
            .padding(.bottom, 16)

            if !service.hasRefreshed && service.apps.isEmpty {
                ProgressView("Loading apps…")
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if service.apps.isEmpty {
                emptyState
            } else if filteredApps.isEmpty {
                ContentUnavailableView {
                    Label(searchText.isEmpty ? "No Favorites Yet" : "No Matching Apps",
                          systemImage: searchText.isEmpty ? "star" : "magnifyingglass")
                } description: {
                    Text(searchText.isEmpty
                         ? "Add favorites from an app’s options menu to keep them close."
                         : "Try a different search or show all your apps.")
                } actions: {
                    Button("Show All Apps") {
                        searchText = ""
                        favoritesOnly = false
                    }
                }
            } else {
                // App list
                ScrollView {
                    LazyVStack(spacing: 10) {
                        ForEach(filteredApps) { app in
                            AppRowView(app: app)
                        }
                    }
                    .padding(.horizontal, 24)
                    .padding(.vertical, 8)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Color(nsColor: .windowBackgroundColor))
        .sheet(item: $addAppMode) { mode in
            AddAppSheet(initialMode: mode)
                .environmentObject(service)
        }
        .searchable(text: $searchText, prompt: "Search apps or capsules")
    }

    private var filteredApps: [InstalledApp] {
        service.apps.filter { app in
            (!favoritesOnly || app.isFavorite) &&
            (searchText.isEmpty ||
             app.name.localizedCaseInsensitiveContains(searchText) ||
             app.capsuleName.localizedCaseInsensitiveContains(searchText) ||
             app.windowsExecutablePath.localizedCaseInsensitiveContains(searchText))
        }.sorted {
            if $0.isFavorite != $1.isFavorite { return $0.isFavorite }
            return $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
        }
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "square.grid.2x2.fill")
                .font(.system(size: 48))
                .foregroundStyle(.quaternary)
            Text("No Apps Installed")
                .font(.title2.bold())
                .foregroundStyle(.secondary)
            Text("Install a Windows application to get started.")
                .foregroundStyle(.tertiary)
            installButton
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    private var installButton: some View {
        Menu {
            Button("Run Installer…") { addAppMode = .installer }
            Button("Add Portable App…") { addAppMode = .portable }
        } label: {
            HStack(spacing: 6) {
                Image(systemName: "plus")
                Text("Add App")
            }
        }
        .fixedSize()
    }
}
