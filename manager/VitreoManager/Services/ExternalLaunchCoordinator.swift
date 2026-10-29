// SPDX-License-Identifier: LGPL-2.1-only

import Combine
import Foundation

/// One validated file-open request.
struct ExternalLaunchRequest: Identifiable, Equatable {
    let id: UUID
    let source: ExternalLaunchSource
    let originalURL: URL
    let canonicalURL: URL
    let displayName: String
    let architecture: InstalledApp.Architecture
    /// Non-nil when the file already lives inside a capsule, which then owns it.
    let owningCapsule: String?

    static func == (lhs: ExternalLaunchRequest, rhs: ExternalLaunchRequest) -> Bool {
        lhs.id == rhs.id
    }
}

/// What the coordinator needs from the Manager to run a request.
///
/// Keeping this narrow lets the selection policy be tested without a live
/// engine, capsule tree, or `UserDefaults`.
@MainActor
protocol ExternalLaunchHosting: AnyObject {
    var capsuleRootDirectory: URL { get }
    var healthyCapsules: [CapsuleInfo] { get }
    /// The configured default capsule, whether or not it is currently healthy.
    var configuredDefaultCapsule: String { get }

    func refreshCapsuleState() async
    @discardableResult
    func launchExternalExecutable(_ executableURL: URL, capsuleName: String) -> UUID?
    func reportExternalLaunchFailure(_ message: String)
}

/// Owns everything between "Finder handed us a file" and "a launch operation
/// started": validation, capsule resolution, and chooser presentation.
@MainActor
final class ExternalLaunchCoordinator: ObservableObject {
    /// The request currently asking the user for a capsule, if any. Only one
    /// chooser is ever visible.
    @Published private(set) var activeRequest: ExternalLaunchRequest?
    /// Capsules offered for `activeRequest`.
    @Published private(set) var offeredCapsules: [CapsuleInfo] = []
    /// Preselected capsule name for `activeRequest`.
    @Published private(set) var suggestedCapsule: String = ""

    private unowned let host: ExternalLaunchHosting
    private let ruleStore: ExternalLaunchRuleStoring
    private var incoming: [(url: URL, source: ExternalLaunchSource)] = []
    private var waitingForChooser: [ExternalLaunchRequest] = []
    private var isDraining = false

    init(host: ExternalLaunchHosting, ruleStore: ExternalLaunchRuleStoring) {
        self.host = host
        self.ruleStore = ruleStore
    }

    var policy: ExternalLaunchPolicy {
        get { ruleStore.policy }
        set { ruleStore.policy = newValue }
    }

    var rememberedRuleCount: Int { ruleStore.ruleCount }

    /// True while any request is being validated, waiting for the user, or
    /// queued behind one that is.
    var hasPendingWork: Bool {
        !incoming.isEmpty || !waitingForChooser.isEmpty
            || activeRequest != nil || isDraining
    }

    func forgetAllRememberedCapsules() {
        ruleStore.removeAllRules()
    }

    /// Queue a file Finder asked Vitreo to open.
    func open(url: URL, source: ExternalLaunchSource) {
        incoming.append((url, source))
        guard !isDraining else { return }
        isDraining = true
        Task { await drain() }
    }

    // MARK: - Queue

    /// Resolve requests in arrival order. A request that needs the user waits
    /// in the chooser queue; it does not hold up requests behind it that can
    /// resolve on their own.
    private func drain() async {
        defer { isDraining = false }
        while !incoming.isEmpty {
            let next = incoming.removeFirst()
            await host.refreshCapsuleState()
            await resolve(url: next.url, source: next.source)
        }
    }

