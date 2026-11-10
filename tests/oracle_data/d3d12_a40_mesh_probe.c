// SPDX-License-Identifier: LGPL-2.1-only
//
// d3d12_a40_mesh_probe.c - D3D12 mesh and amplification shaders (DirectX-Specs
// "Mesh Shader"): the caps, pipeline-state-stream creation and its refusals,
// DispatchMesh direct and through ExecuteIndirect, meshlets with per-vertex
// and per-primitive data, SV_CullPrimitive, the AS -> MS payload, and the
// PIPELINE_STATISTICS1 AS / MS counters. Shaders: d3d12_mesh/d3d12_mesh.hlsl
// (gen_d3d12_mesh.py, dxc 1.6 on winoracle-gpu). Runs on the default adapter:
// the oracle is winoracle-gpu (RTX 4080 SUPER, mesh shader tier 1).
//
// Output: every compared line is a value the spec defines (HRESULTs, exact
// k/255 colours constant over a primitive and sampled far from its edges,
// thread and group counts, AS / MS invocations, MSPrimitives). The caps and
// the counters whose value is implementation-specific (CInvocations,
// CPrimitives, PSInvocations: the rasterizer's) are '#' lines.
//
// Build: x86_64-w64-mingw32-gcc -O1 -static -Wno-unused-function
//        -I<repo>/tools/oracle_probe -o d3d12_a40_mesh_probe.exe d3d12_a40_mesh_probe.c -luuid -lole32
#include "d3d12_probe_common.h"

#include "d3d12_mesh/d3d12_mesh_shaders.h"

static const MeshBlob* blob(const char* name) {
    for (unsigned i = 0; i < sizeof(kMeshBlobs) / sizeof(kMeshBlobs[0]); ++i)
        if (strcmp(kMeshBlobs[i].name, name) == 0) return &kMeshBlobs[i];
    return NULL;
}

static D3D12_SHADER_BYTECODE code(const char* name) {
    D3D12_SHADER_BYTECODE c = {NULL, 0};
    const MeshBlob* b = name ? blob(name) : NULL;
    if (b) {
        c.pShaderBytecode = b->code;
        c.BytecodeLength = b->size;
    }
    return c;
}

// ---------------------------------------------------------------- the stream

typedef struct {
    unsigned char bytes[2048];
    size_t used;
} Stream;

static void put(Stream* s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const void* data, size_t size, size_t align) {
    s->used = (s->used + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    memcpy(s->bytes + s->used, &type, sizeof(type));
    size_t at = (s->used + sizeof(type) + align - 1) & ~(align - 1);
    memcpy(s->bytes + at, data, size);
    s->used = at + size;
}

typedef struct {
    const char* as;
    const char* ms;
    const char* ps;
    const char* vs;
    int rtv;  // 1: one RGBA8 target
    int topology;  // D3D12_PRIMITIVE_TOPOLOGY_TYPE, -1: no subobject
    int input_layout;
    int stream_output;
} PsoCase;

static ID3D12RootSignature* g_rs;

static ID3D12RootSignature* mesh_root(void) {
    if (g_rs) return g_rs;
    D3D12_ROOT_PARAMETER p[2];
    p[0] = rp_consts(0, 0, 4);
    p[1] = rp_view(D3D12_ROOT_PARAMETER_TYPE_UAV, 0);
    g_rs = root_sig(2, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE);
    return g_rs;
}

static HRESULT make_pso(const PsoCase* c, ID3D12PipelineState** out) {
    *out = NULL;
    ID3D12Device2* dev2 = NULL;
    HRESULT hr = ID3D12Device_QueryInterface(g_dev, &IID_ID3D12Device2, (void**)&dev2);
    if (FAILED(hr)) return hr;
    Stream s;
    memset(&s, 0, sizeof(s));
    ID3D12RootSignature* rs = mesh_root();
    put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &rs, sizeof(rs), sizeof(void*));
    D3D12_SHADER_BYTECODE bc;
    if (c->as) { bc = code(c->as); put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->ms) { bc = code(c->ms); put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->vs) { bc = code(c->vs); put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->ps) { bc = code(c->ps); put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->rtv) {
        struct D3D12_RT_FORMAT_ARRAY rt;
        memset(&rt, 0, sizeof(rt));
        rt.NumRenderTargets = 1;
        rt.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rt, sizeof(rt), 4);
    }
    if (c->topology >= 0) {
        D3D12_PRIMITIVE_TOPOLOGY_TYPE t = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)c->topology;
        put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &t, sizeof(t), 4);
    }
    D3D12_RASTERIZER_DESC r;
    memset(&r, 0, sizeof(r));
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;
    r.DepthClipEnable = TRUE;
    put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &r, sizeof(r), 4);
    D3D12_DEPTH_STENCIL_DESC1 ds;
    memset(&ds, 0, sizeof(ds));
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds, sizeof(ds), 4);
    D3D12_INPUT_ELEMENT_DESC il = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    if (c->input_layout) {
        D3D12_INPUT_LAYOUT_DESC d = {&il, 1};
        put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &d, sizeof(d), sizeof(void*));
    }
    D3D12_SO_DECLARATION_ENTRY so = {0, "SV_Position", 0, 0, 4, 0};
    UINT stride = 16;
    if (c->stream_output) {
        D3D12_STREAM_OUTPUT_DESC d = {&so, 1, &stride, 1, 0};
        put(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, &d, sizeof(d), sizeof(void*));
    }
    D3D12_PIPELINE_STATE_STREAM_DESC sd = {s.used, s.bytes};
    hr = ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState, (void**)out);
    release(dev2);
    return hr;
}

