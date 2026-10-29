// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import Darwin
import SwiftUI

private func sampleProcess(pid: pid_t) -> (memory: UInt64, cpu: UInt64)? {
    guard pid > 0 else { return nil }
    var info = proc_taskinfo()
    let size = Int32(MemoryLayout<proc_taskinfo>.size)
    guard proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &info, size) == size else {
        return nil
    }
    return (info.pti_resident_size, info.pti_total_user + info.pti_total_system)
}

struct ActivityView: View {
    private enum Section: String, CaseIterable, Identifiable {
        case processes = "Processes"
        case history = "History & Logs"
        var id: String { rawValue }
    }

    @EnvironmentObject var service: VitreoService
    @State private var section = Section.processes
    @State private var memory: [Int32: UInt64] = [:]
    @State private var cpu: [Int32: Double] = [:]
    @State private var previous: [Int32: (UInt64, Date)] = [:]
    @State private var selectedOperation: OperationRecord?
    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .bottom) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Activity").font(.largeTitle.bold())
                    Text("Guest process trees, operations, and diagnostic logs.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Picker("", selection: $section) {
                    ForEach(Section.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .fixedSize()
                if !service.guestProcesses.isEmpty {
                    Button("Stop All", role: .destructive) { service.stopAll() }
                        .buttonStyle(.bordered)
                }
            }
            .padding(24)

            if section == .processes {
                processContent
            } else {
                historyContent
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .onReceive(timer) { sample(at: $0) }
        .sheet(item: $selectedOperation) { operation in
            OperationLogView(operation: operation)
                .environmentObject(service)
        }
    }

    @ViewBuilder
    private var processContent: some View {
        if service.guestProcesses.isEmpty {
            emptyState(
                icon: "chart.xyaxis.line",
                title: "No Guest Processes",
                detail: "Apps launched from the Manager or command line will appear here.")
        } else {
            ScrollView {
                LazyVStack(spacing: 8) {
                    ForEach(service.guestProcesses) { process in
                        processRow(process)
                    }
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 24)
            }
        }
    }

    private func processRow(_ process: GuestProcessInfo) -> some View {
        HStack(spacing: 14) {
            Image(systemName: process.parentPid == 0 ? "app.fill" : "arrow.turn.down.right")
                .frame(width: 28)
                .foregroundStyle(process.parentPid == 0 ? .green : .secondary)
            VStack(alignment: .leading, spacing: 3) {
                Text(process.imageName).font(.headline)
                Text(process.imagePath)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Text(process.capsule)
                .font(.caption.bold())
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(.blue.opacity(0.12), in: Capsule())
            metric(cpu[process.pid].map { String(format: "%.1f%%", $0) } ?? "—")
            metric(memory[process.pid].map {
                ByteCountFormatter.string(
                    fromByteCount: Int64($0), countStyle: .memory)
            } ?? "—")
            VStack(alignment: .trailing, spacing: 2) {
                Text("PID \(process.pid)").font(.caption.monospaced())
                Text("Parent \(process.parentPid)")
                    .font(.caption2.monospaced())
                    .foregroundStyle(.secondary)
            }
            Button("Stop", role: .destructive) { service.stopProcess(process) }
                .buttonStyle(.bordered)
        }
        .padding(14)
        .background(.background, in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(.quaternary))
    }

    private var historyContent: some View {
        Group {
            if service.operations.isEmpty {
                emptyState(
                    icon: "doc.text.magnifyingglass",
                    title: "No Operations Yet",
                    detail: "Launches, installations, uninstalls, and exports will be recorded here.")
            } else {
                ScrollView {
                    LazyVStack(spacing: 8) {
                        ForEach(service.operations) { operation in
                            Button { selectedOperation = operation } label: {
                                operationRow(operation)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    .padding(.horizontal, 24)
                    .padding(.bottom, 24)
                }
            }
        }
    }

    private func operationRow(_ operation: OperationRecord) -> some View {
        HStack(spacing: 14) {
            Image(systemName: operationIcon(operation))
                .font(.title3)
                .foregroundStyle(operationColor(operation))
                .frame(width: 30)
            VStack(alignment: .leading, spacing: 3) {
                Text(operation.title).font(.headline)
                Text("\(operation.kind.rawValue) • \(operation.startedAt.formatted())")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if !operation.capsule.isEmpty {
                Text(operation.capsule).font(.caption).foregroundStyle(.secondary)
            }
            Text(operation.state.rawValue)
                .font(.caption.bold())
                .foregroundStyle(operationColor(operation))
            if operation.state == .running {
                Button("Cancel", role: .destructive) {
                    service.cancelOperation(operation.id)
                }
                .buttonStyle(.bordered)
            } else {
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
            }
        }
        .padding(14)
        .background(.background, in: RoundedRectangle(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(.quaternary))
    }

    private func metric(_ text: String) -> some View {
        Text(text)
            .font(.callout.monospaced())
            .frame(width: 78, alignment: .trailing)
    }

    private func emptyState(icon: String, title: String, detail: String) -> some View {
        VStack(spacing: 14) {
            Spacer()
            Image(systemName: icon).font(.system(size: 48)).foregroundStyle(.quaternary)
            Text(title).font(.title2.bold()).foregroundStyle(.secondary)
            Text(detail).foregroundStyle(.tertiary)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    private func sample(at date: Date) {
        for process in service.guestProcesses {
            guard let sample = sampleProcess(pid: pid_t(process.pid)) else { continue }
            memory[process.pid] = sample.memory
            if let prior = previous[process.pid] {
                let elapsed = date.timeIntervalSince(prior.1) * 1_000_000_000
                if elapsed > 0 {
                    cpu[process.pid] = Double(sample.cpu &- prior.0) / elapsed * 100
                }
            }
            previous[process.pid] = (sample.cpu, date)
        }
        let active = Set(service.guestProcesses.map(\.pid))
        memory = memory.filter { active.contains($0.key) }
        cpu = cpu.filter { active.contains($0.key) }
        previous = previous.filter { active.contains($0.key) }
    }

    private func operationIcon(_ operation: OperationRecord) -> String {
        switch operation.state {
        case .running: return "arrow.triangle.2.circlepath"
        case .succeeded: return "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        case .cancelled: return "xmark.circle.fill"
        }
    }

    private func operationColor(_ operation: OperationRecord) -> Color {
        switch operation.state {
        case .running: return .blue
        case .succeeded: return .green
        case .failed: return .red
        case .cancelled: return .orange
        }
    }
}

private struct OperationLogView: View {
    let operation: OperationRecord
    @EnvironmentObject var service: VitreoService
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading) {
                    Text(operation.title).font(.title2.bold())
                    Text("\(operation.state.rawValue) • exit \(operation.exitCode.map(String.init) ?? "—")")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if operation.logDirectory != nil {
                    Button("Show Log Files") { service.revealOperationLogs(operation) }
                }
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            ScrollView {
                Text(operation.output.isEmpty ? "No console output was produced." : operation.output)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .topLeading)
                    .padding(12)
            }
            .background(Color.black.opacity(0.86), in: RoundedRectangle(cornerRadius: 8))
            .foregroundStyle(.white)
        }
        .padding(20)
        .frame(minWidth: 720, minHeight: 480)
    }
}
