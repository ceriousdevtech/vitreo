// SPDX-License-Identifier: LGPL-2.1-only

import Foundation

/// Decides whether a host path already belongs to a capsule.
///
/// An executable stored inside capsule A must never be launched through capsule
/// B: doing so would publish A's files to B and break the isolation capsules
/// exist to provide. The Manager therefore treats the owning capsule as
/// mandatory rather than as a default the user may override.
enum CapsuleClassifier {
    /// Capsule owning @p url, or nil when the file lives outside every capsule.
    ///
    /// Comparison is component-wise on the canonical path, so `capsules/demo`
    /// and `capsules/demo-old` are never confused, and only files under a
    /// capsule's `drives/<letter>/` tree count — registry, logs, mount metadata
    /// and runtime state are not executable roots.
    ///
    /// The caller is responsible for passing a canonical URL
    /// (`PEInspector.canonicalize`). A symlink outside a capsule that resolves
    /// into one is therefore owned by that capsule, and a symlink inside a
    /// capsule that resolves outside it is external — which is what the
    /// runtime will actually open in both cases.
    static func owningCapsule(of url: URL, capsuleRoot: URL) -> String? {
        let rootComponents = normalizedComponents(capsuleRoot)
        let fileComponents = normalizedComponents(url)

        // <root>/<capsule>/drives/<letter>/<…>/<file>
        guard fileComponents.count >= rootComponents.count + 5 else { return nil }
        for (index, component) in rootComponents.enumerated()
        where fileComponents[index] != component {
            return nil
        }

        let capsuleName = fileComponents[rootComponents.count]
        guard fileComponents[rootComponents.count + 1] == "drives" else {
            return nil
        }
        let drive = fileComponents[rootComponents.count + 2]
        guard drive.count == 1,
              let letter = drive.first,
              letter.isLetter, letter.isASCII else { return nil }
        return capsuleName.isEmpty ? nil : capsuleName
    }

    private static func normalizedComponents(_ url: URL) -> [String] {
        URL(fileURLWithPath: url.resolvingSymlinksInPath().path)
            .standardizedFileURL
            .pathComponents
            .filter { $0 != "/" }
    }
}
