/// @file cocoa_perf_overlay.mm
/// @brief The performance overlay: the guest's frame rate, GPU and CPU time,
///        memory and the host machine, drawn over a guest window.
///
/// The overlay is a host view stacked above the guest's presentation view in
/// the same window, so a window or screen recording captures it and the
/// guest's frames are not touched. It has three modes: off, a compact bar,
/// and the full HUD (frame-time graph, GPU/CPU/memory rows, host machine).
/// ⌥⌘P steps Off → Compact → Full → Off; View ▸ Performance Overlay picks
/// one. The choice is remembered for later launches in ~/.vitreo/preferences
/// (`perf_overlay=0|compact|full`; the older `1` reads as full).
/// VITREO_PERF_OVERLAY=0|compact|full (or 1, full) overrides it for one
/// launch.
///
/// Sources: frames are graphics::recent_presents (every frame the Metal
/// swapchain presents); GPU time is graphics::recent_gpu_busy, the union of
/// the command buffers' GPUStartTime..GPUEndTime, recorded only while the
/// overlay shows; CPU is getrusage(RUSAGE_SELF) over wall time, as Activity
/// Monitor counts it (100% = one core); memory is the task's phys_footprint,
/// Activity Monitor's "Memory"; GPU memory is MTLDevice.currentAllocatedSize.
///
/// Built without ARC, as the rest of src/windowing/cocoa.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "windowing/cocoa/cocoa_perf_overlay.h"
#include "graphics/common/gpu_busy.hpp"
#include "graphics/common/present_rate.hpp"
#include "platform/host_hardware.hpp"

#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

enum class Mode { off = 0, compact = 1, full = 2 };

constexpr CGFloat kMargin = 10.0;
constexpr unsigned short kKeyCodeP = 35;  // kVK_ANSI_P
constexpr NSTimeInterval kRefreshS = 0.1;
constexpr NSTimeInterval kCpuSampleS = 0.5;

// Full HUD geometry.
constexpr CGFloat kFullWidth = 288.0;
constexpr CGFloat kPad = 12.0;
constexpr CGFloat kGraphHeight = 56.0;
constexpr CGFloat kRowHeight = 18.0;
// The graphs are a time axis: the full HUD shows the last two seconds, the
// compact bar the last one, each frame as wide as it took. A fixed bar count
// would fill the width only at one frame rate.
constexpr float kGraphWindowMs = 2000.0f;
constexpr float kSparkWindowMs = 1000.0f;
constexpr u_int64_t kGraphWindowNs = 2000000000ull;
// Two seconds of frames at 500 FPS; present_rate keeps 1024 presents.
constexpr size_t kMaxIntervals = 1024;

Mode g_mode = Mode::off;
bool g_swallow_key_up = false;
NSTimer* g_timer = nil;

// --- ~/.vitreo/preferences: one key=value per line --------------------------

std::string preferences_path() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) + "/.vitreo/preferences" : std::string();
}

std::map<std::string, std::string> read_preferences() {
    std::map<std::string, std::string> values;
    const std::string path = preferences_path();
    if (path.empty()) return values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::compact: return "compact";
        case Mode::full: return "full";
        case Mode::off: break;
    }
    return "0";
}

Mode parse_mode(const std::string& text) {
    if (text == "compact") return Mode::compact;
    if (text == "full" || text == "1") return Mode::full;
    return Mode::off;
}

void remember_mode(Mode mode) {
    const std::string path = preferences_path();
    if (path.empty()) return;
    auto values = read_preferences();
    values["perf_overlay"] = mode_name(mode);
    const std::string dir = path.substr(0, path.rfind('/'));
    ::mkdir(dir.c_str(), 0755);
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        for (const auto& [key, value] : values) out << key << '=' << value << '\n';
        if (!out) return;
    }
    std::rename(temp.c_str(), path.c_str());
}

Mode initial_mode() {
    if (const char* env = std::getenv("VITREO_PERF_OVERLAY"); env != nullptr && *env) {
        return parse_mode(env);
    }
    const auto values = read_preferences();
    const auto it = values.find("perf_overlay");
    return it != values.end() ? parse_mode(it->second) : Mode::off;
}

