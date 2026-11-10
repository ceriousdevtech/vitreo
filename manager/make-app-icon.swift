// Regenerate VitreoManager/Assets.xcassets/AppIcon.appiconset from the
// full-bleed icon art (assets/vitreo-glass-icon.png).
//
// macOS 11-15 draw an app icon exactly as supplied, on Apple's icon grid: an
// 824x824 rounded tile centred in a 1024 canvas with transparent margins and
// a soft shadow. Full-bleed art fills the whole slot there and looks too big
// next to every other app. macOS 26 recognises a grid tile and masks it the
// same way it masks full-bleed art, so the gridded set is right everywhere.
//
// Usage (from manager/):
//   swift make-app-icon.swift ../assets/vitreo-glass-icon.png //       VitreoManager/Assets.xcassets/AppIcon.appiconset
import AppKit

let args = CommandLine.arguments
guard args.count == 3, let src = NSImage(contentsOfFile: args[1]) else {
    FileHandle.standardError.write("usage: make-app-icon <src.png> <appiconset_dir>\n".data(using: .utf8)!)
    exit(1)
}
let outDir = URL(fileURLWithPath: args[2])
try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

func render(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px,
                               bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                               isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    rep.size = NSSize(width: px, height: px)
    NSGraphicsContext.saveGraphicsState()
    let ctx = NSGraphicsContext(bitmapImageRep: rep)!
    ctx.imageInterpolation = .high
    NSGraphicsContext.current = ctx
    let s = CGFloat(px) / 1024.0
    // Apple grid: 824 tile, 100 margin; the shadow sits slightly below.
    let tile = NSRect(x: 100 * s, y: 100 * s, width: 824 * s, height: 824 * s)
    // The source is a square whose corners are background, not
    // transparency: clip it to the macOS tile shape (corner radius ~22.4%
    // of the tile) and let that shape cast the shadow.
    let radius = 824 * 0.2237 * s
    let shape = NSBezierPath(roundedRect: tile, xRadius: radius, yRadius: radius)
    NSGraphicsContext.saveGraphicsState()
    let shadow = NSShadow()
    shadow.shadowColor = NSColor.black.withAlphaComponent(0.28)
    shadow.shadowBlurRadius = 18 * s
    shadow.shadowOffset = NSSize(width: 0, height: -8 * s)
    shadow.set()
    NSColor.white.setFill()
    shape.fill()
    NSGraphicsContext.restoreGraphicsState()
    shape.addClip()
    src.draw(in: tile, from: .zero, operation: .sourceOver, fraction: 1.0)
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let names: [(String, Int)] = [
    ("icon_16x16.png", 16), ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32), ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128), ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256), ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512), ("icon_512x512@2x.png", 1024),
]
for (name, px) in names {
    try render(px).write(to: outDir.appendingPathComponent(name))
}
print("wrote \(names.count) images to \(outDir.path)")
