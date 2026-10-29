// SPDX-License-Identifier: LGPL-2.1-only

import SwiftUI
import UniformTypeIdentifiers

struct AddAppSheet: View {
    enum Mode: String, CaseIterable, Identifiable {
        case installer = "Run Installer"
        case portable = "Portable App"
        var id: String { rawValue }
    }

    @EnvironmentObject var service: VitreoService
    @Environment(\.dismiss) private var dismiss
    @State private var mode: Mode
    @State private var selectedCapsule = ""
    @State private var installerURL: URL?
    @State private var installerArguments = ""
    @State private var installerOperation: UUID?
    @State private var folderURL: URL?
    @State private var executableFiles: [String] = []
    @State private var selectedExecutable = ""
    @State private var appName = ""
    @State private var errorMessage: String?
    @State private var showReplaceConfirmation = false

    init(initialMode: Mode = .installer) {
        _mode = State(initialValue: initialMode)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Add App").font(.title2.bold())
                Spacer()
                Picker("", selection: $mode) {
                    ForEach(Mode.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .fixedSize()
                .disabled(activeOperation?.state == .running)
            }
            .padding(24)
            Divider()

            if mode == .installer {
                installerContent
            } else {
                portableContent
            }

            if let errorMessage {
                Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                    .foregroundStyle(.red)
                    .font(.callout)
                    .padding(.horizontal, 24)
            }

            Divider()
            footer
                .padding(.horizontal, 24)
                .padding(.vertical, 16)
        }
        .frame(width: 620)
        .onAppear {
            if selectedCapsule.isEmpty {
                selectedCapsule = service.defaultCapsuleName
            }
        }
        .confirmationDialog(
            "Replace Existing Portable App?",
            isPresented: $showReplaceConfirmation,
            titleVisibility: .visible
        ) {
            Button("Move Existing Copy to Trash and Replace", role: .destructive) {
                importPortable(replaceExisting: true)
            }
        } message: {
            Text("A folder with this name already exists in the capsule.")
        }
    }