// ---------------------------------------------------------------- sections

static void caps(void) {
    if (!need_device()) return;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7;
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9;
    D3D12_FEATURE_DATA_D3D12_OPTIONS10 o10;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3;
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6;
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_5};
    memset(&o7, 0, sizeof(o7));
    memset(&o9, 0, sizeof(o9));
    memset(&o10, 0, sizeof(o10));
    memset(&o12, 0, sizeof(o12));
    memset(&o3, 0, sizeof(o3));
    memset(&o6, 0, sizeof(o6));
    HRESULT h7 = ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    HRESULT h9 = ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9));
    HRESULT h10 = ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS10, &o10, sizeof(o10));
    HRESULT h12 = ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12));
    ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6));
    ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    printf("# caps OPTIONS7 %s MeshShaderTier=%u SamplerFeedbackTier=%u\n", H(h7), o7.MeshShaderTier,
           o7.SamplerFeedbackTier);
    printf("# caps OPTIONS9 %s MeshShaderPipelineStatsSupported=%u FullRangeRTAI=%u Derivatives=%u\n", H(h9),
           o9.MeshShaderPipelineStatsSupported, o9.MeshShaderSupportsFullRangeRenderTargetArrayIndex,
           o9.DerivativesInMeshAndAmplificationShadersSupported);
    printf("# caps OPTIONS10 %s PerPrimitiveShadingRate=%u\n", H(h10),
           o10.MeshShaderPerPrimitiveShadingRateSupported);
    printf("# caps OPTIONS12 %s MSPrimitivesIncludesCulled=%d\n", H(h12),
           (int)o12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives);
    printf("# caps ViewInstancingTier=%u VariableShadingRateTier=%u HighestShaderModel=0x%x\n",
           o3.ViewInstancingTier, o6.VariableShadingRateTier, sm.HighestShaderModel);
}

