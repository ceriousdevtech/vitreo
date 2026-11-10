// SPDX-License-Identifier: LGPL-2.1-only
//
// D3D12 PIPELINE_STATISTICS[1] beyond a06's one draw + one dispatch:
// indexed, strip, clipped, indirect and overlapping brackets, and the
// PIPELINE_STATISTICS1 record. Measured on winoracle (WARP12) and
// winoracle-gpu. Values the spec leaves to the implementation (vertex reuse,
// helper pixels) are '#' comments.
//
//   indexed      DrawIndexedInstanced (16-bit, view at byte 2) x2 instances
//   strip        TRIANGLESTRIP, 5 vertices x 3 instances (non-indexed)
//   clipped      one triangle crossing two clip planes
//   indirect     ExecuteIndirect draw (bare signature) and dispatch
//   overlap      two brackets overlapping: draws A, B, C
//   stats1       PIPELINE_STATISTICS1 heap: the 14-counter record
//
// Build: tools/oracle_probe/run_d3d12_probes.sh a10 (x64 and x86).

#include "d3d12_probe_common.h"
#include "d3d12_shaders/d3d12_shaders.h"

#define SC D3D12_RESOURCE_STATE_COMMON
#define RTS D3D12_RESOURCE_STATE_RENDER_TARGET
#define UAVS D3D12_RESOURCE_STATE_UNORDERED_ACCESS
#define CD D3D12_RESOURCE_STATE_COPY_DEST
#define IAS D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
#define RGBA8 DXGI_FORMAT_R8G8B8A8_UNORM
#define FRT D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
#define FUAV D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS

static const D3D12_INPUT_ELEMENT_DESC kPosIL[1] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

typedef struct {
    ID3D12RootSignature* rs;
    ID3D12PipelineState* pso;      // triangles
    ID3D12PipelineState* pt_pso;   // points
    ID3D12DescriptorHeap* rtvh;
    ID3D12Resource* rt;
    ID3D12Resource* vb;
} Gfx;
static Gfx gx;

// Vertex buffer (NDC xy) on an 8x8 target: 0..3 the corners of a 4x4-pixel
// quad at (0,0) (strip order), 4..7 a 2x2 quad at (6,6), 8..10 a triangle
// crossing x = +1 and y = -1, 11..15 a 5-vertex strip over the left half.
static int gfx_init(void) {
    if (!need_gfx()) return 0;
    D3D12_ROOT_PARAMETER p = rp_consts(0, 0, 8);
    gx.rs = root_sig(1, &p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    gfx_desc(&gd, gx.rs, SHADER(vs_depth), SHADER(ps_rootconst), kPosIL, 1, RGBA8);
    gx.pso = gfx_pso(&gd);
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    gx.pt_pso = gfx_pso(&gd);
    gx.rtvh = desc_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, FALSE);
    gx.rt = tex2d(RGBA8, 8, 8, 1, 1, 1, FRT, RTS, NULL);
    float v[32] = {
        -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f,                  // 4x4 quad
        0.5f, -0.5f, 1.0f, -0.5f, 0.5f, -1.0f, 1.0f, -1.0f,                // 2x2 quad
        0.0f, 0.0f, 3.0f, 0.0f, 0.0f, -3.0f,                               // clipped
        -1.0f, 1.0f, -1.0f, -1.0f, -0.5f, 1.0f, -0.5f, -1.0f, 0.0f, 1.0f,  // strip
    };
    gx.vb = upload_data(v, sizeof(v));
    if (!gx.pso || !gx.pt_pso || !gx.rtvh || !gx.rt || !gx.vb) return 0;
    ID3D12Device_CreateRenderTargetView(g_dev, gx.rt, NULL, cpu_at(gx.rtvh, 0));
    return 1;
}

static void gfx_bind(ID3D12GraphicsCommandList* l, D3D_PRIMITIVE_TOPOLOGY topology) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_at(gx.rtvh, 0);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, 8, 8, 0, 1};
    D3D12_RECT sc = {0, 0, 8, 8};
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &sc);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, gx.rs);
    ID3D12GraphicsCommandList_SetPipelineState(
        l, topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST ? gx.pt_pso : gx.pso);
    float rc[8] = {1, 0, 0, 1, 0.5f, 0, 0, 0};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, 0, 8, rc, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, topology);
    D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(gx.vb), 32 * 4, 8};
    ID3D12GraphicsCommandList_IASetVertexBuffers(l, 0, 1, &vbv);
}

static void clear_rt(void) {
    static const float kBlack[4] = {0, 0, 0, 0};
    ID3D12GraphicsCommandList_ClearRenderTargetView(g_list, cpu_at(gx.rtvh, 0), kBlack, 0, NULL);
}