// --- The numbers ---------------------------------------------------------------

struct Sample {
    vitreo::graphics::RecentPresents frames;
    float intervals[kMaxIntervals] = {};
    size_t interval_count = 0;
    double gpu_ms_per_frame = 0.0;  // 0: no GPU work seen
    double gpu_percent = 0.0;
    double cpu_percent = 0.0;
    unsigned cpu_cores = 0;
    unsigned long long memory_bytes = 0;
    unsigned long long gpu_memory_bytes = 0;
};

Sample g_sample;

double process_cpu_seconds() {
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

unsigned long long process_footprint() {
    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.phys_footprint;
}

void take_sample() {
    using namespace vitreo::graphics;
    Sample& s = g_sample;
    const u_int64_t now_ns = present_clock_ns();
    s.frames = recent_presents(now_ns);
    s.interval_count = recent_frame_intervals_ms(now_ns, kGraphWindowNs, s.intervals,
                                                 kMaxIntervals);

    const RecentGpuBusy gpu = recent_gpu_busy(gpu_busy_clock_s());
    s.gpu_percent = gpu.window_ms > 0 ? 100.0 * gpu.busy_ms / gpu.window_ms : 0.0;
    s.gpu_ms_per_frame =
        s.frames.frames > 0 ? gpu.busy_ms / s.frames.frames : gpu.busy_ms;
    s.gpu_memory_bytes = gpu.allocated_bytes;

    // CPU over a longer span than the refresh: a 100 ms window of a game's
    // threads jumps by whole cores between ticks.
    static double last_cpu = -1.0;
    static CFTimeInterval last_wall = 0.0;
    const CFTimeInterval wall = CACurrentMediaTime();
    if (last_cpu < 0.0 || wall - last_wall >= kCpuSampleS) {
        const double cpu = process_cpu_seconds();
        if (last_cpu >= 0.0 && wall > last_wall) {
            s.cpu_percent = 100.0 * (cpu - last_cpu) / (wall - last_wall);
        }
        last_cpu = cpu;
        last_wall = wall;
        s.memory_bytes = process_footprint();
    }
    s.cpu_cores = static_cast<unsigned>([[NSProcessInfo processInfo] activeProcessorCount]);
}

// --- Drawing helpers -------------------------------------------------------------

NSColor* rgba(CGFloat r, CGFloat g, CGFloat b, CGFloat a = 1.0) {
    return [NSColor colorWithSRGBRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:a];
}

NSColor* good_color() { return rgba(52, 211, 120); }
NSColor* fair_color() { return rgba(250, 204, 21); }
NSColor* poor_color() { return rgba(248, 82, 82); }
NSColor* label_color() { return rgba(255, 255, 255, 0.55); }
NSColor* value_color() { return rgba(255, 255, 255, 0.95); }
NSColor* faint_color() { return rgba(255, 255, 255, 0.40); }

NSColor* rate_color(double fps) {
    if (fps >= 55.0) return good_color();
    if (fps >= 30.0) return fair_color();
    return poor_color();
}

NSColor* interval_color(float ms) {
    if (ms <= 18.0f) return good_color();
    if (ms <= 34.0f) return fair_color();
    return poor_color();
}

NSColor* load_color(double percent) {
    if (percent < 70.0) return good_color();
    if (percent < 90.0) return fair_color();
    return poor_color();
}

NSFont* mono(CGFloat size, NSFontWeight weight) {
    return [NSFont monospacedDigitSystemFontOfSize:size weight:weight];
}

NSDictionary* text_attrs(NSFont* font, NSColor* color) {
    return @{NSFontAttributeName : font, NSForegroundColorAttributeName : color};
}

NSString* ns(const char* utf8) { return [NSString stringWithUTF8String:utf8]; }

/// Draw @p text with its top-left at @p point; returns its width.
CGFloat draw_text(NSString* text, NSPoint point, NSFont* font, NSColor* color) {
    NSDictionary* attrs = text_attrs(font, color);
    [text drawAtPoint:point withAttributes:attrs];
    return [text sizeWithAttributes:attrs].width;
}

/// Draw @p text with its top-right at @p point.
void draw_text_right(NSString* text, NSPoint point, NSFont* font, NSColor* color) {
    NSDictionary* attrs = text_attrs(font, color);
    const CGFloat width = [text sizeWithAttributes:attrs].width;
    [text drawAtPoint:NSMakePoint(point.x - width, point.y) withAttributes:attrs];
}

CGFloat text_width(NSString* text, NSFont* font) {
    return [text sizeWithAttributes:text_attrs(font, value_color())].width;
}

std::string format_bytes(unsigned long long bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) {
        std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    } else {
        std::snprintf(text, sizeof text, "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
    }
    return text;
}

/// Frame intervals as bars in @p rect (flipped): the newest at the right
/// edge, each as wide as it lasted on a @p window_ms axis, so the width is
/// always the same span of time and a stall is one wide bar.
void draw_frame_bars(NSRect rect, const float* intervals, size_t count, float window_ms,
                     float scale_ms) {
    if (count == 0 || window_ms <= 0.0f) return;
    const CGFloat per_ms = NSWidth(rect) / window_ms;
    CGFloat right = NSMaxX(rect);
    for (size_t i = count; i > 0 && right > NSMinX(rect); --i) {
        const float ms = intervals[i - 1];
        const CGFloat width = static_cast<CGFloat>(ms) * per_ms;
        const CGFloat left = std::max(NSMinX(rect), right - width);
        const CGFloat h = std::max<CGFloat>(
            1.0, NSHeight(rect) * std::min(1.0f, ms / scale_ms));
        // A hairline gap between bars once they are wide enough to keep one.
        const CGFloat gap = right - left >= 2.0 ? 0.5 : 0.0;
        [interval_color(ms) setFill];
        NSRectFill(NSMakeRect(left, NSMaxY(rect) - h,
                              std::max<CGFloat>(0.5, right - left - gap), h));
        right -= width;
    }
}

// --- Host description (fixed for the process) -----------------------------------

NSString* host_line(int which) {
    static NSString* lines[2] = {nil, nil};
    if (lines[0] == nil) {
        const auto& hw = vitreo::platform::host_hardware();
        std::ostringstream first;
        first << (hw.machine.product_name.empty() ? hw.machine.model_identifier
                                                  : hw.machine.product_name);
        first << " · " << hw.cpu.brand;
        std::ostringstream second;
        vitreo::u32 performance = 0;
        vitreo::u32 efficiency = 0;
        for (const auto& cores : hw.cpu.core_classes) {
            (cores.efficiency ? efficiency : performance) += cores.physical_cores;
        }
        const char* separator = "";
        if (performance + efficiency > 0) {
            second << performance << 'P';
            if (efficiency > 0) second << '+' << efficiency << 'E';
            separator = " · ";
        }
        if (hw.gpu.core_count > 0) {
            second << separator << hw.gpu.core_count << "-core GPU";
            separator = " · ";
        }
        if (hw.memory.total_bytes > 0) {
            second << separator << (hw.memory.total_bytes + (1ull << 29)) / (1ull << 30)
                   << " GB";
            separator = " · ";
        }
        const NSOperatingSystemVersion os = [[NSProcessInfo processInfo] operatingSystemVersion];
        second << separator << "macOS " << os.majorVersion << '.' << os.minorVersion;
        if (os.patchVersion != 0) second << '.' << os.patchVersion;
        lines[0] = [[NSString alloc] initWithUTF8String:first.str().c_str()];
        lines[1] = [[NSString alloc] initWithUTF8String:second.str().c_str()];
    }
    return lines[which];
}

}  // namespace