static const PsoCase kPso[] = {
    // valid
    {NULL, "ms_quad", "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "ms_quad", "ps_color", NULL, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, 0},
    {"as_rows", "ms_payload", "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "ms_lines", NULL, NULL, 1, -1, 0, 0},
    {NULL, "ms_lines", NULL, NULL, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, 0, 0},
    {NULL, "ms_big", "ps_vcolor", NULL, 1, -1, 0, 0},
    {NULL, "ms_quad", NULL, NULL, 0, -1, 0, 0},
    // topology types
    {NULL, "ms_quad", "ps_color", NULL, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, 0, 0},
    {NULL, "ms_quad", "ps_color", NULL, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, 0, 0},
    {NULL, "ms_quad", "ps_color", NULL, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH, 0, 0},
    {NULL, "ms_lines", NULL, NULL, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0, 0},
    // refusals
    {"as_rows", NULL, "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "ms_quad", "ps_color", "vs_plain", 1, -1, 0, 0},
    {"as_rows", "ms_quad", "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "ms_payload", "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "ms_lines", "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "ms_quad", "ps_color", NULL, 1, -1, 1, 0},
    {NULL, "ms_quad", "ps_color", NULL, 1, -1, 0, 1},
    {NULL, "cs_plain", "ps_color", NULL, 1, -1, 0, 0},
    {NULL, "vs_plain", "ps_color", NULL, 1, -1, 0, 0},
    {"ms_quad", "ms_payload", "ps_color", NULL, 1, -1, 0, 0},
    {"as_rows", "as_rows", "ps_color", NULL, 1, -1, 0, 0},
};

static void pso(void) {
    if (!need_device() || !mesh_root()) return;
    for (unsigned i = 0; i < sizeof(kPso) / sizeof(kPso[0]); ++i) {
        const PsoCase* c = &kPso[i];
        ID3D12PipelineState* p = NULL;
        HRESULT hr = make_pso(c, &p);
        char item[160];
        snprintf(item, sizeof(item), "AS=%s MS=%s PS=%s VS=%s rtv=%d topology=%d il=%d so=%d",
                 c->as ? c->as : "-", c->ms ? c->ms : "-", c->ps ? c->ps : "-", c->vs ? c->vs : "-", c->rtv,
                 c->topology, c->input_layout, c->stream_output);
        L(item, "%s out=%s", H(hr), outp(p));
        release(p);
    }
}

// ---------------------------------------------------------------- drawing

static ID3D12Resource* g_rt;
static ID3D12Resource* g_counter;
static ID3D12DescriptorHeap* g_rtv_heap;

static int setup_target(void) {
    if (!need_gfx() || !mesh_root()) return 0;
    if (!g_rt) {
        D3D12_CLEAR_VALUE cv;
        memset(&cv, 0, sizeof(cv));
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RESOURCE_DESC d = tex_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1,
                                         1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_HEAP_PROPERTIES hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, &d,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                        &IID_ID3D12Resource, (void**)&g_rt)))
            return 0;
        g_rtv_heap = desc_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, FALSE);
        ID3D12Device_CreateRenderTargetView(g_dev, g_rt, NULL, cpu_at(g_rtv_heap, 0));
        g_counter = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return g_rt && g_counter;
}

static ID3D12GraphicsCommandList6* list6(void) {
    ID3D12GraphicsCommandList6* l6 = NULL;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(g_list, &IID_ID3D12GraphicsCommandList6, (void**)&l6)))
        return NULL;
    return l6;
}

// Clears the target and the counters and binds the pipeline's state.
static void prepare(ID3D12PipelineState* p, UINT tiles_x, UINT flags, UINT base) {
    const float black[4] = {0, 0, 0, 0};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_at(g_rtv_heap, 0);
    ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &rtv, FALSE, NULL);
    viewport_scissor(64, 64);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, mesh_root());
    ID3D12GraphicsCommandList_SetPipelineState(g_list, p);
    UINT k[4] = {tiles_x, flags, base, 0};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(g_list, 0, 4, k, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(
        g_list, 1, ID3D12Resource_GetGPUVirtualAddress(g_counter));
}

static void zero_counters(void) {
    UINT z[64];
    memset(z, 0, sizeof(z));
    write_buffer(g_counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, z, sizeof(z));
}

// Each tile's sample in primitive 0 (2, 3) and primitive 1 (5, 6), then the
// counters.
static void report(const char* what) {
    static UINT pix[64 * 64];
    if (!read_texture(g_rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, pix, sizeof(pix), NULL, NULL)) {
        L(what, "readback failed");
        return;
    }
    for (UINT t = 0; t < 64; ++t) {
        const UINT x = (t % 8) * 8, y = (t / 8) * 8;
        const UINT a = pix[(y + 3) * 64 + x + 2], b = pix[(y + 6) * 64 + x + 5];
        if (a == 0 && b == 0) continue;
        char item[64];
        snprintf(item, sizeof(item), "%s tile %u", what, t);
        L(item, "prim0=%08x prim1=%08x", a, b);
    }
    UINT c[4];
    read_buffer(g_counter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, c, sizeof(c));
    L(what, "MS threads=%u AS threads=%u MS groups=%u AS groups=%u", c[0], c[1], c[2], c[3]);
}