static ID3D12QueryHeap* stats_heap(D3D12_QUERY_HEAP_TYPE type, UINT n) {
    D3D12_QUERY_HEAP_DESC d = {type, n, 0};
    ID3D12QueryHeap* qh = NULL;
    HRESULT hr = ID3D12Device_CreateQueryHeap(g_dev, &d, &IID_ID3D12QueryHeap, (void**)&qh);
    if (FAILED(hr)) L("heap", "CreateQueryHeap %s", H(hr));
    return qh;
}

static void report(const char* what, const D3D12_QUERY_DATA_PIPELINE_STATISTICS* s, UINT64 covered) {
    char item[160];
    snprintf(item, sizeof(item), "%s IAVertices IAPrimitives CInvocations CPrimitives", what);
    L(item, "%llu %llu %llu %llu", (unsigned long long)s->IAVertices, (unsigned long long)s->IAPrimitives,
      (unsigned long long)s->CInvocations, (unsigned long long)s->CPrimitives);
    snprintf(item, sizeof(item), "%s GS/GSPrims/HS/DS/CS invocations", what);
    L(item, "%llu %llu %llu %llu %llu", (unsigned long long)s->GSInvocations,
      (unsigned long long)s->GSPrimitives, (unsigned long long)s->HSInvocations,
      (unsigned long long)s->DSInvocations, (unsigned long long)s->CSInvocations);
    snprintf(item, sizeof(item), "%s PSInvocations >= covered pixels (%llu)", what, (unsigned long long)covered);
    L(item, "%d", s->PSInvocations >= covered);
    snprintf(item, sizeof(item), "%s VSInvocations nonzero", what);
    L(item, "%d", s->VSInvocations != 0);
    snprintf(item, sizeof(item), "%s PSInvocations VSInvocations", what);
    C(item, "%llu %llu", (unsigned long long)s->PSInvocations, (unsigned long long)s->VSInvocations);
}

// One bracket around fn's draws, resolved and reported.
static void one_bracket(const char* what, D3D_PRIMITIVE_TOPOLOGY topology, UINT64 covered,
                        void (*draw)(void)) {
    ID3D12QueryHeap* qh = stats_heap(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1);
    ID3D12Resource* out = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, CD);
    if (!qh || !out || !begin()) return;
    clear_rt();
    gfx_bind(g_list, topology);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    draw();
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, out, 0);
    L("record+execute", "%s", H(end_and_wait()));
    D3D12_QUERY_DATA_PIPELINE_STATISTICS s;
    memset(&s, 0, sizeof(s));
    read_buffer(out, CD, 0, &s, sizeof(s));
    report(what, &s, covered);
    release(qh);
    release(out);
}

// ---------------------------------------------------------------- indexed

