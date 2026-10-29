# Contributing to Vitreo

Vitreo is a clean-room Windows user-mode runtime for macOS on Apple Silicon.
Product code implements observable Windows, NT, CRT, and x86/x64 contracts
for any guest PE. It never implements one application's private behaviour.

## Ground rules

These rules are not negotiable. A change that breaks one of them is not
merged, however much it helps a particular application.

- **No copied code.** Never copy, translate, or adapt implementation code
  from Wine, ReactOS, Windows, Mesa, SGI GLU, FEX-Emu, QEMU, or any other
  emulator or OS. Work from public contracts (Microsoft Learn, SDK headers,
  import libraries, the Intel SDM, the AMD APM, Khronos specifications),
  observed behaviour, and your own reasoning. Other projects may be read for
  ideas. Their code does not enter this tree.
- **No Windows-derived assets or data.** Do not commit pixels, fonts, cursors,
  NLS tables, registry exports, or databases taken from a Windows install.
  Oracle probes record contract facts, such as return values, error codes,
  and layouts. Content comes from macOS (AppKit, CoreText) or from open
  standards (Unicode, CLDR, IANA).
- **No app-specific behaviour under `src/`.** Do not compare executable
  names, module names, or guest addresses. If an application fails, find the
  Windows contract it depends on and fix that contract for every guest.
- **No fake success.** Unsupported behaviour fails the documented way: the
  documented return value, the documented `GetLastError` value, and a single
  `[vitreo] UNIMPLEMENTED <dll>!<api>: <case>` log line. Never return a
  plausible guess.
- **Whole contracts.** An API is finished when every parameter, flag, return
  value, documented error, and output-on-failure is correct on both x86 and
  x64. An instruction is finished when every operand size, addressing form,
  flag effect, and fault matches the SDM in the interpreter and in the JIT.

## Building

See [README.md](README.md#build). On Apple Silicon, use the arm64 CMake from
Homebrew (`/opt/homebrew/bin/cmake`). An x86_64 CMake that runs under
Rosetta configures an Intel build, and that build has no JIT.

## Tests

Every behaviour change comes with a CTest regression test. Its expected
values come from the authoritative source or from an oracle measurement,
never from Vitreo's own output.

```bash
ctest --test-dir build -E "[Oo]racle" --output-on-failure
```

Tests whose names contain `Oracle` compare against a native Windows machine
over SSH. They need a Windows host that you configure yourself. CI does not
run them.

### Oracle probes

Some behaviour that Windows relies on is undocumented, such as TEB fields,
guard sizes, and ordering. Measure that behaviour on real Windows. Write a
small C probe, run the same probe on Windows and under Vitreo, and commit the
Windows output as an `.expected` file next to the test that uses it. Code
comments call the reference Windows machine "winoracle".

## Commits and pull requests

- Keep each commit to one contract or one fix. Say which contract it changes
  and which source it was checked against.
- Run the focused tests for the area you changed, and run the CI subset
  before you open a pull request.
- Do not commit build trees, logs, crash dumps, or captured guest data.

## Licence

By contributing, you agree that your contribution is licensed under the
licence in [LICENSE](LICENSE).