// --- The view -----------------------------------------------------------------

@interface VtrPerfOverlayView : NSView
- (void)refresh;
@end

@implementation VtrPerfOverlayView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
        self.layer.cornerRadius = 10.0;
        self.layer.masksToBounds = YES;
        self.layer.backgroundColor = [rgba(14, 15, 20, 0.80) CGColor];
        self.layer.borderColor = [rgba(255, 255, 255, 0.14) CGColor];
        self.layer.borderWidth = 1.0;
    }
    return self;
}

// Input belongs to the guest beneath: the overlay is never hit.
- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (BOOL)isFlipped { return YES; }

// The window's visibility sync un-hides every content subview it did not
// hide for the guest (cocoa_display.mm); while the overlay is off it stays
// hidden instead of flashing until the next tick.
- (void)setHidden:(BOOL)hidden {
    [super setHidden:hidden || g_mode == Mode::off];
}

- (NSSize)sizeForMode {
    if (g_mode == Mode::compact) return NSMakeSize([self compactWidth], 28.0);
    // header + graph + 4 rows + divider + 2 footer lines
    const CGFloat height = kPad + 30.0 + 6.0 + kGraphHeight + 10.0 + 4 * kRowHeight +
                           9.0 + 2 * 14.0 + kPad - 2.0;
    return NSMakeSize(kFullWidth, height);
}

