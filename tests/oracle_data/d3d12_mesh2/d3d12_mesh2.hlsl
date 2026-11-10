// d3d12_mesh2.hlsl - the second mesh / amplification corpus, for
// d3d12_a42_mesh2_probe.c (compiled by gen_d3d12_mesh2.py with dxc 1.6 on
// winoracle-gpu).
//
// Target 64x64 R8G8B8A8_UNORM cut into an 8x8 grid of 8x8-pixel tiles, as in
// d3d12_mesh.hlsl. Two layouts:
//   quad tile: 4 vertices at tile pixels (1,1) (7,1) (1,7) (7,7); triangle 0 =
//              (0,1,2) is sampled at pixel (2,3), triangle 1 = (1,3,2) at (5,6).
//   tri tile:  one triangle (1,1) (7,1) (1,7), sampled at pixel (2,3).
// Every colour is k/255 and constant over a primitive.
//
// b0: { seed, flags, base, pad }   u0: counters (byte offsets 0, 4, ...).
//
// Limits (DirectX-Specs "Mesh Shader"): MS groupshared + payload <= 28k, AS
// groupshared (payload included) <= 32k, payload <= 16k, MS output
// vert_attr_bytes * ALIGN32(n_verts) + prim_attr_bytes * ALIGN32(n_prims)
// <= 32768 (SV_CullPrimitive not counted), AS payload + MS output <= 47k.

cbuffer Params : register(b0) {
    uint g_seed;
    uint g_flags;
    uint g_base;
    uint g_pad;
};
RWByteAddressBuffer g_counter : register(u0);

float4 px_to_ndc(float x, float y) { return float4(x / 32.0 - 1.0, 1.0 - y / 32.0, 0.5, 1.0); }

float4 quad_corner(uint tile, uint corner) {
    uint tx = tile % 8u, ty = tile / 8u;
    return px_to_ndc(float(tx * 8u + ((corner & 1u) ? 7u : 1u)), float(ty * 8u + ((corner & 2u) ? 7u : 1u)));
}

float4 tri_corner(uint tile, uint corner) {
    uint tx = tile % 8u, ty = tile / 8u;
    float x = corner == 1u ? 7.0 : 1.0;
    float y = corner == 2u ? 7.0 : 1.0;
    return px_to_ndc(float(tx * 8u) + x, float(ty * 8u) + y);
}

float4 unpack_word(uint w) {
    return float4(float(w & 0xFFu), float((w >> 8) & 0xFFu), float((w >> 16) & 0xFFu), float(w >> 24)) / 255.0;
}

uint rotl(uint v, uint n) { return (v << n) | (v >> (32u - n)); }

struct PosOut {
    float4 pos : SV_Position;
};
struct WordPrim {
    uint word : PRIMWORD;
};
struct WordCullPrim {
    uint word : PRIMWORD;
    bool cull : SV_CullPrimitive;
};

// ======================================================== 1. bigtg

// 12 KB payload: with the 16 KB groupshared of ms_bigtg the MS is exactly at
// its 28k limit. Outputs: 256 x 4 rows (16384) + 128 x 2 rows (4096).
struct BigPayload {
    uint words[3072];
};

struct BigV {
    float4 pos : SV_Position;
    float4 a1 : ATTR1;
    float4 a2 : ATTR2;
    float4 a3 : ATTR3;
};
struct BigP {
    uint4 d0 : PDATA0;
    uint4 d1 : PDATA1;
};