static ID3D12Resource* g_ib;
static void draw_indexed(void) {
    // The view starts at byte 2: its index 0 is idx[1].
    D3D12_INDEX_BUFFER_VIEW ibv = {ID3D12Resource_GetGPUVirtualAddress(g_ib) + 2, 24, DXGI_FORMAT_R16_UINT};
    ID3D12GraphicsCommandList_IASetIndexBuffer(g_list, &ibv);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(g_list, 12, 2, 0, 0, 0);
}
static void s_indexed(void) {
    if (!gfx_init()) return;
    UINT16 idx[13] = {0xEEEE, 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    g_ib = upload_data(idx, sizeof(idx));
    if (!g_ib) return;
    one_bracket("indexed 12 x2", D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 20, draw_indexed);
    release(g_ib);
}

// ---------------------------------------------------------------- strip

static void draw_strip(void) { ID3D12GraphicsCommandList_DrawInstanced(g_list, 5, 3, 11, 0); }
static void s_strip(void) {
    if (!gfx_init()) return;
    one_bracket("strip 5 x3", D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, 16, draw_strip);
}

// ---------------------------------------------------------------- clipped

static void draw_clipped(void) { ID3D12GraphicsCommandList_DrawInstanced(g_list, 3, 1, 8, 0); }
static void s_clipped(void) {
    if (!gfx_init()) return;
    one_bracket("clipped triangle", D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 16, draw_clipped);
}

// ---------------------------------------------------------------- indirect

static void s_indirect(void) {
    if (!gfx_init()) return;
    ID3D12QueryHeap* qh = stats_heap(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2);
    ID3D12Resource* out = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, CD);
    D3D12_DESCRIPTOR_RANGE r = range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0);
    D3D12_ROOT_PARAMETER p[2] = {rp_consts(0, 0, 4), rp_table(1, &r)};
    ID3D12RootSignature* crs = root_sig(2, p, 0, NULL, 0);
    ID3D12PipelineState* cpso = compute_pso(crs, SHADER(cs_count));
    ID3D12DescriptorHeap* heap = desc_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, TRUE);
    ID3D12Resource* cnt = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, FUAV, UAVS);
    D3D12_INDIRECT_ARGUMENT_DESC a;
    memset(&a, 0, sizeof(a));
    a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC sd = {16, 1, &a, 0};
    ID3D12CommandSignature* dsig = NULL;
    ID3D12Device_CreateCommandSignature(g_dev, &sd, NULL, &IID_ID3D12CommandSignature, (void**)&dsig);
    D3D12_INDIRECT_ARGUMENT_DESC b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC cd = {12, 1, &b, 0};
    ID3D12CommandSignature* csig = NULL;
    ID3D12Device_CreateCommandSignature(g_dev, &cd, NULL, &IID_ID3D12CommandSignature, (void**)&csig);
    // DRAW {6 vertices, 2 instances, from vertex 8 (the clipped triangle and
    // the first strip triangle as a list), instance 0}; DISPATCH {3, 2, 1}.
    UINT32 words[8] = {6, 2, 8, 0, 3, 2, 1, 0};
    ID3D12Resource* args = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, SC);
    if (!qh || !out || !cpso || !heap || !cnt || !dsig || !csig || !args) return;
    write_buffer(args, SC, 0, words, sizeof(words));
    D3D12_UNORDERED_ACCESS_VIEW_DESC u;
    memset(&u, 0, sizeof(u));
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = 64;
    u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(g_dev, cnt, NULL, &u, cpu_at(heap, 0));
    if (!begin()) return;
    transition(args, 0, SC, IAS);
    clear_rt();
    gfx_bind(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(g_list, dsig, 1, args, 0, NULL, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
    ID3D12GraphicsCommandList_SetPipelineState(g_list, cpso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(g_list, crs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(g_list, 0, 0, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(g_list, 1, gpu_at(heap, 0));
    ID3D12GraphicsCommandList_ExecuteIndirect(g_list, csig, 1, args, 16, NULL, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, out, 0);
    L("record+execute", "%s", H(end_and_wait()));
    D3D12_QUERY_DATA_PIPELINE_STATISTICS s[2];
    memset(s, 0, sizeof(s));
    read_buffer(out, CD, 0, s, sizeof(s));
    report("indirect draw 6 x2", &s[0], 0);
    report("indirect dispatch (3,2,1)", &s[1], 0);
    release(qh); release(out); release(crs); release(cpso); release(heap); release(cnt);
    release(dsig); release(csig); release(args);
}

// ---------------------------------------------------------------- overlap

static void s_overlap(void) {
    if (!gfx_init()) return;
    ID3D12QueryHeap* qh = stats_heap(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2);
    ID3D12Resource* out = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, CD);
    if (!qh || !out || !begin()) return;
    clear_rt();
    gfx_bind(g_list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 1, 1, 0, 0);   // A: 1 point
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 2, 1, 4, 0);   // B: 2 points
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 4, 1, 11, 0);  // C: 4 points
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 2, out, 0);
    L("record+execute", "%s", H(end_and_wait()));
    D3D12_QUERY_DATA_PIPELINE_STATISTICS s[2];
    memset(s, 0, sizeof(s));
    read_buffer(out, CD, 0, s, sizeof(s));
    report("query 0 (A+B)", &s[0], 0);
    report("query 1 (B+C)", &s[1], 0);
    release(qh); release(out);
}

// ---------------------------------------------------------------- stats1

static void s_stats1(void) {
    if (!gfx_init()) return;
    ID3D12QueryHeap* qh = stats_heap(D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1, 1);
    ID3D12Resource* out = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, CD);
    if (!qh || !out || !begin()) return;
    clear_rt();
    gfx_bind(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 4, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS1, 0, 1, out, 0);
    L("record+execute", "%s", H(end_and_wait()));
    D3D12_QUERY_DATA_PIPELINE_STATISTICS1 s;
    memset(&s, 0xCD, sizeof(s));
    read_buffer(out, CD, 0, &s, sizeof(s));
    report("stats1 strip 4", (const D3D12_QUERY_DATA_PIPELINE_STATISTICS*)&s, 16);
    L("stats1 AS/MS invocations MSPrimitives", "%llu %llu %llu", (unsigned long long)s.ASInvocations,
      (unsigned long long)s.MSInvocations, (unsigned long long)s.MSPrimitives);
    // A PIPELINE_STATISTICS query type on a STATISTICS1 heap.
    if (!begin()) return;
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    L("PIPELINE_STATISTICS type on a STATISTICS1 heap: Close", "%s", H(ID3D12GraphicsCommandList_Close(g_list)));
    release(qh); release(out);
}