- (void)refresh {
    const NSSize size = [self sizeForMode];
    NSView* parent = self.superview;
    const CGFloat y = (parent == nil || parent.isFlipped)
                          ? kMargin
                          : NSHeight(parent.bounds) - kMargin - size.height;
    const NSRect frame = NSMakeRect(kMargin, y, size.width, size.height);
    if (!NSEqualRects(frame, self.frame)) self.frame = frame;
    self.needsDisplay = YES;
}

// --- Compact: one row ------------------------------------------------------------

- (NSArray<NSString*>*)compactFields {
    const Sample& s = g_sample;
    char text[64];
    NSMutableArray* fields = [NSMutableArray array];
    std::snprintf(text, sizeof text, "%.1f ms", s.frames.average_ms);
    [fields addObject:ns(text)];
    if (s.gpu_ms_per_frame > 0.0) {
        std::snprintf(text, sizeof text, "GPU %.1f ms", s.gpu_ms_per_frame);
        [fields addObject:ns(text)];
    }
    std::snprintf(text, sizeof text, "CPU %.0f%%", s.cpu_percent);
    [fields addObject:ns(text)];
    if (s.memory_bytes > 0) [fields addObject:ns(format_bytes(s.memory_bytes).c_str())];
    return fields;
}

- (NSString*)compactRate {
    if (g_sample.frames.width == 0) return @"--";
    return [NSString stringWithFormat:@"%u", g_sample.frames.frames];
}

- (CGFloat)compactWidth {
    NSFont* big = mono(15, NSFontWeightBold);
    NSFont* small = mono(11.5, NSFontWeightMedium);
    CGFloat width = 10.0 + text_width([self compactRate], big) + 4.0 +
                    text_width(@"FPS", mono(10, NSFontWeightSemibold)) + 8.0 + 52.0;
    for (NSString* field in [self compactFields]) {
        width += 15.0 + text_width(field, small);
    }
    return std::ceil(width + 10.0);
}

- (void)drawCompact {
    const Sample& s = g_sample;
    NSFont* big = mono(15, NSFontWeightBold);
    NSFont* unit = mono(10, NSFontWeightSemibold);
    NSFont* small = mono(11.5, NSFontWeightMedium);
    NSColor* color = s.frames.width == 0 ? faint_color() : rate_color(s.frames.frames);
    CGFloat x = 10.0;
    x += draw_text([self compactRate], NSMakePoint(x, 4.5), big, color) + 4.0;
    x += draw_text(@"FPS", NSMakePoint(x, 9.5), unit, label_color()) + 8.0;

    const NSRect spark = NSMakeRect(x, 6.0, 52.0, 16.0);
    [rgba(255, 255, 255, 0.06) setFill];
    NSRectFill(spark);
    draw_frame_bars(spark, s.intervals, s.interval_count, kSparkWindowMs, 50.0f);
    x = NSMaxX(spark);

    for (NSString* field in [self compactFields]) {
        [rgba(255, 255, 255, 0.18) setFill];
        NSRectFill(NSMakeRect(x + 7.0, 8.0, 1.0, 12.0));
        x += 15.0;
        x += draw_text(field, NSMakePoint(x, 7.0), small, value_color());
    }
}

// --- Full HUD ----------------------------------------------------------------------