#ifdef AS_BIG
groupshared BigPayload s_bp;
[numthreads(128, 1, 1)]
void as_big(uint gi : SV_GroupIndex) {
    for (uint k = 0; k < 24u; ++k) {
        uint i = gi * 24u + k;
        s_bp.words[i] = (i * 2654435761u) ^ g_seed;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(1, 1, 1, s_bp);
}
#endif

#ifdef MS_BIGTG
groupshared uint s_gs[4096];
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void ms_bigtg(uint gi : SV_GroupIndex, in payload BigPayload pl, out vertices BigV verts[256],
              out indices uint3 tris[128], out primitives BigP prims[128]) {
    SetMeshOutputCounts(256, 128);
    for (uint k = 0; k < 32u; ++k) {
        uint i = gi * 32u + k;
        s_gs[i] = pl.words[(i * 7u) % 3072u] + i * 0x01010101u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = 0; j < 2u; ++j) {
        uint v = gi * 2u + j;
        uint tile = v / 4u;
        BigV o;
        o.pos = quad_corner(tile, v % 4u);
        o.a1 = float4(float(tile) / 255.0, 0.25, 0.5, 0.75);
        o.a2 = float4(0.125, 64.0 / 255.0, 0.375, 0.625);
        o.a3 = float4(0.875, 0.0625, 128.0 / 255.0, 1.0);
        verts[v] = o;
    }
    uint t = gi / 2u;
    tris[gi] = (gi & 1u) == 0u ? uint3(t * 4u, t * 4u + 1u, t * 4u + 2u) : uint3(t * 4u + 1u, t * 4u + 3u, t * 4u + 2u);
    BigP p;
    p.d0 = uint4(pl.words[gi * 24u + 5u], s_gs[4095u - gi * 32u], pl.words[3071u - gi], s_gs[(gi * 37u + 11u) % 4096u]);
    p.d1 = uint4(s_gs[gi * 32u + 31u], pl.words[(gi * 101u) % 3072u], s_gs[(gi * 613u) % 4096u], pl.words[gi]);
    prims[gi] = p;
}
#endif

// A full 16 KB payload (the AS limit) into an MS with 12 KB of groupshared
// memory (12 + 16 = the MS's 28k) and ms_bigtg's outputs.
struct FullPayload {
    uint words[4096];
};

#ifdef AS_FULL
groupshared FullPayload s_fp;
[numthreads(128, 1, 1)]
void as_full(uint gi : SV_GroupIndex) {
    for (uint k = 0; k < 32u; ++k) {
        uint i = gi * 32u + k;
        s_fp.words[i] = (i * 2246822519u) ^ g_seed;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(1, 1, 1, s_fp);
}
#endif

#ifdef MS_FULL
groupshared uint s_gf[3072];
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void ms_full(uint gi : SV_GroupIndex, in payload FullPayload pl, out vertices BigV verts[256],
             out indices uint3 tris[128], out primitives BigP prims[128]) {
    SetMeshOutputCounts(256, 128);
    for (uint k = 0; k < 24u; ++k) {
        uint i = gi * 24u + k;
        s_gf[i] = pl.words[(i * 11u) % 4096u] + i * 0x01010101u;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = 0; j < 2u; ++j) {
        uint v = gi * 2u + j;
        uint tile = v / 4u;
        BigV o;
        o.pos = quad_corner(tile, v % 4u);
        o.a1 = float4(float(tile) / 255.0, 0.25, 0.5, 0.75);
        o.a2 = float4(0.125, 64.0 / 255.0, 0.375, 0.625);
        o.a3 = float4(0.875, 0.0625, 128.0 / 255.0, 1.0);
        verts[v] = o;
    }
    uint t = gi / 2u;
    tris[gi] = (gi & 1u) == 0u ? uint3(t * 4u, t * 4u + 1u, t * 4u + 2u) : uint3(t * 4u + 1u, t * 4u + 3u, t * 4u + 2u);
    BigP p;
    p.d0 = uint4(pl.words[4095u - gi], s_gf[3071u - gi * 24u], pl.words[gi * 32u + 31u], s_gf[(gi * 37u + 11u) % 3072u]);
    p.d1 = uint4(s_gf[gi * 24u + 23u], pl.words[(gi * 101u) % 4096u], pl.words[4064u + (gi & 31u)], pl.words[gi]);
    prims[gi] = p;
}
#endif

#ifdef PS_BIG
float4 ps_big(float4 pos : SV_Position, float4 a1 : ATTR1, float4 a2 : ATTR2, float4 a3 : ATTR3,
              nointerpolation uint4 d0 : PDATA0, nointerpolation uint4 d1 : PDATA1) : SV_Target {
    uint h = d0.x ^ rotl(d0.y, 8u) ^ rotl(d0.z, 16u) ^ rotl(d0.w, 24u) ^ rotl(d1.x, 4u) ^ rotl(d1.y, 12u) ^
             rotl(d1.z, 20u) ^ rotl(d1.w, 28u);
    float4 c = unpack_word(h);
    return float4(c.rgb, a1.x + a2.y + a3.z);
}
#endif

// MS only: 28 KB groupshared (its whole limit) and exactly 32768 bytes of
// output: 256 x 5 rows (20480) + 128 x 6 rows (12288).
struct Big2V {
    float4 pos : SV_Position;
    float4 a1 : ATTR1;
    float4 a2 : ATTR2;
    float4 a3 : ATTR3;
    float4 a4 : ATTR4;
};
struct Big2P {
    uint4 d0 : PDATA0;
    uint4 d1 : PDATA1;
    uint4 d2 : PDATA2;
    uint4 d3 : PDATA3;
    uint4 d4 : PDATA4;
    uint4 d5 : PDATA5;
};

#ifdef MS_BIGTG2
groupshared uint s_gs2[7168];
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void ms_bigtg2(uint gi : SV_GroupIndex, out vertices Big2V verts[256], out indices uint3 tris[128],
               out primitives Big2P prims[128]) {
    SetMeshOutputCounts(256, 128);
    for (uint k = 0; k < 56u; ++k) {
        uint i = gi * 56u + k;
        s_gs2[i] = (i * 0x9E3779B9u) ^ g_seed ^ (i << 20);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = 0; j < 2u; ++j) {
        uint v = gi * 2u + j;
        uint tile = v / 4u;
        Big2V o;
        o.pos = quad_corner(tile, v % 4u);
        o.a1 = float4(float(tile) / 255.0, 0.25, 0.5, 0.75);
        o.a2 = float4(0.125, 32.0 / 255.0, 0.375, 0.625);
        o.a3 = float4(0.875, 0.0625, 64.0 / 255.0, 1.0);
        o.a4 = float4(0.5, 0.5, 0.5, 96.0 / 255.0);
        verts[v] = o;
    }
    uint t = gi / 2u;
    tris[gi] = (gi & 1u) == 0u ? uint3(t * 4u, t * 4u + 1u, t * 4u + 2u) : uint3(t * 4u + 1u, t * 4u + 3u, t * 4u + 2u);
    Big2P p;
    uint b = 7167u - gi * 56u;
    p.d0 = uint4(s_gs2[b], s_gs2[b - 1u], s_gs2[b - 2u], s_gs2[b - 3u]);
    p.d1 = uint4(s_gs2[(gi * 211u) % 7168u], s_gs2[(gi * 997u + 3u) % 7168u], s_gs2[gi], s_gs2[gi + 7000u]);
    p.d2 = uint4(s_gs2[(gi * 13u) % 7168u], s_gs2[(gi * 17u + 1u) % 7168u], s_gs2[(gi * 19u + 2u) % 7168u],
                 s_gs2[(gi * 23u + 3u) % 7168u]);
    p.d3 = uint4(s_gs2[(gi * 29u + 4u) % 7168u], s_gs2[(gi * 31u + 5u) % 7168u], s_gs2[(gi * 37u + 6u) % 7168u],
                 s_gs2[(gi * 41u + 7u) % 7168u]);
    p.d4 = uint4(s_gs2[3584u + gi], s_gs2[3584u - gi], s_gs2[5000u + gi], s_gs2[1000u + gi]);
    p.d5 = uint4(gi, t, s_gs2[6000u + gi], s_gs2[2000u + gi]);
    prims[gi] = p;
}
#endif

#ifdef PS_BIG2
float4 ps_big2(float4 pos : SV_Position, float4 a1 : ATTR1, float4 a2 : ATTR2, float4 a3 : ATTR3, float4 a4 : ATTR4,
               nointerpolation uint4 d0 : PDATA0, nointerpolation uint4 d1 : PDATA1,
               nointerpolation uint4 d2 : PDATA2, nointerpolation uint4 d3 : PDATA3,
               nointerpolation uint4 d4 : PDATA4, nointerpolation uint4 d5 : PDATA5) : SV_Target {
    uint4 m = d0 ^ uint4(rotl(d1.x, 3u), rotl(d1.y, 5u), rotl(d1.z, 7u), rotl(d1.w, 9u)) ^
              uint4(rotl(d2.x, 11u), rotl(d2.y, 13u), rotl(d2.z, 15u), rotl(d2.w, 17u)) ^
              uint4(rotl(d3.x, 19u), rotl(d3.y, 21u), rotl(d3.z, 23u), rotl(d3.w, 25u)) ^
              uint4(rotl(d4.x, 27u), rotl(d4.y, 29u), rotl(d4.z, 31u), rotl(d4.w, 2u)) ^
              uint4(rotl(d5.x, 4u), rotl(d5.y, 6u), rotl(d5.z, 8u), rotl(d5.w, 10u));
    uint h = m.x ^ rotl(m.y, 8u) ^ rotl(m.z, 16u) ^ rotl(m.w, 24u);
    float4 c = unpack_word(h);
    return float4(c.rgb, a1.x + a2.y + a3.z + a4.w);
}
#endif

// ======================================================== 2. cr / tir

// Three triangles in pixel coordinates (64x64 viewport), every edge at least
// 0.18 px from a pixel centre and from a pixel square boundary it must not
// touch:
//   T0 tiny, inside pixel (8,8), covers no pixel centre.
//   T1 sliver y 20.30..20.45, x 20.25..40.75, covers no pixel centre.
//   T2 right triangle (40.25,40.25) (56.5,40.25) (40.25,56.5), hypotenuse
//      x + y = 96.75.
#ifdef MS_CR
static const float2 kCrVerts[9] = {
    float2(8.3, 8.3),    float2(8.7, 8.3),   float2(8.3, 8.45),
    float2(20.25, 20.3), float2(40.75, 20.3), float2(20.25, 20.45),
    float2(40.25, 40.25), float2(56.5, 40.25), float2(40.25, 56.5),
};
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void ms_cr(uint gi : SV_GroupIndex, out vertices PosOut verts[9], out indices uint3 tris[3]) {
    SetMeshOutputCounts(9, 3);
    for (uint k = 0; k < 3u; ++k) {
        float2 p = kCrVerts[gi * 3u + k];
        verts[gi * 3u + k].pos = px_to_ndc(p.x, p.y);
    }
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
}
#endif

#ifdef PS_WHITE
float4 ps_white(float4 pos : SV_Position) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }
#endif

// The pixel rectangle [8,16) x [8,16) as two triangles.
#ifdef MS_RECT
[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms_rect(uint gi : SV_GroupIndex, out vertices PosOut verts[4], out indices uint3 tris[2]) {
    SetMeshOutputCounts(4, 2);
    verts[gi].pos = px_to_ndc((gi & 1u) ? 16.0 : 8.0, (gi & 2u) ? 16.0 : 8.0);
    if (gi < 2u) tris[gi] = gi == 0u ? uint3(0, 1, 2) : uint3(1, 3, 2);
}
#endif

// Target-independent rasterization: no render target; u0[0] += covered
// samples, u0[4] += invocations.
#ifdef PS_TIR
void ps_tir(float4 pos : SV_Position, uint cov : SV_Coverage) {
    g_counter.InterlockedAdd(0, countbits(cov));
    g_counter.InterlockedAdd(4, 1);
}
#endif

// ======================================================== 3. primid

// Eight tri tiles 0..7, SV_PrimitiveID = 100 + (i * 5) % 8, primitive 7 adds
// 0x12300.
struct PidPrim {
    uint pid : SV_PrimitiveID;
};

#ifdef MS_PID
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void ms_pid(uint gi : SV_GroupIndex, out vertices PosOut verts[24], out indices uint3 tris[8],
            out primitives PidPrim prims[8]) {
    SetMeshOutputCounts(24, 8);
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = tri_corner(gi, k);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
    prims[gi].pid = 100u + ((gi * 5u) & 7u) + (gi == 7u ? 0x12300u : 0u);
}
#endif

#ifdef MS_NOPID
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void ms_nopid(uint gi : SV_GroupIndex, out vertices PosOut verts[24], out indices uint3 tris[8]) {
    SetMeshOutputCounts(24, 8);
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = tri_corner(gi, k);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
}
#endif

#ifdef PS_PID
float4 ps_pid(float4 pos : SV_Position, uint pid : SV_PrimitiveID) : SV_Target {
    return float4(float(pid & 0xFFu), float((pid >> 8) & 0xFFu), float((pid >> 16) & 0xFFu), 255.0) / 255.0;
}
#endif

// ======================================================== 5. payload

// Same 32-byte size, different 32-bit layouts.
struct P32 {
    uint a;
    float b;
    uint2 c;
    float4 d;
};
struct Q32 {
    float x;
    uint y;
    float z;
    uint w;
    uint4 v;
};
// 16-bit and bool fields (needs -enable-16bit-types), and its 12-byte
// uint view.
#if defined(AS_R16) || defined(MS_R16)
struct R16 {
    bool flag;
    uint16_t h0;
    uint16_t h1;
    uint tail;
};
#endif
struct R12 {
    uint a;
    uint b;
    uint c;
};
struct S48 {
    uint w[12];
};

#ifdef AS_P32
groupshared P32 s_p32;
[numthreads(1, 1, 1)]
void as_p32() {
    s_p32.a = 0x3F8040C0u;
    s_p32.b = asfloat(0x40A0B0C0u);
    s_p32.c = uint2(0x11223344u, 0x55667788u);
    s_p32.d = asfloat(uint4(0x3E7F1020u, 0xBF102030u, 0x42C8E0F0u, 0x0D0E0F10u));
    DispatchMesh(1, 1, 1, s_p32);
}
#endif

#ifdef MS_Q32
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void ms_q32(uint gi : SV_GroupIndex, in payload Q32 pl, out vertices PosOut verts[24], out indices uint3 tris[8],
            out primitives WordPrim prims[8]) {
    SetMeshOutputCounts(24, 8);
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = tri_corner(gi, k);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
    uint words[8] = {asuint(pl.x), pl.y, asuint(pl.z), pl.w, pl.v.x, pl.v.y, pl.v.z, pl.v.w};
    prims[gi].word = words[gi];
}
#endif

#ifdef AS_R16
groupshared R16 s_r16;
[numthreads(1, 1, 1)]
void as_r16() {
    s_r16.flag = g_flags != 7u;  // true for the probe's flags 0
    s_r16.h0 = (uint16_t)0x1234u;
    s_r16.h1 = (uint16_t)0xABCDu;
    s_r16.tail = 0x55AA33CCu;
    DispatchMesh(1, 1, 1, s_r16);
}
#endif

#ifdef MS_R16
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void ms_r16(uint gi : SV_GroupIndex, in payload R16 pl, out vertices PosOut verts[9], out indices uint3 tris[3],
            out primitives WordPrim prims[3]) {
    SetMeshOutputCounts(9, 3);
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = tri_corner(gi, k);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
    uint words[3] = {pl.flag ? 0xFF0102F0u : 0xFF000000u, (uint(pl.h1) << 16) | uint(pl.h0), pl.tail};
    prims[gi].word = words[gi];
}
#endif

#ifdef MS_R12
[outputtopology("triangle")]
[numthreads(3, 1, 1)]
void ms_r12(uint gi : SV_GroupIndex, in payload R12 pl, out vertices PosOut verts[9], out indices uint3 tris[3],
            out primitives WordPrim prims[3]) {
    SetMeshOutputCounts(9, 3);
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = tri_corner(gi, k);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
    uint words[3] = {pl.a, pl.b, pl.c};
    prims[gi].word = words[gi];
}
#endif

#ifdef MS_S48
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void ms_s48(uint gi : SV_GroupIndex, in payload S48 pl, out vertices PosOut verts[24], out indices uint3 tris[8],
            out primitives WordPrim prims[8]) {
    SetMeshOutputCounts(24, 8);
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = tri_corner(gi, k);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
    prims[gi].word = pl.w[gi] ^ pl.w[gi + 4u];
}
#endif

#ifdef PS_WORD
float4 ps_word(float4 pos : SV_Position, nointerpolation uint word : PRIMWORD) : SV_Target {
    return unpack_word(word);
}
#endif

// ======================================================== 6. cprim

// One group = one row of tri tiles (row = base + group, or the payload's
// row + group): thread i draws primitive i.
//   0..3 tiles 0..3, inside; 4 tile 4, SV_CullPrimitive; 5 tile 5, degenerate
//   (collinear); 6 fully outside (NDC x 3.0 .. 3.5); 7 tile 7 reaching x = 80 px,
//   partly outside the viewport.
struct RowPayload {
    uint row;
};

void emit_cprim(uint row, uint gi, out vertices PosOut verts[24], out indices uint3 tris[8],
                out primitives WordCullPrim prims[8]) {
    SetMeshOutputCounts(24, 8);
    uint tile = row * 8u + gi;
    float ox = float(gi * 8u), oy = float(row * 8u);
    float2 c[3];
    c[0] = float2(1.0, 1.0);
    c[1] = float2(7.0, 1.0);
    c[2] = float2(1.0, 7.0);
    if (gi == 5u) {
        c[1] = float2(4.0, 4.0);
        c[2] = float2(7.0, 7.0);
    } else if (gi == 6u) {
        ox = 128.0;  // NDC x 3.0 .. 3.5
        c[1] = float2(17.0, 1.0);
    } else if (gi == 7u) {
        c[1] = float2(24.0, 1.0);  // x 57 .. 80 px
    }
    for (uint k = 0; k < 3u; ++k) verts[gi * 3u + k].pos = px_to_ndc(ox + c[k].x, oy + c[k].y);
    tris[gi] = uint3(gi * 3u, gi * 3u + 1u, gi * 3u + 2u);
    WordCullPrim p;
    p.word = 0xFF000000u | (tile * 4u) | ((gi * 32u) << 8) | (0x40u << 16);
    p.cull = gi == 4u;
    prims[gi] = p;
}

#ifdef MS_CPRIM
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void ms_cprim(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex, out vertices PosOut verts[24],
              out indices uint3 tris[8], out primitives WordCullPrim prims[8]) {
    emit_cprim(g_base + gid.x, gi, verts, tris, prims);
}
#endif

#ifdef AS_CPRIM
groupshared RowPayload s_row;
[numthreads(4, 1, 1)]
void as_cprim(uint gi : SV_GroupIndex) {
    if (gi == 0u) s_row.row = g_base;
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(2, 1, 1, s_row);
}
#endif

#ifdef MS_CPRIM_PL
[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void ms_cprim_pl(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex, in payload RowPayload pl,
                 out vertices PosOut verts[24], out indices uint3 tris[8], out primitives WordCullPrim prims[8]) {
    emit_cprim(pl.row + gid.x, gi, verts, tris, prims);
}
#endif
