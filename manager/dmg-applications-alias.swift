import AppKit
import Foundation

// A symlink to /Applications shows up as a plain square on a disk image.
// A Finder alias keeps the Applications folder icon.

if CommandLine.arguments.count != 2 {
    fputs("usage: dmg-applications-alias.swift <Applications>\n", stderr)
    exit(1)
}

let destination = URL(fileURLWithPath: CommandLine.arguments[1])
let applications = URL(fileURLWithPath: "/Applications", isDirectory: true)
let iconPath = "/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/ApplicationsFolderIcon.icns"

do {
    let bookmark = try applications.bookmarkData(
        options: [.suitableForBookmarkFile],
        includingResourceValuesForKeys: nil,
        relativeTo: nil
    )
    try URL.writeBookmarkData(bookmark, to: destination)
} catch {
    fputs("error: could not create the Applications alias: \(error)\n", stderr)
    exit(1)
}

if let icon = NSImage(contentsOfFile: iconPath) {
    if !NSWorkspace.shared.setIcon(icon, forFile: destination.path, options: []) {
        fputs("error: could not set the Applications folder icon\n", stderr)
        exit(1)
    }
} else {
    fputs("error: Applications folder icon is missing at \(iconPath)\n", stderr)
    exit(1)
}