static void draw_direct(void) {
    if (!setup_target()) return;
    PsoCase pc = {NULL, "ms_quad", "ps_color", NULL, 1, -1, 0, 0};
    ID3D12PipelineState* p = NULL;
    HRESULT hr = make_pso(&pc, &p);
    if (FAILED(hr)) {
        L("pso", "%s", H(hr));
        return;
    }
    // (4, 2, 2) groups of tiles_x 4: tiles 0..7 (z 0) and 16..23 (z 1);
    // flags 1 culls primitive 1 of the odd tiles. Then (0, 1, 1): nothing.
    zero_counters();
    if (!begin()) return;
    ID3D12GraphicsCommandList6* l6 = list6();
    if (!l6) {
        L("list6", "QueryInterface failed");
        return;
    }
    prepare(p, 4, 1, 0);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 4, 2, 2);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 0, 1, 1);
    L("submit", "%s", H(end_and_wait()));
    report("direct");
    // A second dispatch on the same target: base 40, 3 x 1 x 1, no culling.
    zero_counters();
    if (!begin()) return;
    prepare(p, 4, 0, 40);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 3, 1, 1);
    L("submit", "%s", H(end_and_wait()));
    report("direct base 40");
    release(l6);
    release(p);
}

static void draw_amplified(void) {
    if (!setup_target()) return;
    PsoCase pc = {"as_rows", "ms_payload", "ps_color", NULL, 1, -1, 0, 0};
    ID3D12PipelineState* p = NULL;
    HRESULT hr = make_pso(&pc, &p);
    if (FAILED(hr)) {
        L("pso", "%s", H(hr));
        return;
    }
    zero_counters();
    if (!begin()) return;
    ID3D12GraphicsCommandList6* l6 = list6();
    prepare(p, 8, 1, 0);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 8, 1, 1);
    L("submit", "%s", H(end_and_wait()));
    report("amplified");
    release(l6);
    release(p);
}