    private var installerContent: some View {
        VStack(alignment: .leading, spacing: 18) {
            formRow("Installer") {
                HStack {
                    Text(installerURL?.lastPathComponent ?? "No file selected")
                        .foregroundStyle(installerURL == nil ? .tertiary : .primary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Spacer()
                    Button("Choose…", action: chooseInstaller)
                }
            }
            formRow("Arguments") {
                TextField("Optional installer arguments", text: $installerArguments)
                    .textFieldStyle(.roundedBorder)
            }
            capsulePicker
            if let operation = activeOperation {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        ProgressView().controlSize(.small)
                            .opacity(operation.state == .running ? 1 : 0)
                        Text(operation.state.rawValue).font(.headline)
                        Spacer()
                        if let exit = operation.exitCode {
                            Text("Exit \(exit)").font(.caption.monospaced())
                        }
                    }
                    ScrollView {
                        Text(operation.output.isEmpty
                             ? "Waiting for installer output…"
                             : operation.output)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                            .padding(10)
                    }
                    .frame(height: 180)
                    .background(.black.opacity(0.86), in: RoundedRectangle(cornerRadius: 8))
                    .foregroundStyle(.white)
                    Text(operation.state == .succeeded
                         ? "Installation finished. The app library has been rescanned."
                         : "Installer output and per-process logs are retained in Activity.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            } else {
                Text("The installer runs inside the selected capsule. Its output, exit status, child processes, and per-process logs will be available in Activity.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(24)
    }

    private var portableContent: some View {
        VStack(alignment: .leading, spacing: 18) {
            formRow("App Folder") {
                HStack {
                    Text(folderURL?.lastPathComponent ?? "No folder selected")
                        .foregroundStyle(folderURL == nil ? .tertiary : .primary)
                    Spacer()
                    Button("Choose…", action: chooseFolder)
                }
            }
            if !executableFiles.isEmpty {
                formRow("Main Executable") {
                    Picker("", selection: $selectedExecutable) {
                        ForEach(executableFiles, id: \.self) { Text($0).tag($0) }
                    }.labelsHidden()
                }
                formRow("App Name") {
                    TextField("Application name", text: $appName)
                        .textFieldStyle(.roundedBorder)
                }
            }
            capsulePicker
            Text("The complete folder is copied into C:\\Vitreo\\Apps. Existing folders are never overwritten without confirmation.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .padding(24)
    }

    private var capsulePicker: some View {
        formRow("Capsule") {
            if service.capsules.isEmpty {
                Text("Create a capsule first").foregroundStyle(.orange)
            } else {
                Picker("", selection: $selectedCapsule) {
                    ForEach(service.capsules) { capsule in
                        Text(capsule.name).tag(capsule.name)
                    }
                }.labelsHidden()
            }
        }
    }

    private var footer: some View {
        HStack {
            if activeOperation?.state == .running {
                Button("Cancel Installer", role: .destructive) {
                    if let installerOperation {
                        service.cancelOperation(installerOperation)
                    }
                }
            } else {
                Button("Cancel", role: .cancel) { dismiss() }
            }
            Spacer()
            if mode == .installer {
                if let operation = activeOperation, operation.state != .running {
                    Button("Done") { dismiss() }
                        .buttonStyle(.borderedProminent)
                } else {
                    Button("Run Installer", action: runInstaller)
                        .buttonStyle(.borderedProminent)
                        .disabled(installerURL == nil || selectedCapsule.isEmpty ||
                                  activeOperation?.state == .running)
                }
            } else {
                Button("Import App") { importPortable(replaceExisting: false) }
                    .buttonStyle(.borderedProminent)
                    .disabled(folderURL == nil || selectedExecutable.isEmpty ||
                              appName.trimmingCharacters(in: .whitespaces).isEmpty ||
                              selectedCapsule.isEmpty)
            }
        }
    }

    private var activeOperation: OperationRecord? {
        guard let installerOperation else { return nil }
        return service.operations.first { $0.id == installerOperation }
    }

    private func formRow<Content: View>(
        _ label: String, @ViewBuilder content: () -> Content
    ) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 12) {
            Text(label).font(.headline).frame(width: 130, alignment: .trailing)
            content().frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func chooseInstaller() {
        let panel = NSOpenPanel()
        panel.title = "Select a Windows Installer"
        panel.allowedContentTypes = [UTType(filenameExtension: "exe") ?? .data]
        panel.canChooseDirectories = false
        if panel.runModal() == .OK { installerURL = panel.url }
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.title = "Select a Portable Application Folder"
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        guard panel.runModal() == .OK, let url = panel.url else { return }
        folderURL = url
        appName = url.lastPathComponent
        executableFiles = scanExecutables(in: url)
        selectedExecutable = executableFiles.first ?? ""
        if executableFiles.isEmpty {
            errorMessage = "No Windows executables were found in this folder."
        } else {
            errorMessage = nil
        }
    }

    private func scanExecutables(in root: URL) -> [String] {
        guard let enumerator = FileManager.default.enumerator(
            at: root,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]) else { return [] }
        var result: [String] = []
        for case let url as URL in enumerator {
            let values = try? url.resourceValues(forKeys: [.isRegularFileKey])
            guard url.pathExtension.lowercased() == "exe",
                  values?.isRegularFile == true else { continue }
            result.append(String(url.path.dropFirst(root.path.count + 1)))
        }
        return result.sorted()
    }

    private func runInstaller() {
        guard let installerURL else { return }
        errorMessage = nil
        installerOperation = service.startInstaller(
            exeURL: installerURL,
            capsuleName: selectedCapsule,
            arguments: splitArguments(installerArguments))
        if installerOperation == nil {
            errorMessage = service.lastError ?? "Could not launch the installer."
        }
    }

    private func importPortable(replaceExisting: Bool) {
        guard let folderURL else { return }
        do {
            try service.addPortableApp(
                folderURL: folderURL,
                mainExe: selectedExecutable,
                appName: appName.trimmingCharacters(in: .whitespacesAndNewlines),
                capsuleName: selectedCapsule,
                replaceExisting: replaceExisting)
            dismiss()
        } catch CocoaError.fileWriteFileExists {
            showReplaceConfirmation = true
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func splitArguments(_ value: String) -> [String] {
        var result: [String] = []
        var current = ""
        var quoted = false
        for character in value {
            if character == "\"" { quoted.toggle() }
            else if character.isWhitespace && !quoted {
                if !current.isEmpty {
                    result.append(current)
                    current = ""
                }
            } else { current.append(character) }
        }
        if !current.isEmpty { result.append(current) }
        return result
    }
}