// ---------------------------------------------------------------- so

// Vertex-shader stream output of COLOR (16 bytes a vertex) into a buffer
// with room for one triangle, from two triangles: SO_STATISTICS_STREAM0
// counts 1 written and 2 needed; STREAM1 nothing. A second bracket with
// no target bound writes nothing and still needs 2.
static void s_so(void) {
    if (!need_gfx()) return;
    static const D3D12_INPUT_ELEMENT_DESC il[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    ID3D12RootSignature* rs =
        root_sig(0, NULL, 0, NULL,
                 D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd;
    gfx_desc(&gd, rs, SHADER(vs_color), SHADER(ps_color), il, 2, RGBA8);
    D3D12_SO_DECLARATION_ENTRY so = {0, "COLOR", 0, 0, 4, 0};
    UINT stride = 16;
    gd.StreamOutput.NumEntries = 1;
    gd.StreamOutput.pSODeclaration = &so;
    gd.StreamOutput.pBufferStrides = &stride;
    gd.StreamOutput.NumStrides = 1;
    gd.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    gd.PS.pShaderBytecode = NULL;
    gd.PS.BytecodeLength = 0;
    ID3D12PipelineState* pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g_dev, &gd, &IID_ID3D12PipelineState, (void**)&pso);
    L("CreateGraphicsPipelineState(VS stream output)", "%s", H(hr));
    ID3D12QueryHeap* qh = stats_heap(D3D12_QUERY_HEAP_TYPE_SO_STATISTICS, 4);
    ID3D12Resource* out = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, CD);
    // 48 data bytes (one triangle) at 0, the filled-size word at 64.
    ID3D12Resource* sob = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, 0, SC);
    float v[36];
    for (int i = 0; i < 6; ++i) {
        float* p = v + i * 6;
        p[0] = (float)(i & 1); p[1] = (float)(i >> 1); p[2] = (float)i; p[3] = 1; p[4] = 2; p[5] = 3;
    }
    ID3D12Resource* vb = upload_data(v, sizeof(v));
    if (!pso || !qh || !out || !sob || !vb) return;
    UINT32 zero[64] = {0};
    write_buffer(sob, SC, 0, zero, sizeof(zero));
    if (!begin()) return;
    transition(sob, 0, SC, D3D12_RESOURCE_STATE_STREAM_OUT);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, rs);
    ID3D12GraphicsCommandList_SetPipelineState(g_list, pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = {ID3D12Resource_GetGPUVirtualAddress(vb), sizeof(v), 24};
    ID3D12GraphicsCommandList_IASetVertexBuffers(g_list, 0, 1, &vbv);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW sv = {ID3D12Resource_GetGPUVirtualAddress(sob), 48,
                                          ID3D12Resource_GetGPUVirtualAddress(sob) + 64};
    ID3D12GraphicsCommandList_SOSetTargets(g_list, 0, 1, &sv);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 6, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1);
    ID3D12GraphicsCommandList_SOSetTargets(g_list, 0, 0, NULL);
    ID3D12GraphicsCommandList_BeginQuery(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 2);
    ID3D12GraphicsCommandList_DrawInstanced(g_list, 6, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 2);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 0, 1, out, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1, 1, 1, out, 16);
    ID3D12GraphicsCommandList_ResolveQueryData(g_list, qh, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0, 2, 1, out, 32);
    transition(sob, 0, D3D12_RESOURCE_STATE_STREAM_OUT, SC);
    L("record+execute", "%s", H(end_and_wait()));
    UINT64 s[6] = {0};
    read_buffer(out, CD, 0, s, sizeof(s));
    L("STREAM0 written needed", "%llu %llu", (unsigned long long)s[0], (unsigned long long)s[1]);
    L("STREAM1 written needed", "%llu %llu", (unsigned long long)s[2], (unsigned long long)s[3]);
    L("STREAM0 no target written needed", "%llu %llu", (unsigned long long)s[4], (unsigned long long)s[5]);
    UINT32 words[17] = {0};
    read_buffer(sob, SC, 0, words, sizeof(words));
    L("BufferFilledSize", "%u", words[16]);
    L("first vertex COLOR", "%s", hex32(words, 4));
    release(rs); release(pso); release(qh); release(out); release(sob); release(vb);
}

SECTIONS({"indexed", s_indexed}, {"strip", s_strip}, {"clipped", s_clipped}, {"indirect", s_indirect},
         {"overlap", s_overlap}, {"stats1", s_stats1}, {"so", s_so})
