// SPDX-License-Identifier: LGPL-2.1-only

import AppKit
import Foundation

/// Extract the primary application icon from a Windows PE executable (.exe).
/// Returns nil if the file is not a valid PE or contains no icon resources.
/// Safe to call from a background thread.
func extractPEIcon(at url: URL) -> NSImage? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    guard data.count > 0x40 else { return nil }
    guard data[0] == 0x4D, data[1] == 0x5A else { return nil }  // MZ

    let lfanew = Int(data.ple32(at: 0x3C))
    guard lfanew > 0, lfanew + 24 <= data.count else { return nil }
    guard data.ple32(at: lfanew) == 0x00004550 else { return nil }  // PE\0\0

    let numSections    = Int(data.ple16(at: lfanew + 6))
    let sizeOfOptional = Int(data.ple16(at: lfanew + 20))

    let optStart = lfanew + 24
    guard optStart + 4 <= data.count else { return nil }
    let magic = data.ple16(at: optStart)

    // Offset from optStart to the first DataDirectory entry
    let ddBase: Int
    switch magic {
    case 0x010B: ddBase = optStart + 96   // PE32
    case 0x020B: ddBase = optStart + 112  // PE32+
    default: return nil
    }

    guard ddBase + 24 <= data.count else { return nil }
    let rsrcRVA  = Int(data.ple32(at: ddBase + 16))   // DataDirectory[2] = resource dir
    let rsrcVSz  = Int(data.ple32(at: ddBase + 20))
    guard rsrcRVA > 0, rsrcVSz > 0 else { return nil }

    // RVA → file offset through the section that contains it. A section
    // spans VirtualSize bytes in memory (SizeOfRawData when VirtualSize is 0),
    // and only its first SizeOfRawData bytes are backed by the file.
    let sectionBase = optStart + sizeOfOptional
    func rva2off(_ rva: Int) -> Int? {
        for i in 0..<numSections {
            let s = sectionBase + i * 40
            guard s + 40 <= data.count else { break }
            let virtualSize = Int(data.ple32(at: s + 8))
            let va          = Int(data.ple32(at: s + 12))
            let rawSize     = Int(data.ple32(at: s + 16))
            let raw         = Int(data.ple32(at: s + 20))
            let span = virtualSize != 0 ? virtualSize : rawSize
            guard rva >= va, rva < va + span else { continue }
            let delta = rva - va
            guard delta < rawSize else { return nil }
            return raw + delta
        }
        return nil
    }
    guard let rsrcFileOff = rva2off(rsrcRVA), rsrcFileOff < data.count else { return nil }

    // Traverse one directory level; id == -1 returns the first entry found.
    // Returns (sectionRelativeOffset, isDirectory).
    func findEntry(at dirFileOff: Int, id: Int) -> (offset: Int, isDir: Bool)? {
        guard dirFileOff + 16 <= data.count else { return nil }
        let named = Int(data.ple16(at: dirFileOff + 12))
        let ids   = Int(data.ple16(at: dirFileOff + 14))
        for i in 0..<(named + ids) {
            let e = dirFileOff + 16 + i * 8
            guard e + 8 <= data.count else { return nil }
            let entryId  = Int(data.ple32(at: e))
            let entryVal = Int(data.ple32(at: e + 4))
            if id == -1 || entryId == id {
                let isDir = (entryVal & 0x80000000) != 0
                let off   = isDir ? (entryVal & 0x7FFFFFFF) : entryVal
                return (off, isDir)
            }
        }
        return nil
    }

    // A leaf IMAGE_RESOURCE_DATA_ENTRY → (file offset, size) of the raw data.
    func leafData(at sectionRelOff: Int) -> (fileOff: Int, size: Int)? {
        let o = rsrcFileOff + sectionRelOff
        guard o + 8 <= data.count else { return nil }
        let dataRVA  = Int(data.ple32(at: o))
        let dataSize = Int(data.ple32(at: o + 4))
        guard let fileOff = rva2off(dataRVA),
              fileOff + dataSize <= data.count, dataSize > 0 else { return nil }
        return (fileOff, dataSize)
    }

    // 3-level traversal helper: type → name (first) → language (first) → leaf data
    func findResourceData(type typeId: Int) -> (fileOff: Int, size: Int)? {
        guard let (t, td) = findEntry(at: rsrcFileOff, id: typeId), td else { return nil }
        guard let (n, nd) = findEntry(at: rsrcFileOff + t, id: -1),  nd else { return nil }
        guard let (l, ld) = findEntry(at: rsrcFileOff + n, id: -1), !ld else { return nil }
        return leafData(at: l)
    }

    // ---- Locate RT_GROUP_ICON (type 14) ----
    guard let (grpFileOff, _) = findResourceData(type: 14) else { return nil }
    guard grpFileOff + 6 <= data.count else { return nil }

    let iconCount = Int(data.ple16(at: grpFileOff + 4))
    guard iconCount > 0, grpFileOff + 6 + iconCount * 14 <= data.count else { return nil }

    // ---- Locate RT_ICON (type 3) directory (level-1 subdirectory offset) ----
    guard let (iconTypeOff, iconTypeIsDir) = findEntry(at: rsrcFileOff, id: 3),
          iconTypeIsDir else { return nil }

    // ---- Build in-memory ICO file ----
    // ICONDIR (6 bytes) + N × ICONDIRENTRY (16 bytes each) + raw DIB/PNG blobs
    var entries = Data()
    var blobs   = Data()
    var written = 0  // icons successfully written

    let fixedDataOffset = 6 + iconCount * 16
    var currentBlobOffset = fixedDataOffset

    for i in 0..<iconCount {
        let e = grpFileOff + 6 + i * 14
        guard e + 14 <= data.count else { continue }

        let width      = data[e + 0]
        let height     = data[e + 1]
        let colorCount = data[e + 2]
        let planes     = data.ple16(at: e + 4)
        let bitCount   = data.ple16(at: e + 6)
        let iconId     = Int(data.ple16(at: e + 12))

        // Locate RT_ICON entry for this ID
        guard let (iconNameOff, iconNameIsDir) = findEntry(at: rsrcFileOff + iconTypeOff, id: iconId),
              iconNameIsDir else { continue }
        guard let (iconLangOff, iconLangIsDir) = findEntry(at: rsrcFileOff + iconNameOff, id: -1),
              !iconLangIsDir else { continue }
        guard let (iconFileOff, iconSize) = leafData(at: iconLangOff) else { continue }

        // ICONDIRENTRY
        entries.append(width)
        entries.append(height)
        entries.append(colorCount)
        entries.append(0)            // reserved
        entries.appendPLE16(planes)
        entries.appendPLE16(bitCount)
        entries.appendPLE32(UInt32(iconSize))
        entries.appendPLE32(UInt32(currentBlobOffset))

        blobs.append(data[iconFileOff..<(iconFileOff + iconSize)])
        currentBlobOffset += iconSize
        written += 1
    }
    guard written > 0 else { return nil }

    // ICONDIR header
    var icoFile = Data()
    icoFile.appendPLE16(0)              // reserved
    icoFile.appendPLE16(1)              // type = ICO
    icoFile.appendPLE16(UInt16(written))
    icoFile.append(entries)
    icoFile.append(blobs)

    return NSImage(data: icoFile)
}