- (CGFloat)drawRow:(NSString*)label value:(NSString*)value y:(CGFloat)y
           meter:(double)percent {
    NSFont* label_font = [NSFont systemFontOfSize:11.5 weight:NSFontWeightMedium];
    NSFont* value_font = mono(12, NSFontWeightSemibold);
    draw_text(label, NSMakePoint(kPad, y), label_font, label_color());
    const CGFloat value_x = kPad + 62.0;
    const CGFloat width = draw_text(value, NSMakePoint(value_x, y), value_font, value_color());
    if (percent >= 0.0) {
        const CGFloat meter_x = value_x + std::max<CGFloat>(width + 10.0, 118.0);
        const NSRect track = NSMakeRect(meter_x, y + 6.0, kFullWidth - kPad - meter_x, 5.0);
        if (NSWidth(track) > 10.0) {
            NSBezierPath* back = [NSBezierPath bezierPathWithRoundedRect:track
                                                                 xRadius:2.5
                                                                 yRadius:2.5];
            [rgba(255, 255, 255, 0.10) setFill];
            [back fill];
            NSRect fill = track;
            fill.size.width = NSWidth(track) * std::clamp(percent, 0.0, 100.0) / 100.0;
            if (NSWidth(fill) > 0.5) {
                [load_color(percent) setFill];
                [[NSBezierPath bezierPathWithRoundedRect:fill xRadius:2.5 yRadius:2.5] fill];
            }
        }
    }
    return y + kRowHeight;
}

- (void)drawFull {
    const Sample& s = g_sample;
    const bool have_frames = s.frames.width != 0;
    char text[96];
    CGFloat y = kPad - 2.0;

    // Header: the frame rate, then average and worst frame time.
    NSFont* big = mono(28, NSFontWeightBold);
    NSColor* color = have_frames ? rate_color(s.frames.frames) : faint_color();
    NSString* rate = have_frames ? [NSString stringWithFormat:@"%u", s.frames.frames] : @"--";
    const CGFloat rate_width = draw_text(rate, NSMakePoint(kPad, y), big, color);
    draw_text(@"FPS", NSMakePoint(kPad + rate_width + 5.0, y + 14.0),
              mono(11, NSFontWeightSemibold), label_color());
    if (have_frames) {
        std::snprintf(text, sizeof text, "%.1f ms", s.frames.average_ms);
        draw_text_right(ns(text), NSMakePoint(kFullWidth - kPad, y + 2.0),
                        mono(13, NSFontWeightSemibold), value_color());
        std::snprintf(text, sizeof text, "worst %.1f ms", s.frames.worst_ms);
        draw_text_right(ns(text), NSMakePoint(kFullWidth - kPad, y + 18.0),
                        mono(10.5, NSFontWeightMedium), interval_color(
                            static_cast<float>(s.frames.worst_ms)));
    } else {
        draw_text_right(@"no frames yet", NSMakePoint(kFullWidth - kPad, y + 10.0),
                        mono(11, NSFontWeightMedium), faint_color());
    }
    y += 30.0 + 6.0;

    // Frame-time graph: the last two seconds, 60 and 30 FPS guides.
    const NSRect graph = NSMakeRect(kPad, y, kFullWidth - 2 * kPad, kGraphHeight);
    [rgba(255, 255, 255, 0.05) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:graph xRadius:4 yRadius:4] fill];
    float peak = 0.0f;
    for (size_t i = 0; i < s.interval_count; ++i) peak = std::max(peak, s.intervals[i]);
    const float scale_ms = std::clamp(peak * 1.15f, 40.0f, 120.0f);
    draw_frame_bars(NSInsetRect(graph, 2.0, 2.0), s.intervals, s.interval_count,
                    kGraphWindowMs, scale_ms);
    for (const float guide : {1000.0f / 60.0f, 1000.0f / 30.0f}) {
        const CGFloat gy = NSMaxY(graph) - 2.0 - (NSHeight(graph) - 4.0) * guide / scale_ms;
        NSBezierPath* line = [NSBezierPath bezierPath];
        const CGFloat dash[] = {3.0, 3.0};
        [line setLineDash:dash count:2 phase:0];
        line.lineWidth = 1.0;
        [line moveToPoint:NSMakePoint(NSMinX(graph) + 2.0, std::floor(gy) + 0.5)];
        [line lineToPoint:NSMakePoint(NSMaxX(graph) - 2.0, std::floor(gy) + 0.5)];
        [rgba(255, 255, 255, 0.28) setStroke];
        [line stroke];
        draw_text(guide < 20.0f ? @"60" : @"30", NSMakePoint(NSMaxX(graph) - 17.0, gy - 12.0),
                  mono(9, NSFontWeightMedium), faint_color());
    }
    y = NSMaxY(graph) + 10.0;

    // Rows.
    if (s.gpu_ms_per_frame > 0.0) {
        std::snprintf(text, sizeof text, "%.1f ms  %.0f%%", s.gpu_ms_per_frame,
                      s.gpu_percent);
        y = [self drawRow:@"GPU" value:ns(text) y:y meter:s.gpu_percent];
    } else {
        y = [self drawRow:@"GPU" value:@"idle" y:y meter:-1.0];
    }
    std::snprintf(text, sizeof text, "%.0f%%  of %u cores", s.cpu_percent, s.cpu_cores);
    y = [self drawRow:@"CPU" value:ns(text) y:y
                meter:s.cpu_cores > 0 ? s.cpu_percent / s.cpu_cores : -1.0];
    std::string memory = s.memory_bytes > 0 ? format_bytes(s.memory_bytes) : "--";
    if (s.gpu_memory_bytes > 0) memory += "  GPU " + format_bytes(s.gpu_memory_bytes);
    y = [self drawRow:@"Memory" value:ns(memory.c_str()) y:y meter:-1.0];
    if (have_frames) {
        std::snprintf(text, sizeof text, "%u × %u", s.frames.width, s.frames.height);
        y = [self drawRow:@"Frame" value:ns(text) y:y meter:-1.0];
    } else {
        y = [self drawRow:@"Frame" value:@"--" y:y meter:-1.0];
    }

    // The host machine.
    y += 3.0;
    [rgba(255, 255, 255, 0.12) setFill];
    NSRectFill(NSMakeRect(kPad, y, kFullWidth - 2 * kPad, 1.0));
    y += 6.0;
    NSFont* footer = [NSFont systemFontOfSize:10.5 weight:NSFontWeightRegular];
    draw_text(host_line(0), NSMakePoint(kPad, y), footer, label_color());
    y += 14.0;
    NSString* second = [host_line(1) stringByAppendingString:@" · Vitreo"];
    draw_text(second, NSMakePoint(kPad, y), footer, faint_color());
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (g_mode == Mode::compact) {
        [self drawCompact];
    } else if (g_mode == Mode::full) {
        [self drawFull];
    }
}
@end