static void draw_indirect(void) {
    if (!setup_target()) return;
    // Command signatures.
    D3D12_INDIRECT_ARGUMENT_DESC arg;
    memset(&arg, 0, sizeof(arg));
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    D3D12_COMMAND_SIGNATURE_DESC sd = {12, 1, &arg, 0};
    ID3D12CommandSignature* sig = NULL;
    HRESULT hr = ID3D12Device_CreateCommandSignature(g_dev, &sd, NULL, &IID_ID3D12CommandSignature, (void**)&sig);
    L("signature stride 12", "%s", H(hr));
    ID3D12CommandSignature* bad = NULL;
    sd.ByteStride = 8;
    L("signature stride 8", "%s", H(ID3D12Device_CreateCommandSignature(g_dev, &sd, NULL,
                                                                          &IID_ID3D12CommandSignature, (void**)&bad)));
    release(bad);
    bad = NULL;
    sd.ByteStride = 16;
    L("signature stride 16", "%s", H(ID3D12Device_CreateCommandSignature(g_dev, &sd, NULL,
                                                                           &IID_ID3D12CommandSignature, (void**)&bad)));
    release(bad);
    D3D12_INDIRECT_ARGUMENT_DESC two[2];
    memset(two, 0, sizeof(two));
    two[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    two[0].Constant.RootParameterIndex = 0;
    two[0].Constant.DestOffsetIn32BitValues = 2;
    two[0].Constant.Num32BitValuesToSet = 1;
    two[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    D3D12_COMMAND_SIGNATURE_DESC sd2 = {16, 2, two, 0};
    ID3D12CommandSignature* sig2 = NULL;
    L("signature constant+mesh", "%s",
      H(ID3D12Device_CreateCommandSignature(g_dev, &sd2, mesh_root(), &IID_ID3D12CommandSignature, (void**)&sig2)));
    if (!sig) return;
    PsoCase pc = {NULL, "ms_quad", "ps_color", NULL, 1, -1, 0, 0};
    ID3D12PipelineState* p = NULL;
    if (FAILED(make_pso(&pc, &p))) return;
    // Two commands: (4, 1, 1) at base 0, then (2, 1, 1) at base 48 through
    // the root constant; a count buffer of 1 runs only the first command.
    UINT args[8] = {4, 1, 1, 0, 2, 1, 1, 0};
    UINT args2[8] = {0, 4, 1, 1, 48, 2, 1, 1};
    UINT count[1] = {1};
    ID3D12Resource* ab = upload_data(args, sizeof(args));
    ID3D12Resource* ab2 = upload_data(args2, sizeof(args2));
    ID3D12Resource* cb = upload_data(count, sizeof(count));
    zero_counters();
    if (!begin()) return;
    prepare(p, 4, 0, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(g_list, sig, 2, ab, 0, cb, 0);
    L("submit", "%s", H(end_and_wait()));
    report("indirect count 1");
    zero_counters();
    if (!begin()) return;
    prepare(p, 4, 0, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(g_list, sig, 1, ab, 0, NULL, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(g_list, sig, 1, ab, 16, NULL, 0);
    L("submit", "%s", H(end_and_wait()));
    report("indirect two calls");
    if (sig2) {
        zero_counters();
        if (!begin()) return;
        prepare(p, 4, 0, 0);
        ID3D12GraphicsCommandList_ExecuteIndirect(g_list, sig2, 2, ab2, 0, NULL, 0);
        L("submit", "%s", H(end_and_wait()));
        report("indirect constant+mesh");
    }
    release(ab);
    release(ab2);
    release(cb);
    release(sig);
    release(sig2);
    release(p);
}

static void stats(void) {
    if (!setup_target()) return;
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, 3, 0};
    ID3D12QueryHeap* heap = NULL;
    HRESULT hr = ID3D12Device_CreateQueryHeap(g_dev, &qd, &IID_ID3D12QueryHeap, (void**)&heap);
    L("query heap PIPELINE_STATISTICS1", "%s", H(hr));
    if (FAILED(hr)) return;
    PsoCase quad = {NULL, "ms_quad", "ps_color", NULL, 1, -1, 0, 0};
    PsoCase amp = {"as_rows", "ms_payload", "ps_color", NULL, 1, -1, 0, 0};
    PsoCase lines = {NULL, "ms_lines", NULL, NULL, 1, -1, 0, 0};
    ID3D12PipelineState *pq = NULL, *pa = NULL, *pl = NULL;
    make_pso(&quad, &pq);
    make_pso(&amp, &pa);
    make_pso(&lines, &pl);
    if (!pq || !pa || !pl) {
        L("stats", "pso missing");
        return;
    }
    ID3D12Resource* rb = buffer(D3D12_HEAP_TYPE_READBACK, 3 * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1),
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!begin()) return;
    ID3D12GraphicsCommandList6* l6 = list6();
    prepare(pq, 4, 1, 0);
    ID3D12GraphicsCommandList_BeginQuery(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 4, 2, 2);
    ID3D12GraphicsCommandList_EndQuery(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    prepare(pa, 8, 1, 0);
    ID3D12GraphicsCommandList_BeginQuery(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 1);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 8, 1, 1);
    ID3D12GraphicsCommandList_EndQuery(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 1);
    prepare(pl, 4, 0, 0);
    ID3D12GraphicsCommandList_BeginQuery(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 2);
    ID3D12GraphicsCommandList6_DispatchMesh(l6, 1, 1, 1);
    ID3D12GraphicsCommandList_EndQuery(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 2);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0, 3, rb, 0);
    L("submit", "%s", H(end_and_wait()));
    D3D12_QUERY_DATA_PIPELINE_STATISTICS1* q = NULL;
    D3D12_RANGE range = {0, 3 * sizeof(*q)};
    if (FAILED(ID3D12Resource_Map(rb, 0, &range, (void**)&q))) return;
    const char* names[3] = {"quad 4x2x2 cull", "amplified 8", "lines 1"};
    for (int i = 0; i < 3; ++i) {
        L(names[i], "IA=%llu/%llu VS=%llu GS=%llu/%llu HS=%llu DS=%llu CS=%llu AS=%llu MS=%llu MSPrimitives=%llu",
          q[i].IAVertices, q[i].IAPrimitives, q[i].VSInvocations, q[i].GSInvocations, q[i].GSPrimitives,
          q[i].HSInvocations, q[i].DSInvocations, q[i].CSInvocations, q[i].ASInvocations, q[i].MSInvocations,
          q[i].MSPrimitives);
        printf("# %s %s C=%llu/%llu PS=%llu\n", g_sec, names[i], q[i].CInvocations, q[i].CPrimitives,
               q[i].PSInvocations);
    }
    ID3D12Resource_Unmap(rb, 0, NULL);
    release(l6);
    release(rb);
    release(pq);
    release(pa);
    release(pl);
    release(heap);
}

SECTIONS({"caps", caps}, {"pso", pso}, {"direct", draw_direct}, {"amplified", draw_amplified},
         {"indirect", draw_indirect}, {"stats", stats})