enum MacAppIconError: LocalizedError {
    case invalidImage
    case pngEncodingFailed(size: Int)
    case iconConversionFailed(String)

    var errorDescription: String? {
        switch self {
        case .invalidImage:
            return "The Windows application icon has no drawable dimensions."
        case .pngEncodingFailed(let size):
            return "Could not render the application icon at \(size)×\(size)."
        case .iconConversionFailed(let detail):
            return detail.isEmpty
                ? "macOS could not convert the application icon to ICNS."
                : "macOS could not convert the application icon to ICNS: \(detail)"
        }
    }
}

/// Render a Windows application icon into a portable macOS `.icns` resource.
///
/// Finder and Launch Services do not use an executable's PE icon directly.
/// A launcher bundle needs an ICNS file under `Contents/Resources` and a
/// matching `CFBundleIconFile` entry in its Info.plist.
@discardableResult
func writeMacAppIcon(
    _ image: NSImage,
    to resourcesDirectory: URL,
    fileManager: FileManager = .default
) throws -> URL {
    guard image.size.width > 0, image.size.height > 0 else {
        throw MacAppIconError.invalidImage
    }

    try fileManager.createDirectory(
        at: resourcesDirectory, withIntermediateDirectories: true)
    let iconset = resourcesDirectory.appendingPathComponent(
        ".AppIcon-\(UUID().uuidString).iconset")
    try fileManager.createDirectory(at: iconset, withIntermediateDirectories: true)
    defer { try? fileManager.removeItem(at: iconset) }

    let variants: [(name: String, pixels: Int)] = [
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

    for variant in variants {
        guard let png = renderPNG(image, pixels: variant.pixels) else {
            throw MacAppIconError.pngEncodingFailed(size: variant.pixels)
        }
        try png.write(
            to: iconset.appendingPathComponent(variant.name),
            options: .atomic)
    }

    let output = resourcesDirectory.appendingPathComponent("AppIcon.icns")
    let process = Process()
    let errors = Pipe()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
    process.arguments = [
        "--convert", "icns",
        "--output", output.path,
        iconset.path,
    ]
    process.standardError = errors
    try process.run()
    process.waitUntilExit()
    let diagnostic = String(
        data: errors.fileHandleForReading.readDataToEndOfFile(),
        encoding: .utf8
    )?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    guard process.terminationStatus == 0,
          fileManager.fileExists(atPath: output.path) else {
        throw MacAppIconError.iconConversionFailed(diagnostic)
    }
    return output
}

private func renderPNG(_ image: NSImage, pixels: Int) -> Data? {
    guard let bitmap = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: pixels,
        pixelsHigh: pixels,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: 0,
        bitsPerPixel: 0
    ), let context = NSGraphicsContext(bitmapImageRep: bitmap) else {
        return nil
    }

    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = context
    context.cgContext.clear(
        CGRect(x: 0, y: 0, width: pixels, height: pixels))
    context.imageInterpolation = .high

    let scale = min(
        CGFloat(pixels) / image.size.width,
        CGFloat(pixels) / image.size.height)
    let width = image.size.width * scale
    let height = image.size.height * scale
    let destination = NSRect(
        x: (CGFloat(pixels) - width) / 2,
        y: (CGFloat(pixels) - height) / 2,
        width: width,
        height: height)
    image.draw(
        in: destination,
        from: NSRect(origin: .zero, size: image.size),
        operation: .copy,
        fraction: 1,
        respectFlipped: true,
        hints: [.interpolation: NSImageInterpolation.high])
    context.flushGraphics()
    NSGraphicsContext.restoreGraphicsState()

    return bitmap.representation(using: .png, properties: [:])
}

// MARK: - Private Data helpers

private extension Data {
    func ple16(at offset: Int) -> UInt16 {
        guard offset + 2 <= count else { return 0 }
        return UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func ple32(at offset: Int) -> UInt32 {
        guard offset + 4 <= count else { return 0 }
        return UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }

    mutating func appendPLE16(_ v: UInt16) {
        append(UInt8(v & 0xFF))
        append(UInt8((v >> 8) & 0xFF))
    }

    mutating func appendPLE32(_ v: UInt32) {
        append(UInt8(v & 0xFF))
        append(UInt8((v >> 8) & 0xFF))
        append(UInt8((v >> 16) & 0xFF))
        append(UInt8((v >> 24) & 0xFF))
    }
}
