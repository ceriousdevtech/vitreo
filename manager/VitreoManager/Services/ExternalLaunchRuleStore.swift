// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

/// What the Manager does when Finder opens an executable stored outside every
/// capsule.
enum ExternalLaunchPolicy: String, Codable, CaseIterable, Identifiable {
    case askEveryTime
    case useDefaultCapsule

    var id: String { rawValue }

    var title: String {
        switch self {
        case .askEveryTime: return "Ask every time"
        case .useDefaultCapsule: return "Use the default capsule"
        }
    }
}

/// A remembered "this executable runs in this capsule" decision.
struct ExternalLaunchRule: Codable, Equatable {
    let canonicalPath: String
    let capsuleName: String
}

/// Narrow persistence seam, so selection policy can be tested without
/// `UserDefaults`. It is deliberately not a general persistence framework.
@MainActor
protocol ExternalLaunchRuleStoring: AnyObject {
    var policy: ExternalLaunchPolicy { get set }
    var ruleCount: Int { get }
    /// Set when the last write could not be persisted; nil once one succeeds.
    var lastPersistenceError: String? { get }

    func rule(forCanonicalPath path: String) -> ExternalLaunchRule?
    func setRule(_ rule: ExternalLaunchRule)
    func removeRule(forCanonicalPath path: String)
    func removeAllRules()
}

/// `UserDefaults`-backed rule store using versioned keys.
///
/// Rules are keyed by canonical absolute host path. A moved or renamed file
/// simply has no rule — the Manager asks again rather than guessing, and no
/// executable content is hashed, so opening a multi-gigabyte installer stays
/// instant.
@MainActor
final class ExternalLaunchRuleStore: ExternalLaunchRuleStoring {
    private enum Keys {
        static let policy = "externalLaunchPolicy"
        static let rules = "externalLaunchRules.v1"
    }

    private let defaults: UserDefaults
    private var rules: [String: String]
    private(set) var lastPersistenceError: String?

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: Keys.rules),
           let decoded = try? JSONDecoder().decode(
            [String: String].self, from: data) {
            rules = decoded
        } else {
            rules = [:]
        }
    }

    var policy: ExternalLaunchPolicy {
        get {
            guard let raw = defaults.string(forKey: Keys.policy),
                  let value = ExternalLaunchPolicy(rawValue: raw) else {
                // Opening a downloaded file must not silently change a
                // capsule's state, so the shipped default asks.
                return .askEveryTime
            }
            return value
        }
        set { defaults.set(newValue.rawValue, forKey: Keys.policy) }
    }

    var ruleCount: Int { rules.count }

    func rule(forCanonicalPath path: String) -> ExternalLaunchRule? {
        guard let capsule = rules[path] else { return nil }
        return ExternalLaunchRule(canonicalPath: path, capsuleName: capsule)
    }

    func setRule(_ rule: ExternalLaunchRule) {
        rules[rule.canonicalPath] = rule.capsuleName
        persist()
    }

    func removeRule(forCanonicalPath path: String) {
        guard rules.removeValue(forKey: path) != nil else { return }
        persist()
    }

    func removeAllRules() {
        rules.removeAll()
        persist()
    }

    /// A failed write leaves the in-memory decision intact: the launch the user
    /// just authorized still proceeds, and only the "remember this" promise is
    /// reported as broken.
    private func persist() {
        do {
            defaults.set(try JSONEncoder().encode(rules), forKey: Keys.rules)
            lastPersistenceError = nil
        } catch {
            lastPersistenceError =
                "Vitreo could not save the capsule choice for this executable: "
                + error.localizedDescription
        }
    }
}
