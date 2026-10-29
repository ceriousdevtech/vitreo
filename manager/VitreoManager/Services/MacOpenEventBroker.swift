// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

/// Where a file-open request came from.
enum ExternalLaunchSource: Sendable {
    /// Finder double-click, `Open With`, or `open -a Vitreo`.
    case launchServices
    /// The explicit "Run with Vitreo…" action, which always asks.
    case runWithVitreoService
}

/// Receives file URLs from AppKit and holds them until a consumer attaches.
///
/// A cold launch delivers `application(_:open:)` before SwiftUI has built the
/// scene, so events would otherwise be dropped on exactly the path users take
/// most: double-clicking an `.exe` while Vitreo is not running. The broker is
/// owned by the app delegate rather than being a global, so tests can drive a
/// fresh one.
///
/// The broker only queues. It does not read files, choose capsules, or start
/// processes.
@MainActor
final class MacOpenEventBroker {
    /// A single Finder event carrying an unreasonable number of files is more
    /// likely a mis-selection than an intent to start hundreds of Windows
    /// processes, so the surplus is reported instead of launched.
    static let maximumURLsPerEvent = 32

    private var pending: [(url: URL, source: ExternalLaunchSource)] = []
    private var consumer: ((URL, ExternalLaunchSource) -> Void)?

    /// Number of URLs waiting for a consumer. Cleared once one attaches.
    var pendingCount: Int { pending.count }

    /// Reports how many URLs of an event were ignored, for user-facing errors.
    var onOverflow: ((Int) -> Void)?

    func submit(urls: [URL], source: ExternalLaunchSource) {
        let accepted = urls.prefix(Self.maximumURLsPerEvent)
        if urls.count > accepted.count {
            onOverflow?(urls.count - accepted.count)
        }
        for url in accepted { submit(url: url, source: source) }
    }

    func submit(url: URL, source: ExternalLaunchSource) {
        if let consumer {
            consumer(url, source)
        } else {
            pending.append((url, source))
        }
    }

    /// Attach the consumer and hand it everything received so far, in order.
    func attach(_ consumer: @escaping (URL, ExternalLaunchSource) -> Void) {
        self.consumer = consumer
        let queued = pending
        pending.removeAll()
        for entry in queued { consumer(entry.url, entry.source) }
    }

    func detach() {
        consumer = nil
    }
}
