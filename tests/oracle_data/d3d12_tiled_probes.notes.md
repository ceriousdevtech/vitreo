# Tiled-resources probe lines that are expected to differ

These notes cover `d3d12_a20_tiled_probe` and `d3d12_a21_tiled_paths_probe`
(`*.expected` = WARP, `*.winoracle-gpu.expected` = RTX 4080). The listed
lines are implementation-defined. Under Vitreo they are expected diff lines,
not regressions.

## a20 [status]: filter-weight precision (3 lines)

D3D11.3 7.18.16.1 (Texture Addressing and LOD Precision) requires at least
8 bits of fractional precision for the subtexel position and the LOD
fraction. It does not say how a position between two of those steps rounds.
WARP and the RTX 4080 truncate the subtexel fraction and round the LOD
fraction up. Apple GPUs round the subtexel fraction and truncate the LOD
fraction. The tier-2 status (D3D11.3 5.9.4.5) reports the texels that the
sample actually weighted, so on Metal the status agrees with Metal's value:

| line | Windows | Vitreo (Metal) |
|---|---|---|
| `status sample_straddle_0.1` | value=0x3f66feff mapped=0 (weight 25/256) | value=0x3f660606 mapped=0 (26/256) |
| `status sample_tiny_weight` | value=0x3f800000 mapped=1 (weight 0) | value=0x3f7efeff mapped=0 (1/256 on the NULL texel) |
| `status lod_0.002` | value=0x3f7efeff mapped=0 (mip 1 weight 1/256) | value=0x3f800000 mapped=1 (weight 0) |

The `tier options` line differs as well: the device reports
TiledResourcesTier=2, not 3, because Metal has no standard 3D tile.

## a21 [paths]: WARP and the RTX 4080 disagree (3 lines)

Vitreo matches WARP on these lines:

- `paths structured_mapped`: a 4-byte load at bytes 65532..65535, all in
  the mapped tile. WARP reports mapped=1. The RTX 4080 reports 0, apparently
  checking a coarser span.
- `paths root_srv_mapped` / `paths root_uav_null`: root descriptors carry no
  view. WARP reports the tile of the bytes read. The RTX 4080 reports 0 for
  the root SRV and 1 for the root UAV whatever the tile holds.

## a21 [paths]: typed views of a reserved buffer (Vitreo, 3 lines)

`paths typed_mapped`, `paths uav_typed_mapped` and `paths uav_typed_null`
show value 0 under Vitreo. Their mapped bits match. Metal makes no texture
over a placement-sparse MTLBuffer: `newTextureWithDescriptor` returns nil
(measured on an M4 Pro). Such a view is UNIMPLEMENTED and reads 0. The two
UAV lines read 1.0 on Windows because fxc loads the missing alpha channel
of the R32_FLOAT view.
