// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct CapsuleListView: View {
    @EnvironmentObject var service: VitreoService
    @State private var showCreate = false
    @State private var editRequest: CapsuleEditRequest?
    @State private var detailCapsule: CapsuleInfo?
    @State private var deleteCapsule: CapsuleInfo?
    @State private var importRequest: CapsuleImportRequest?
    @State private var errorMessage: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Capsules").font(.largeTitle.bold())
                    Text("Isolated Windows environments, storage, and mounts.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    chooseCapsuleToImport()
                } label: {
                    Label("Import", systemImage: "square.and.arrow.down")
                }
                Button {
                    showCreate = true
                } label: {
                    Label("New Capsule", systemImage: "plus")
                }
                .buttonStyle(.borderedProminent)
            }
            .padding(24)

            if service.capsules.isEmpty {
                emptyState
            } else {
                ScrollView {
                    LazyVStack(spacing: 8) {
                        ForEach(service.capsules) { capsule in
                            capsuleRow(capsule)
                        }
                    }
                    .padding(.horizontal, 24)
                    .padding(.bottom, 24)
                }
            }
        }
        .sheet(isPresented: $showCreate) {
            CapsuleNameSheet(title: "New Capsule", actionTitle: "Create") { name in
                await service.createCapsule(name: name)
            }
        }
        .sheet(item: $editRequest) { request in
            CapsuleNameSheet(
                title: request.kind == .rename ? "Rename Capsule" : "Clone Capsule",
                actionTitle: request.kind == .rename ? "Rename" : "Clone",
                initialName: request.kind == .rename ? request.capsule.name : "\(request.capsule.name)-copy"
            ) { name in
                if request.kind == .rename {
                    return await service.renameCapsule(request.capsule, to: name)
                }
                return await service.cloneCapsule(request.capsule, to: name)
            }
        }
        .sheet(item: $importRequest) { request in
            CapsuleNameSheet(
                title: "Import Capsule",
                actionTitle: "Import",
                initialName: request.suggestedName
            ) { name in
                await service.importCapsule(from: request.source, name: name)
            }
        }
        .sheet(item: $detailCapsule) { capsule in
            CapsuleDetailView(capsule: capsule)
                .environmentObject(service)
        }
        .confirmationDialog(
            "Remove \(deleteCapsule?.name ?? "Capsule")?",
            isPresented: Binding(
                get: { deleteCapsule != nil },
                set: { if !$0 { deleteCapsule = nil } }),
            titleVisibility: .visible
        ) {
            Button("Move to Trash", role: .destructive) {
                if let capsule = deleteCapsule {
                    Task {
                        errorMessage = await service.removeCapsule(capsule)
                    }
                }
                deleteCapsule = nil
            }
        } message: {
            Text("The capsule and its applications will be moved to the macOS Trash and can be recovered until the Trash is emptied.")
        }
        .alert("Capsule Operation Failed", isPresented: Binding(
            get: { errorMessage != nil },
            set: { if !$0 { errorMessage = nil } }
        )) {
            Button("OK", role: .cancel) { errorMessage = nil }
        } message: {
            Text(errorMessage ?? "")
        }
    }

    private func capsuleRow(_ capsule: CapsuleInfo) -> some View {
        let apps = service.apps.filter { $0.capsuleName == capsule.name }
        let processes = service.guestProcesses.filter { $0.capsule == capsule.name }
        return HStack(spacing: 14) {
            ZStack {
                RoundedRectangle(cornerRadius: 10)
                    .fill(capsule.isHealthy ? Color.blue.gradient : Color.orange.gradient)
                    .frame(width: 46, height: 46)
                Image(systemName: capsule.isHealthy
                      ? "externaldrive.fill"
                      : "externaldrive.badge.exclamationmark")
                    .foregroundStyle(.white)
                    .font(.title2)
            }
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(capsule.name).font(.headline)
                    if capsule.name == service.defaultCapsuleName {
                        Text("DEFAULT")
                            .font(.caption2.bold())
                            .padding(.horizontal, 5)
                            .padding(.vertical, 2)
                            .background(.blue.opacity(0.12), in: Capsule())
                    }
                }
                Text(capsule.rootPath)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            chip("\(apps.count) apps", color: .blue)
            if !processes.isEmpty { chip("\(processes.count) running", color: .green) }
            if !capsule.mounts.isEmpty { chip("\(capsule.mounts.count) mounted", color: .purple) }
            chip(ByteCountFormatter.string(
                fromByteCount: Int64(capsule.sizeBytes), countStyle: .file),
                 color: .secondary)
            Button("Details") { detailCapsule = capsule }
                .buttonStyle(.bordered)
            Menu {
                Button { detailCapsule = capsule } label: {
                    Label("Details…", systemImage: "info.circle")
                }
                Button {
                    editRequest = CapsuleEditRequest(kind: .clone, capsule: capsule)
                } label: {
                    Label("Clone…", systemImage: "square.on.square")
                }
                Button {
                    editRequest = CapsuleEditRequest(kind: .rename, capsule: capsule)
                } label: {
                    Label("Rename…", systemImage: "pencil")
                }
                Button { exportCapsule(capsule) } label: {
                    Label("Export…", systemImage: "square.and.arrow.up")
                }
                Button { service.revealCapsuleInFinder(capsule) } label: {
                    Label("Open in Finder", systemImage: "folder")
                }
                Divider()
                Button(role: .destructive) { deleteCapsule = capsule } label: {
                    Label("Move to Trash…", systemImage: "trash")
                }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
        }
        .padding(14)
        .background(.background, in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(.quaternary))
    }

    private func chip(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.caption2.bold())
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(color.opacity(0.12), in: Capsule())
            .foregroundStyle(color)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "externaldrive.badge.plus")
                .font(.system(size: 48))
                .foregroundStyle(.quaternary)
            Text("No Capsules").font(.title2.bold()).foregroundStyle(.secondary)
            Text("Create an isolated Windows environment to get started.")
                .foregroundStyle(.tertiary)
            Button("Create Capsule") { showCreate = true }
                .buttonStyle(.borderedProminent)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    private func chooseCapsuleToImport() {
        let panel = NSOpenPanel()
        panel.title = "Import Vitreo Capsule"
        panel.prompt = "Import"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let base = url.deletingPathExtension().lastPathComponent
            .replacingOccurrences(of: "Vitreo-", with: "")
            .replacingOccurrences(of: "Vitreo-", with: "")
        importRequest = CapsuleImportRequest(source: url, suggestedName: base)
    }

    private func exportCapsule(_ capsule: CapsuleInfo) {
        let panel = NSSavePanel()
        panel.title = "Export Vitreo Capsule"
        panel.prompt = "Export"
        panel.nameFieldStringValue = "\(capsule.name).vitreocapsule"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        Task {
            errorMessage = await service.exportCapsule(capsule, to: url)
        }
    }
}