    private func resolve(url: URL, source: ExternalLaunchSource) async {
        let inspection = await Task.detached(priority: .userInitiated) {
            PEInspector.inspect(url: url)
        }.value

        let details: PEInspection
        switch inspection {
        case .success(let value):
            details = value
        case .failure(let error):
            host.reportExternalLaunchFailure(
                "\(url.lastPathComponent): \(error.message)")
            return
        }

        let request = ExternalLaunchRequest(
            id: UUID(),
            source: source,
            originalURL: url,
            canonicalURL: details.canonicalURL,
            displayName: details.canonicalURL.lastPathComponent,
            architecture: details.architecture,
            owningCapsule: CapsuleClassifier.owningCapsule(
                of: details.canonicalURL,
                capsuleRoot: host.capsuleRootDirectory))

        // An executable stored in a capsule is not a candidate for a chooser:
        // running it through a different capsule would expose its own capsule's
        // files to that capsule.
        if let owner = request.owningCapsule {
            guard host.healthyCapsules.contains(where: {
                $0.name.caseInsensitiveCompare(owner) == .orderedSame
            }) else {
                host.reportExternalLaunchFailure(
                    "\(request.displayName) belongs to the capsule “\(owner)”, "
                    + "which is missing or incomplete. Repair or recreate that "
                    + "capsule in Vitreo before opening this file.")
                return
            }
            host.launchExternalExecutable(
                request.canonicalURL, capsuleName: owner)
            return
        }

        guard !host.healthyCapsules.isEmpty else {
            host.reportExternalLaunchFailure(
                "There is no usable capsule to run \(request.displayName) in. "
                + "Create one in Capsules first.")
            return
        }

        if let capsule = automaticCapsule(for: request) {
            host.launchExternalExecutable(
                request.canonicalURL, capsuleName: capsule)
            return
        }

        enqueueForChooser(request)
    }

    /// Remembered rule, then policy. Anything unresolved falls through to the
    /// chooser rather than picking a capsule on the user's behalf.
    private func automaticCapsule(for request: ExternalLaunchRequest) -> String? {
        if request.source == .runWithVitreoService { return nil }

        let path = request.canonicalURL.path
        if let rule = ruleStore.rule(forCanonicalPath: path) {
            if let healthy = healthyCapsuleName(rule.capsuleName) { return healthy }
            // A rule pointing at a capsule that no longer exists is stale.
            ruleStore.removeRule(forCanonicalPath: path)
        }

        guard policy == .useDefaultCapsule else { return nil }
        return healthyCapsuleName(host.configuredDefaultCapsule)
    }

    private func healthyCapsuleName(_ name: String) -> String? {
        host.healthyCapsules.first {
            $0.name.caseInsensitiveCompare(name) == .orderedSame
        }?.name
    }

    private func enqueueForChooser(_ request: ExternalLaunchRequest) {
        waitingForChooser.append(request)
        presentNextIfIdle()
    }

    private func presentNextIfIdle() {
        guard activeRequest == nil, !waitingForChooser.isEmpty else { return }
        let request = waitingForChooser.removeFirst()
        let capsules = host.healthyCapsules
        guard !capsules.isEmpty else {
            host.reportExternalLaunchFailure(
                "There is no usable capsule to run \(request.displayName) in. "
                + "Create one in Capsules first.")
            presentNextIfIdle()
            return
        }
        offeredCapsules = capsules
        suggestedCapsule = healthyCapsuleName(host.configuredDefaultCapsule)
            ?? capsules[0].name
        activeRequest = request
    }

    // MARK: - Chooser results

    func confirm(
        _ request: ExternalLaunchRequest,
        capsuleName: String,
        remember: Bool
    ) {
        finishChooser(for: request)

        guard request.owningCapsule == nil else {
            // Defensive: an owned executable never reaches the chooser, and it
            // must not acquire a different capsule if it somehow does.
            host.reportExternalLaunchFailure(
                "\(request.displayName) can only run in its own capsule.")
            return
        }
        guard let capsule = healthyCapsuleName(capsuleName) else {
            host.reportExternalLaunchFailure(
                "The capsule “\(capsuleName)” is no longer available.")
            return
        }

        if remember {
            ruleStore.setRule(ExternalLaunchRule(
                canonicalPath: request.canonicalURL.path,
                capsuleName: capsule))
            if let failure = ruleStore.lastPersistenceError {
                host.reportExternalLaunchFailure(failure)
            }
        }
        host.launchExternalExecutable(request.canonicalURL, capsuleName: capsule)
    }

    /// Cancelling drops only this request and moves on to the next one.
    func cancel(_ request: ExternalLaunchRequest) {
        finishChooser(for: request)
    }

    private func finishChooser(for request: ExternalLaunchRequest) {
        if activeRequest?.id == request.id {
            activeRequest = nil
            offeredCapsules = []
            suggestedCapsule = ""
        } else {
            waitingForChooser.removeAll { $0.id == request.id }
        }
        presentNextIfIdle()
    }
}
