# Oracle data

These files record what native Windows does. The tests read or compile
them:

- `*.expected`: the output of a small probe program run on Windows 11
  (x86 and x64). Each line records a return value, a `GetLastError` value,
  an output buffer, or a layout.
- `*_shaders.h`, `d3d11_oracle/`, `d3d12_*/`: `fxc` / `dxc` output of
  project-written HLSL, plus the device results measured with it.
- `*_inputs.h`, `crypt_*.h`, `*.tsv`: input corpora and export lists that the
  same probes used.

They are contract facts: return values, error codes, layouts, and export
names. They contain no Windows code, pixels, fonts, or databases.

The probe programs and generators that produce these files are maintained
outside this repository. Do not edit a file here by hand. Re-record it on
Windows instead.

The Windows account and paths in the recordings are neutral placeholders:
the account is `C:\Users\Owner`. Tests compare against these files
byte for byte, so the placeholder has the same length as the original
account name.
