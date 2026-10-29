// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

struct CLIResult: Sendable {
    let exitCode: Int32
    let standardOutput: String
    let standardError: String

    var combinedOutput: String {
        [standardOutput, standardError]
            .filter { !$0.isEmpty }
            .joined(separator: standardOutput.isEmpty || standardError.isEmpty ? "" : "\n")
    }
}

enum CLIClientError: LocalizedError {
    case executableMissing(String)
    case invalidOutput(String)

    var errorDescription: String? {
        switch self {
        case .executableMissing(let path):
            return "The Vitreo engine is not executable at \(path)."
        case .invalidOutput(let output):
            return "Vitreo returned invalid management data: \(output)"
        }
    }
}

actor VitreoCLIClient {
    func run(
        binary: String,
        arguments: [String],
        environment: [String: String] = [:]
    ) async throws -> CLIResult {
        guard FileManager.default.isExecutableFile(atPath: binary) else {
            throw CLIClientError.executableMissing(binary)
        }

        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                let process = Process()
                let output = Pipe()
                let error = Pipe()
                process.executableURL = URL(fileURLWithPath: binary)
                process.arguments = arguments
                if !environment.isEmpty {
                    process.environment = ProcessInfo.processInfo.environment.merging(
                        environment, uniquingKeysWith: { _, override in override })
                }
                process.standardOutput = output
                process.standardError = error
                process.terminationHandler = { process in
                    let stdout = String(
                        data: output.fileHandleForReading.readDataToEndOfFile(),
                        encoding: .utf8) ?? ""
                    let stderr = String(
                        data: error.fileHandleForReading.readDataToEndOfFile(),
                        encoding: .utf8) ?? ""
                    continuation.resume(returning: CLIResult(
                        exitCode: process.terminationStatus,
                        standardOutput: stdout.trimmingCharacters(in: .whitespacesAndNewlines),
                        standardError: stderr.trimmingCharacters(in: .whitespacesAndNewlines)))
                }
                do {
                    try process.run()
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        } onCancel: {
            // Short management commands have no durable resources. Long-running
            // guest processes are supervised by VitreoService, which owns and
            // terminates the concrete Process object.
        }
    }

    func engineStatus(binary: String) async -> EngineStatus {
        guard FileManager.default.isExecutableFile(atPath: binary) else {
            return EngineStatus(
                state: .missing, path: binary, version: "",
                message: "Build the Vitreo engine or select a binary in Settings.")
        }
        do {
            let result = try await run(binary: binary, arguments: ["version"])
            guard result.exitCode == 0 else {
                return EngineStatus(
                    state: .incompatible, path: binary, version: "",
                    message: result.combinedOutput)
            }
            return EngineStatus(
                state: .ready, path: binary, version: result.standardOutput,
                message: "Engine ready")
        } catch {
            return EngineStatus(
                state: .incompatible, path: binary, version: "",
                message: error.localizedDescription)
        }
    }

    func listProcesses(binary: String, capsules: [String]) async -> [GuestProcessInfo] {
        var result: [GuestProcessInfo] = []
        for capsule in capsules {
            do {
                let command = try await run(
                    binary: binary,
                    arguments: ["process", "list", "--capsule", capsule, "--json"])
                guard command.exitCode == 0,
                      let data = command.standardOutput.data(using: .utf8) else { continue }
                result.append(contentsOf: try JSONDecoder().decode(
                    [GuestProcessInfo].self, from: data))
            } catch {
                continue
            }
        }
        return result.sorted {
            if $0.capsule == $1.capsule { return $0.pid < $1.pid }
            return $0.capsule.localizedCaseInsensitiveCompare($1.capsule) == .orderedAscending
        }
    }
}
