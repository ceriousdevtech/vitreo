# Security Policy

## Status

Vitreo is beta software, provided "as is" without warranty of any kind under
the terms of its [license](LICENSE). Security reports are welcome and are
reviewed as time allows. There is no guarantee of a response, a fix, or a
timeline for either.

## Scope

Vitreo runs untrusted Windows programs. These count as security issues:

- a guest escaping its capsule: reading or writing host files outside the
  capsule's mapped drives, or reaching host APIs that the runtime does not
  expose
- memory corruption in the runtime that a guest can trigger, in the JIT, the
  PE loader, WinAPI marshalling, or the graphics translation
- a crafted PE, shader, font, image, archive, or installer that crashes the
  runtime or `Vitreo.app` in a way that can be exploited
- the manager app or the launcher helper running code, or changing
  permissions, without the user asking

A guest that misbehaves inside its own capsule is a compatibility bug, not a
security issue. Report it as a normal issue.

## Reporting a vulnerability

Please do not open a public issue. Report privately through GitHub's
**Report a vulnerability** button on the repository's Security tab, or email
[info@vitreo.dev](mailto:info@vitreo.dev) with "SECURITY" in the subject.

Include the Vitreo version (`vitreo version`), your macOS version and Mac
model, and the steps or a sample that reproduces the problem. Do not attach
copyrighted Windows software. Instead, say where it can be obtained.

Please give the maintainer a reasonable chance to look at the report before
disclosing it publicly.