// --- The toggle, its menu items and the refresh timer --------------------------

@interface VtrPerfOverlayController : NSObject
+ (instancetype)shared;
- (void)setMode:(Mode)mode;
- (void)cycle:(id)sender;
- (void)choose:(id)sender;
- (void)tick:(NSTimer*)timer;
@end

@implementation VtrPerfOverlayController
+ (instancetype)shared {
    static VtrPerfOverlayController* controller = nil;
    if (controller == nil) controller = [[VtrPerfOverlayController alloc] init];
    return controller;
}

- (void)setMode:(Mode)mode {
    g_mode = mode;
    remember_mode(mode);
    [self tick:nil];
}

- (void)cycle:(id)sender {
    (void)sender;
    [self setMode:static_cast<Mode>((static_cast<int>(g_mode) + 1) % 3)];
}

- (void)choose:(id)sender {
    [self setMode:static_cast<Mode>([(NSMenuItem*)sender tag])];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(choose:)) {
        item.state = item.tag == static_cast<NSInteger>(g_mode) ? NSControlStateValueOn
                                                                : NSControlStateValueOff;
    }
    return YES;
}

- (void)tick:(NSTimer*)timer {
    (void)timer;
    const bool shown = g_mode != Mode::off;
    vitreo::graphics::set_gpu_busy_wanted(shown);
    if (shown) take_sample();
    for (NSWindow* window in [NSApp windows]) {
        for (NSView* view in [[window contentView] subviews]) {
            if (![view isKindOfClass:[VtrPerfOverlayView class]]) continue;
            if (view.hidden == shown) view.hidden = !shown;
            if (shown) [(VtrPerfOverlayView*)view refresh];
        }
    }
}
@end

