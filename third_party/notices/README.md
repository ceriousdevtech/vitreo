# Third-party notices

Vitreo is licensed under the GNU Lesser General Public License, version 2.1 only
(`LICENSE` at the repository root). Nothing in this directory licenses Vitreo.
These files are the notices for third-party works the build actually compiles
or links. Keep them with any copy of those works.

The license texts under `runtime/` and `tests/` are the upstream files for the
pinned versions, unchanged. The two Khronos and Microsoft notice files quote
the copyright and permission text from the sources named in each file.

## Runtime binary (`vitreo`)

The build copies `runtime/` to `third-party-notices/` in the same directory as
the `vitreo` executable. Ship that directory with the executable. The works
below are statically linked, so the executable is a copy of them.

| Work | Pin | Notice file | What the binary contains |
| --- | --- | --- | --- |
| SPIRV-Cross | `vulkan-sdk-1.3.283.0` | `spirv-cross-LICENSE` | Apache License 2.0. That tag has no NOTICE file. |
| glslang | `vulkan-sdk-1.3.283.0` | `glslang-LICENSE.txt` | The upstream license text (BSD-3, BSD-2, MIT, Apache 2.0, and the other terms in that file). Linked pieces are `glslang`, `SPIRV`, and `glslang-default-resource-limits`. SPIRV-Tools is off (`ENABLE_OPT`). That tag has no NOTICE file. |
| SPIR-V header `spirv.hpp` | the copy inside the SPIRV-Cross pin | `khronos-spirv-header-LICENSE` | Khronos copyright and permission notice, compiled in through SPIRV-Cross. |
| D3D11 fixed-function tessellator reference | May 7, 2012 | `microsoft-d3d11-tessellator-LICENSE` | MIT. The same notice is in `src/graphics/shaders/tessellator.hpp`. |

If a pin in the root `CMakeLists.txt` changes, replace the matching file in
`runtime/` from that tag before shipping a binary.

## Test binaries

GoogleTest v1.14.0 (`tests/googletest-LICENSE`, BSD-3-Clause) is linked only
into test executables. The test build copies `tests/` to
`third-party-notices/` under the test binary directory. It is not part of
`vitreo`.