private struct CapsuleEditRequest: Identifiable {
    enum Kind { case rename, clone }
    let kind: Kind
    let capsule: CapsuleInfo
    var id: String { "\(kind)-\(capsule.id)" }
}

private struct CapsuleImportRequest: Identifiable {
    let source: URL
    let suggestedName: String
    var id: String { source.path }
}

private struct CapsuleNameSheet: View {
    let title: String
    let actionTitle: String
    let action: (String) async -> String?
    @Environment(\.dismiss) private var dismiss
    @State private var name: String
    @State private var isWorking = false
    @State private var errorMessage: String?

    init(
        title: String,
        actionTitle: String,
        initialName: String = "",
        action: @escaping (String) async -> String?
    ) {
        self.title = title
        self.actionTitle = actionTitle
        self.action = action
        _name = State(initialValue: initialName)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(title).font(.title2.bold())
            TextField("Capsule name", text: $name)
                .textFieldStyle(.roundedBorder)
                .disabled(isWorking)
            if let errorMessage {
                Text(errorMessage).foregroundStyle(.red).font(.callout)
            }
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }.disabled(isWorking)
                Button(actionTitle) {
                    isWorking = true
                    Task {
                        if let error = await action(
                            name.trimmingCharacters(in: .whitespacesAndNewlines)) {
                            errorMessage = error
                            isWorking = false
                        } else {
                            dismiss()
                        }
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(isWorking || name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(24)
        .frame(width: 420)
    }
}

private struct CapsuleDetailView: View {
    let capsule: CapsuleInfo
    @EnvironmentObject var service: VitreoService
    @Environment(\.dismiss) private var dismiss
    @State private var validationMessage: String?
    @State private var isChangingMount = false

    private var currentCapsule: CapsuleInfo {
        service.capsules.first(where: { $0.id == capsule.id }) ?? capsule
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                VStack(alignment: .leading) {
                    Text(currentCapsule.name).font(.title.bold())
                    Text(currentCapsule.isHealthy ? "Healthy capsule" : "Capsule needs repair")
                        .foregroundStyle(currentCapsule.isHealthy ? .green : .orange)
                }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 10) {
                detail("Storage", ByteCountFormatter.string(
                    fromByteCount: Int64(currentCapsule.sizeBytes), countStyle: .file))
                detail("Applications", String(service.apps.filter {
                    $0.capsuleName == currentCapsule.name }.count))
                detail("Processes", String(service.guestProcesses.filter {
                    $0.capsule == currentCapsule.name }.count))
                detail("Modified", currentCapsule.modifiedAt?.formatted() ?? "Unknown")
                detail("Path", currentCapsule.rootPath)
            }
            GroupBox("Mounted Media") {
                VStack(alignment: .leading, spacing: 8) {
                    if currentCapsule.mounts.isEmpty {
                        Text("No optical images mounted.").foregroundStyle(.secondary)
                    } else {
                        ForEach(currentCapsule.mounts) { mount in
                            HStack {
                                Text(mount.drive).font(.headline.monospaced())
                                Text(mount.sourcePath)
                                    .font(.caption.monospaced())
                                    .lineLimit(1)
                                    .truncationMode(.middle)
                                Spacer()
                                Button("Unmount") {
                                    isChangingMount = true
                                    Task {
                                        validationMessage = await service.unmount(
                                            mount, in: currentCapsule)
                                        isChangingMount = false
                                    }
                                }
                                .disabled(isChangingMount)
                            }
                        }
                    }
                    Button {
                        chooseImage()
                    } label: {
                        Label("Mount Optical Image…", systemImage: "opticaldiscdrive")
                    }
                    .disabled(isChangingMount)
                }
                .padding(8)
            }
            if let validationMessage {
                Text(validationMessage).font(.callout)
                    .foregroundStyle(validationMessage == "Capsule is healthy." ? .green : .red)
            }
            HStack {
                Button("Open in Finder") { service.revealCapsuleInFinder(currentCapsule) }
                Button("Validate") {
                    Task {
                        validationMessage = await service.validateCapsule(currentCapsule)
                            ?? "Capsule is healthy."
                    }
                }
                Spacer()
                if service.guestProcesses.contains(
                    where: { $0.capsule == currentCapsule.name }) {
                    Button("Stop Capsule", role: .destructive) {
                        service.stopAll(capsule: currentCapsule.name)
                    }
                }
            }
        }
        .padding(24)
        .frame(minWidth: 620)
    }

    private func detail(_ label: String, _ value: String) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(value).textSelection(.enabled)
        }
    }

    private func chooseImage() {
        let panel = NSOpenPanel()
        panel.title = "Mount Optical Image"
        panel.prompt = "Mount"
        panel.canChooseDirectories = false
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [UTType(filenameExtension: "iso") ?? .data]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        isChangingMount = true
        Task {
            validationMessage = await service.mountImage(url, in: currentCapsule)
            isChangingMount = false
        }
    }
}