// --- C entry points -----------------------------------------------------------

void vtr_perf_overlay_attach(void* content_view) {
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        g_mode = initial_mode();
    }
    NSView* content = (__bridge NSView*)content_view;
    if (content == nil) return;
    for (NSView* view in [content subviews]) {
        if ([view isKindOfClass:[VtrPerfOverlayView class]]) return;
    }
    VtrPerfOverlayView* view =
        [[VtrPerfOverlayView alloc] initWithFrame:NSMakeRect(kMargin, kMargin, 10, 10)];
    view.autoresizingMask = content.isFlipped
                                ? (NSViewMaxXMargin | NSViewMaxYMargin)
                                : (NSViewMaxXMargin | NSViewMinYMargin);
    view.hidden = g_mode == Mode::off;
    [content addSubview:view positioned:NSWindowAbove relativeTo:nil];
    [view release];
    if (g_timer == nil) {
        g_timer = [[NSTimer timerWithTimeInterval:kRefreshS
                                           target:[VtrPerfOverlayController shared]
                                         selector:@selector(tick:)
                                         userInfo:nil
                                          repeats:YES] retain];
        g_timer.tolerance = kRefreshS / 4;
        [[NSRunLoop mainRunLoop] addTimer:g_timer forMode:NSRunLoopCommonModes];
    }
    [[VtrPerfOverlayController shared] tick:nil];
}

bool vtr_perf_overlay_consume_key_event(void* ns_event) {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    if (event == nil) return false;
    if (event.type == NSEventTypeKeyUp) {
        if (g_swallow_key_up && event.keyCode == kKeyCodeP) {
            g_swallow_key_up = false;
            return true;
        }
        return false;
    }
    if (event.type != NSEventTypeKeyDown || event.keyCode != kKeyCodeP) return false;
    const NSEventModifierFlags mods =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask &
        (NSEventModifierFlagShift | NSEventModifierFlagControl |
         NSEventModifierFlagOption | NSEventModifierFlagCommand);
    if (mods != (NSEventModifierFlagOption | NSEventModifierFlagCommand)) return false;
    g_swallow_key_up = true;
    if (!event.isARepeat) [[VtrPerfOverlayController shared] cycle:nil];
    return true;
}

void vtr_perf_overlay_add_menu_item(void* view_menu) {
    NSMenu* menu = (__bridge NSMenu*)view_menu;
    if (menu == nil) return;
    VtrPerfOverlayController* controller = [VtrPerfOverlayController shared];
    for (NSMenuItem* item in [menu itemArray]) {
        if (item.submenu != nil && item.submenu.itemArray.count > 0 &&
            item.submenu.itemArray[0].target == controller) {
            return;
        }
    }
    NSMenu* submenu = [[NSMenu alloc] initWithTitle:@"Performance Overlay"];
    const struct {
        NSString* title;
        Mode mode;
    } choices[] = {{@"Off", Mode::off}, {@"Compact", Mode::compact}, {@"Full", Mode::full}};
    for (const auto& choice : choices) {
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:choice.title
                                                      action:@selector(choose:)
                                               keyEquivalent:@""];
        item.target = controller;
        item.tag = static_cast<NSInteger>(choice.mode);
        [submenu addItem:item];
        [item release];
    }
    [submenu addItem:[NSMenuItem separatorItem]];
    // The event pump consumes ⌥⌘P before AppKit sees it; the equivalent
    // here only shows the shortcut.
    NSMenuItem* cycle = [[NSMenuItem alloc] initWithTitle:@"Next Overlay Mode"
                                                   action:@selector(cycle:)
                                            keyEquivalent:@"p"];
    cycle.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    cycle.target = controller;
    [submenu addItem:cycle];
    [cycle release];

    NSMenuItem* parent = [[NSMenuItem alloc] initWithTitle:@"Performance Overlay"
                                                    action:nil
                                             keyEquivalent:@""];
    parent.submenu = submenu;
    [submenu release];
    [menu insertItem:parent atIndex:0];
    [parent release];
}
