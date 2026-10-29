import AppKit

// Finder icon positions are the center of each icon, in points, origin at the
// top-left of the disk-image window. Pass 1 or 2 as the scale. The packager
// combines those into one HiDPI TIFF; a single 2x PNG is ignored on macOS 26.

let windowWidth: CGFloat = 680
let windowHeight: CGFloat = 420
let iconSize = 128
let appCenter = CGPoint(x: 180, y: 228)
let applicationsCenter = CGPoint(x: 500, y: 228)

let output: String
let scale: CGFloat
if CommandLine.arguments.count == 3, let parsed = Double(CommandLine.arguments[2]), parsed == 1 || parsed == 2 {
    output = CommandLine.arguments[1]
    scale = CGFloat(parsed)
} else {
    fputs("usage: dmg-background.swift <output.png> <1|2>\n", stderr)
    exit(1)
}

guard let rep = NSBitmapImageRep(
    bitmapDataPlanes: nil,
    pixelsWide: Int(windowWidth * scale),
    pixelsHigh: Int(windowHeight * scale),
    bitsPerSample: 8,
    samplesPerPixel: 4,
    hasAlpha: true,
    isPlanar: false,
    colorSpaceName: .deviceRGB,
    bytesPerRow: 0,
    bitsPerPixel: 0
) else {
    fputs("error: could not allocate the background image\n", stderr)
    exit(1)
}
rep.size = NSSize(width: windowWidth, height: windowHeight)

let graphics = NSGraphicsContext(bitmapImageRep: rep)
NSGraphicsContext.current = graphics
guard let graphics else {
    fputs("error: could not draw the background image\n", stderr)
    exit(1)
}
let context = graphics.cgContext
context.interpolationQuality = .high
context.setAllowsAntialiasing(true)

func point(fromTop point: CGPoint) -> CGPoint {
    CGPoint(x: point.x, y: windowHeight - point.y)
}

func color(_ red: CGFloat, _ green: CGFloat, _ blue: CGFloat, _ alpha: CGFloat) -> NSColor {
    NSColor(srgbRed: red, green: green, blue: blue, alpha: alpha)
}

let canvas = NSRect(x: 0, y: 0, width: windowWidth, height: windowHeight)
NSGradient(
    colors: [
        color(0.965, 0.975, 1.0, 1),
        color(0.93, 0.945, 0.995, 1),
        color(0.955, 0.935, 0.985, 1),
    ]
)?.draw(in: canvas, angle: -90)

func glow(at centerFromTop: CGPoint, radius: CGFloat, tint: NSColor) {
    let center = point(fromTop: centerFromTop)
    let colors = [tint.cgColor, tint.withAlphaComponent(0).cgColor] as CFArray
    guard let gradient = CGGradient(
        colorsSpace: CGColorSpaceCreateDeviceRGB(),
        colors: colors,
        locations: [0, 1]
    ) else {
        return
    }
    context.drawRadialGradient(
        gradient,
        startCenter: center,
        startRadius: 0,
        endCenter: center,
        endRadius: radius,
        options: []
    )
}

glow(at: appCenter, radius: 210, tint: color(0.23, 0.61, 1.0, 0.22))
glow(at: applicationsCenter, radius: 210, tint: color(0.66, 0.48, 0.96, 0.20))

let arrowY = windowHeight - appCenter.y
let arrowStart: CGFloat = 286
let arrowEnd: CGFloat = 392
let shaftRect = NSRect(x: arrowStart, y: arrowY - 1.75, width: arrowEnd - arrowStart, height: 3.5)
let shaft = NSBezierPath(roundedRect: shaftRect, xRadius: 1.75, yRadius: 1.75)
context.saveGState()
shaft.addClip()
NSGradient(
    colors: [color(0.18, 0.56, 1.0, 1), color(0.62, 0.36, 0.95, 1)]
)?.draw(in: shaftRect, angle: 0)
context.restoreGState()

let head = NSBezierPath()
head.move(to: NSPoint(x: arrowEnd - 2, y: arrowY + 8))
head.line(to: NSPoint(x: arrowEnd + 14, y: arrowY))
head.line(to: NSPoint(x: arrowEnd - 2, y: arrowY - 8))
head.close()
color(0.62, 0.36, 0.95, 1).setFill()
head.fill()

func centered(_ text: String, font: NSFont, tint: NSColor, rect: NSRect, kern: CGFloat) {
    let style = NSMutableParagraphStyle()
    style.alignment = .center
    let attributes: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: tint,
        .paragraphStyle: style,
        .kern: kern,
    ]
    NSString(string: text).draw(in: rect, withAttributes: attributes)
}

let titleFont = NSFont.systemFont(ofSize: 22, weight: .semibold)
let taglineFont = NSFont.systemFont(ofSize: 13, weight: .regular)
let captionFont = NSFont.systemFont(ofSize: 13, weight: .medium)

centered(
    "VITREO",
    font: titleFont,
    tint: color(0.08, 0.09, 0.12, 1),
    rect: NSRect(x: 0, y: windowHeight - 64, width: windowWidth, height: 28),
    kern: 4
)
centered(
    "Windows software. At home on Mac.",
    font: taglineFont,
    tint: color(0.32, 0.36, 0.46, 1),
    rect: NSRect(x: 0, y: windowHeight - 90, width: windowWidth, height: 18),
    kern: 0.4
)
centered(
    "Drag Vitreo to the Applications folder",
    font: captionFont,
    tint: color(0.22, 0.26, 0.36, 1),
    rect: NSRect(x: 40, y: 28, width: windowWidth - 80, height: 20),
    kern: 0.2
)

NSGraphicsContext.current = nil
guard let png = rep.representation(using: .png, properties: [:]) else {
    fputs("error: could not encode the background image\n", stderr)
    exit(1)
}
do {
    try png.write(to: URL(fileURLWithPath: output))
} catch {
    fputs("error: could not write \(output): \(error)\n", stderr)
    exit(1)
}

print(
    "LAYOUT window \(Int(windowWidth)) \(Int(windowHeight)) icon \(iconSize) app \(Int(appCenter.x)) \(Int(appCenter.y)) applications \(Int(applicationsCenter.x)) \(Int(applicationsCenter.y))"
)
