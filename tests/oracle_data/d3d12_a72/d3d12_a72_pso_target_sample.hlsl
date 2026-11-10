// SPDX-License-Identifier: LGPL-2.1-only
//
// Shaders of d3d12_a72_pso_target_sample_probe: pixel shaders that write
// SV_Target outputs with no render target behind them, and a UAV-only pixel
// shader that counts its invocations and ORs its coverage. Compiled on
// winoracle-gpu by d3d12_a72_gen_shaders.py (fxc ps/vs_5_0 and dxc *_6_0); the
// probe embeds the bytecode from d3d12_a72_shaders.h.
//
// Colours are exact in UNORM8: 0.2 -> 51, 0.4 -> 102, 0.6 -> 153, 0.8 -> 204.

RWByteAddressBuffer counts : register(u0);

// A full-viewport triangle at depth 0.5 (no input layout).
float4 vs_full(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}

float4 ps_t0() : SV_Target0 { return float4(0.2, 0.4, 0.6, 0.8); }

struct T01 {
    float4 a : SV_Target0;
    float4 b : SV_Target1;
};
T01 ps_t01() {
    T01 o;
    o.a = float4(0.2, 0.4, 0.6, 0.8);
    o.b = float4(1, 0, 1, 1);
    return o;
}

float4 ps_t3() : SV_Target3 { return float4(1, 0, 1, 1); }

struct T03 {
    float4 a : SV_Target0;
    float4 d : SV_Target3;
};
T03 ps_t03() {
    T03 o;
    o.a = float4(0.2, 0.4, 0.6, 0.8);
    o.d = float4(1, 0, 1, 1);
    return o;
}

// counts[0] += 1 per invocation; counts[1] |= the invocation's coverage.
void ps_uav(float4 pos : SV_Position, uint cov : SV_Coverage) {
    uint ignored;
    counts.InterlockedAdd(0, 1, ignored);
    counts.InterlockedOr(4, cov, ignored);
}
