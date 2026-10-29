import AppKit
import Foundation

// App icons are square bitmaps. macOS masks them when they are an app, and
// leaves them square when they are a disk image. This cuts the same artwork
// into the rounded icon shape and can stamp that onto the DMG file.

let volumeIcon = CommandLine.arguments.count > 1 && CommandLine.arguments[1] == "--apply"

let sourcePath: String
let outputPath: String
let badgePath: String?
if volumeIcon {
    if CommandLine.arguments.count != 4 {
        fputs("usage: dmg-volume-icon.swift --apply <icon.icns> <file>\n", stderr)
        exit(1)
    }
    sourcePath = ""
    outputPath = CommandLine.arguments[2]
    badgePath = CommandLine.arguments[3]
} else {
    if CommandLine.arguments.count != 3 {
        fputs("usage: dmg-volume-icon.swift <source.icns> <output.icns>\n", stderr)
        exit(1)
    }
    sourcePath = CommandLine.arguments[1]
    outputPath = CommandLine.arguments[2]
    badgePath = nil
}

func fail(_ message: String) -> Never {
    fputs("error: \(message)\n", stderr)
    exit(1)
}

if let badgePath {
    guard let image = NSImage(contentsOfFile: outputPath) else {
        fail("could not read \(outputPath)")
    }
    if !NSWorkspace.shared.setIcon(image, forFile: badgePath, options: []) {
        fail("could not set the disk image icon")
    }
    exit(0)
}

guard let source = NSImage(contentsOfFile: sourcePath), source.isValid else {
    fail("could not read \(sourcePath)")
}

func roundedIcon(side: Int) -> NSBitmapImageRep {
    guard let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: side,
        pixelsHigh: side,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0
    ) else {
        fail("could not allocate the icon")
    }
    rep.size = NSSize(width: side, height: side)
    guard let graphics = NSGraphicsContext(bitmapImageRep: rep) else {
        fail("could not draw the icon")
    }
    NSGraphicsContext.current = graphics
    graphics.imageInterpolation = .high
    let canvas = NSRect(x: 0, y: 0, width: side, height: side)
    NSColor.clear.setFill()
    canvas.fill()
    let radius = CGFloat(side) * 0.2237
    NSBezierPath(roundedRect: canvas, xRadius: radius, yRadius: radius).addClip()
    source.draw(in: canvas, from: .zero, operation: .copy, fraction: 1)
    NSGraphicsContext.current = nil
    return rep
}

let iconset = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("vitreo-volume.iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

let sizes: [(String, Int)] = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]

for (name, side) in sizes {
    guard let png = roundedIcon(side: side).representation(using: .png, properties: [:]) else {
        fail("could not encode \(name)")
    }
    try png.write(to: iconset.appendingPathComponent(name))
}

let task = Process()
task.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
task.arguments = ["-c", "icns", iconset.path, "-o", outputPath]
try task.run()
task.waitUntilExit()
if task.terminationStatus != 0 {
    fail("iconutil failed")
}
try? FileManager.default.removeItem(at: iconset)
