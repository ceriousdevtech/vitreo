// d3d12_frame_bench.hlsl: the shapes of a deferred SM5 frame for the
// tests/winapi/d3d12/test_d3d12_frame_bench.cpp benchmark. Shader model 5.0,
// space 0, compiled with fxc on winoracle-gpu (see the generated header).
// The arithmetic is filler of a realistic binding footprint: what matters to
// the benchmark is the number of CBVs, SRVs and samplers each stage reads.

cbuffer View : register(b0) {
    float4x4 view_proj;
    float4 view_params[8];
};
cbuffer Primitive : register(b1) {
    float4x4 local_to_world;
    float4 primitive_params[4];
};
cbuffer Material : register(b2) {
    float4 material[16];
};
cbuffer PassConstants : register(b3) {
    float4 pass_params[8];
};

struct VsIn {
    float3 position : ATTRIBUTE0;
    float3 normal : ATTRIBUTE1;
    float2 uv : ATTRIBUTE2;
    float4 tangent : ATTRIBUTE3;
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
};

Texture2D<float4> vs_height : register(t0);
SamplerState vs_sampler : register(s0);

VsOut vs_base(VsIn i) {
    VsOut o;
    float4 world = mul(local_to_world, float4(i.position, 1.0));
    float h = vs_height.SampleLevel(vs_sampler, i.uv, 0).x * primitive_params[0].x;
    world.y += h;
    o.position = mul(view_proj, world);
    o.uv = i.uv * primitive_params[1].xy + primitive_params[1].zw;
    o.normal = mul((float3x3)local_to_world, i.normal);
    o.tangent = float4(mul((float3x3)local_to_world, i.tangent.xyz), i.tangent.w);
    return o;
}

float4 vs_depth(float3 position : ATTRIBUTE0) : SV_Position {
    float4 world = mul(local_to_world, float4(position, 1.0));
    return mul(view_proj, world);
}

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);
Texture2D<float4> t4 : register(t4);
Texture2D<float4> t5 : register(t5);
Texture2D<float4> t6 : register(t6);
Texture2D<float4> t7 : register(t7);
Texture2D<float4> t8 : register(t8);
Texture2D<float4> t9 : register(t9);
Texture2D<float4> t10 : register(t10);
Texture2D<float4> t11 : register(t11);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);

struct GBuffer {
    float4 a : SV_Target0;
    float4 b : SV_Target1;
    float4 c : SV_Target2;
    float4 d : SV_Target3;
};

GBuffer ps_base(VsOut i) {
    float2 uv = i.uv;
    float4 base = t0.Sample(s0, uv) * material[0];
    float4 normal = t1.Sample(s1, uv) * 2.0 - 1.0;
    float4 rough = t2.Sample(s0, uv * material[1].xy);
    float4 ao = t3.Sample(s2, uv);
    float4 detail = t4.Sample(s1, uv * 4.0) * material[2];
    float4 mask = t5.Sample(s3, uv);
    float4 e = t6.Sample(s0, uv) + t7.Sample(s1, uv * 0.5);
    float4 f = t8.Sample(s2, uv + material[3].xy) * t9.Sample(s3, uv);
    float4 g = t10.Sample(s0, uv) + t11.Sample(s1, uv.yx);
    GBuffer o;
    float3 n = normalize(i.normal + normal.x * i.tangent.xyz);
    o.a = base * ao + detail * mask.x + view_params[0];
    o.b = float4(n * 0.5 + 0.5, rough.y);
    o.c = rough * pass_params[0] + e * material[4];
    o.d = f + g * primitive_params[2] + material[5];
    return o;
}

// The cascades' and the cube face's layered path: a geometry shader that
// routes each triangle to the slice the primitive constants name.
struct GsIn {
    float4 position : SV_Position;
};
struct GsOut {
    float4 position : SV_Position;
    uint slice : SV_RenderTargetArrayIndex;
};
GsIn vs_layered(float3 position : ATTRIBUTE0) {
    GsIn o;
    o.position = mul(view_proj, mul(local_to_world, float4(position, 1.0)));
    return o;
}
[maxvertexcount(3)]
void gs_layered(triangle GsIn i[3], inout TriangleStream<GsOut> stream) {
    for (uint k = 0; k < 3; ++k) {
        GsOut o;
        o.position = i[k].position;
        o.slice = (uint)primitive_params[3].x;
        stream.Append(o);
    }
}

// Lighting: a full-screen triangle reading the G-buffer and the depth.
struct FsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};
FsOut vs_fullscreen(uint id : SV_VertexID) {
    FsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_light(FsOut i) : SV_Target0 {
    float4 a = t0.Sample(s0, i.uv);
    float4 b = t1.Sample(s0, i.uv);
    float4 c = t2.Sample(s0, i.uv);
    float4 d = t3.Sample(s0, i.uv);
    float z = t4.Sample(s1, i.uv).x;
    float3 l = normalize(pass_params[0].xyz - float3(i.uv, z));
    float ndl = saturate(dot(b.xyz * 2 - 1, l));
    return a * ndl * pass_params[1] + c * d.x + view_params[1] * z;
}

// UI: one texture, the colour in root constants (b3).
FsOut vs_ui(float2 position : ATTRIBUTE0, float2 uv : ATTRIBUTE1) {
    FsOut o;
    o.position = float4(position * pass_params[0].xy + pass_params[0].zw, 0, 1);
    o.uv = uv;
    return o;
}
float4 ps_ui(FsOut i) : SV_Target0 {
    return t0.Sample(s0, i.uv) * pass_params[1];
}

// The cascades' layered path without a geometry shader: the vertex shader
// selects the slice itself (VPAndRTArrayIndexFromAnyShaderFeedingRasterizer).
GsOut vs_layer(float3 position : ATTRIBUTE0) {
    GsOut o;
    o.position = mul(view_proj, mul(local_to_world, float4(position, 1.0)));
    o.slice = (uint)primitive_params[3].x;
    return o;
}

// Post: a 8x8 compute pass from an SRV to a UAV, scaled by the CBV.
Texture2D<float4> post_in : register(t0);
RWTexture2D<float4> post_out : register(u0);
[numthreads(8, 8, 1)]
void cs_post(uint3 id : SV_DispatchThreadID) {
    float4 c = post_in.Load(int3(id.xy, 0));
    post_out[id.xy] = c * pass_params[0] + pass_params[1];
}
