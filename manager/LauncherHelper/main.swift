// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

struct LauncherConfiguration: Decodable {
    let executable: String
    let arguments: [String]
}

private func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("Vitreo Launcher: \(message)\n".utf8))
    exit(EXIT_FAILURE)
}

guard let configurationURL = Bundle.main.url(
    forResource: "LauncherConfig", withExtension: "json") else {
    fail("LauncherConfig.json is missing from the application bundle.")
}

let configuration: LauncherConfiguration
do {
    configuration = try JSONDecoder().decode(
        LauncherConfiguration.self,
        from: Data(contentsOf: configurationURL))
} catch {
    fail("LauncherConfig.json is invalid: \(error.localizedDescription)")
}

guard FileManager.default.isExecutableFile(
    atPath: configuration.executable) else {
    fail("The configured Vitreo engine is missing or not executable.")
}

let process = Process()
process.executableURL = URL(fileURLWithPath: configuration.executable)
process.arguments = configuration.arguments

do {
    try process.run()
    process.waitUntilExit()
    exit(process.terminationStatus)
} catch {
    fail("Could not start Vitreo: \(error.localizedDescription)")
}
