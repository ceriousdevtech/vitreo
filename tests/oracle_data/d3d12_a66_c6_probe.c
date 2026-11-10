/*
 * d3d12_a66_c6_probe.c - D3D12 cleanup slice C6 (the last shader-feature
 * items): runs d3d12_c6/d3d12_c6.hlsl on the default adapter (winoracle:
 * WARP; Vitreo: Metal). Sections (one per process, "--list" names them):
 *   caps   BarycentricsSupported, ViewInstancingTier, ConservativeRasterizationTier
 *          ('#' lines: adapter-specific).
 *   bary   vs_c6 / ms_c6 (+ gs_c6_lines) with ps_c6 on an 8x8 target:
 *          conservative ("bary cr"), target-independent 4x UAV-only
 *          ("bary tir"), mesh ("bary ms"), vertex-shader stream output
 *          ("bary so", then "bary so buffer": ID and ATTR of 6 vertices and
 *          the filled size) and geometry-shader lines ("bary gs lines");
 *          2048 words: ((x + 8 y) * 4 + slot) * 8.
 *   lp     lines (vs_lines, 5 lines) and points (vs_points, 6) on a 16x16
 *          UAV-only target with ForcedSampleCount 4 / 1 and with conservative
 *          rasterization, through ps_cov / ps_icov: 1024 words, (x + 16 y) * 4;
 *          "lines bary tir4" (ps_lines_bary, 4 lines): 4096 words.
 *   many   vs_many + ps_many (v1..v31) conservative and TIR 4x: 512 words.
 *   crnops TIR / conservative lines and points with no pixel shader (C10).
 *   adj    adjacency topologies with no geometry shader (C10).
 *   mix    a mesh pixel input row fed by vertex and primitive outputs (C10).
 *   vi     view instancing, 3 views at location (0, 0), UAV-only: the
 *          per-stage counters (first 32 words), and "vi stats" the
 *          PIPELINE_STATISTICS of two draws.
 * Each block is "## <name>" then 8 words a line (hex); a PSO that fails
 * prints "pso <name>: 0x...".
 *
 * Never calls any shutdown or power API.
 *
 * Build: x86_64-w64-mingw32-gcc -O1 -static -o d3d12_a66_c6_probe.exe \
 *        d3d12_a66_c6_probe.c -I. -luuid -lole32
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>

#include "d3d12_c6/d3d12_c6_shaders.h"

#define CHECK(expr)                                                        \
    do {                                                                   \
        HRESULT hr_ = (expr);                                              \
        if (FAILED(hr_)) {                                                 \
            printf("FAILED %s: 0x%08lx\n", #expr, (unsigned long)hr_);   \
            fflush(stdout);                                                \
            return 1;                                                      \
        }                                                                  \
    } while (0)

enum { kWords = 4096, kUpload = 1 << 16, kSoBytes = 4096 };

typedef HRESULT(WINAPI* PFN_CREATE)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT(WINAPI* PFN_SERIALIZE)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                       ID3DBlob**);

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static ID3D12CommandAllocator* g_alloc;
static ID3D12GraphicsCommandList* g_list;
static ID3D12RootSignature* g_root;
static ID3D12DescriptorHeap* g_heap;
static ID3D12DescriptorHeap* g_rtvs;
static ID3D12Resource *g_out, *g_upload, *g_readback, *g_rt8, *g_so, *g_so_rb, *g_query_rb;
static ID3D12QueryHeap* g_queries;

static D3D12_SHADER_BYTECODE code(const char* name) {
    D3D12_SHADER_BYTECODE bc = {NULL, 0};
    unsigned k;
    if (name == NULL) return bc;
    for (k = 0; k < sizeof(kC6Blobs) / sizeof(kC6Blobs[0]); ++k) {
        if (strcmp(kC6Blobs[k].name, name) == 0) {
            bc.pShaderBytecode = kC6Blobs[k].code;
            bc.BytecodeLength = kC6Blobs[k].size;
        }
    }
    return bc;
}

static ID3D12Resource* make(D3D12_HEAP_TYPE heap, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp;
    ID3D12Resource* r = NULL;
    memset(&hp, 0, sizeof(hp));
    hp.Type = heap;
    if (FAILED(ID3D12Device_CreateCommittedResource(g_dev, &hp, D3D12_HEAP_FLAG_NONE, rd, state, NULL,
                                                    &IID_ID3D12Resource, (void**)&r))) {
        return NULL;
    }
    return r;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return rd;
}

static void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(g_list, 1, &b);
}

/* ------------------------------------------------------------ pipelines */
typedef struct {
    unsigned char bytes[2048];
    size_t used;
} Stream;

static void put_sub(Stream* s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const void* data, size_t size,
                    size_t align) {
    const size_t p = sizeof(void*);
    size_t at;
    s->used = (s->used + p - 1) & ~(p - 1);
    memcpy(s->bytes + s->used, &type, sizeof(type));
    at = s->used + sizeof(type);
    at = (at + align - 1) & ~(align - 1);
    memcpy(s->bytes + at, data, size);
    s->used = at + size;
}

typedef struct {
    const char* vs;
    const char* gs;
    const char* ps;
    const char* ms;
    const char* as;
    const char* hs;
    const char* ds;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
    UINT render_targets;  /* 0: UAV-only */
    UINT forced;          /* ForcedSampleCount */
    BOOL conservative;
    BOOL multisample_lines; /* MultisampleEnable: quadrilateral lines */
    BOOL stream_output;     /* ID + ATTR of stream 0, rasterized stream 0 */
    UINT views;             /* view instances at location (0, 0) */
    BOOL depth;             /* DSVFormat D32_FLOAT, DepthEnable (C10 crnops) */
} Spec;

static ID3D12PipelineState* pso(const Spec* c, const char* name) {
    ID3D12Device2* dev2 = NULL;
    ID3D12PipelineState* out = NULL;
    Stream s;
    D3D12_SHADER_BYTECODE bc;
    D3D12_RASTERIZER_DESC r;
    D3D12_DEPTH_STENCIL_DESC1 ds;
    struct D3D12_RT_FORMAT_ARRAY rt;
    D3D12_PIPELINE_STATE_STREAM_DESC sd;
    D3D12_SO_DECLARATION_ENTRY so[2] = {{0, "ID", 0, 0, 1, 0}, {0, "ATTR", 0, 0, 4, 0}};
    UINT stride = 20;
    D3D12_VIEW_INSTANCE_LOCATION loc[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    DXGI_SAMPLE_DESC sample = {1, 0};
    UINT mask = 0xFFFFFFFFu;
    HRESULT hr;
    memset(&s, 0, sizeof(s));
    if (FAILED(ID3D12Device_QueryInterface(g_dev, &IID_ID3D12Device2, (void**)&dev2))) return NULL;
    put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &g_root, sizeof(g_root), sizeof(void*));
    if (c->as) { bc = code(c->as); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->ms) { bc = code(c->ms); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->vs) { bc = code(c->vs); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->hs) { bc = code(c->hs); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->ds) { bc = code(c->ds); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->gs) { bc = code(c->gs); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, &bc, sizeof(bc), sizeof(void*)); }
    if (c->ps) { bc = code(c->ps); put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &bc, sizeof(bc), sizeof(void*)); }
    memset(&rt, 0, sizeof(rt));
    /* UAV-only: one target of format UNKNOWN, none bound (the pixel shaders
     * write SV_Target0, which NumRenderTargets 0 refuses: d3d12_a05). */
    rt.NumRenderTargets = 1;
    rt.RTFormats[0] = c->render_targets ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
    put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rt, sizeof(rt), 4);
    if (!c->ms) {
        put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &c->topology, sizeof(c->topology), 4);
    }
    memset(&r, 0, sizeof(r));
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;
    r.DepthClipEnable = TRUE;
    r.MultisampleEnable = c->multisample_lines;
    r.ForcedSampleCount = c->forced;
    r.ConservativeRaster = c->conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                           : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &r, sizeof(r), 4);
    memset(&ds, 0, sizeof(ds));
    ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    if (c->depth) {
        DXGI_FORMAT dsv = DXGI_FORMAT_D32_FLOAT;
        ds.DepthEnable = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &dsv, sizeof(dsv), 4);
    }
    put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds, sizeof(ds), 4);
    put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &sample, sizeof(sample), 4);
    put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &mask, sizeof(mask), 4);
    if (c->stream_output) {
        D3D12_STREAM_OUTPUT_DESC d = {so, 2, &stride, 1, 0};
        put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, &d, sizeof(d), sizeof(void*));
    }
    if (c->views) {
        D3D12_VIEW_INSTANCING_DESC vi = {c->views, loc, D3D12_VIEW_INSTANCING_FLAG_NONE};
        put_sub(&s, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, &vi, sizeof(vi), sizeof(void*));
    }
    s.used = (s.used + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    sd.SizeInBytes = s.used;
    sd.pPipelineStateSubobjectStream = s.bytes;
    hr = ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState, (void**)&out);
    ID3D12Device2_Release(dev2);
    if (FAILED(hr)) {
        printf("pso %s: 0x%08lx\n", name, (unsigned long)hr);
        fflush(stdout);
        return NULL;
    }
    return out;
}

/* ------------------------------------------------------------ command lists */
/* Reset the list, zero u1 (and the stream-output buffer), bind the table. */
static int begin(ID3D12PipelineState* state, UINT size) {
    D3D12_GPU_DESCRIPTOR_HANDLE table;
    D3D12_VIEWPORT vp;
    D3D12_RECT sc;
    CHECK(ID3D12CommandAllocator_Reset(g_alloc));
    CHECK(ID3D12GraphicsCommandList_Reset(g_list, g_alloc, state));
    barrier(g_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(g_list, g_out, 0, g_upload, 0, kWords * 4);
    barrier(g_out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(g_so, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(g_list, g_so, 0, g_upload, 0, kSoBytes);
    barrier(g_so, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_list, 1, &g_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_list, g_root);
    g_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g_heap, &table);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_list, 0, table);
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (float)size;
    vp.Height = (float)size;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    sc.left = 0;
    sc.top = 0;
    sc.right = (LONG)size;
    sc.bottom = (LONG)size;
    ID3D12GraphicsCommandList_RSSetViewports(g_list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(g_list, 1, &sc);
    return 0;
}

static void bind_rt8(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    g_rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_rtvs, &rtv);
    ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 1, &rtv, FALSE, NULL);
}

static void print_words(const char* name, const UINT32* w, UINT words) {
    UINT k;
    printf("## %s\n", name);
    for (k = 0; k < words; ++k) printf("%08x%s", w[k], (k % 8) == 7 ? "\n" : " ");
    fflush(stdout);
}

/* Copy u1 (and the SO buffer, the queries) out, run, wait, print. */
static int finish(const char* name, UINT words, int so, UINT queries) {
    ID3D12Fence* fence = NULL;
    HANDLE ev;
    const UINT32* w = NULL;
    D3D12_RANGE none = {0, 0};
    barrier(g_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyResource(g_list, g_readback, g_out);
    barrier(g_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(g_so, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyResource(g_list, g_so_rb, g_so);
    barrier(g_so, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);
    if (queries) {
        ID3D12GraphicsCommandList_ResolveQueryData(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0,
                                                   queries, g_query_rb, 0);
    }
    CHECK(ID3D12GraphicsCommandList_Close(g_list));
    ID3D12CommandQueue_ExecuteCommandLists(g_queue, 1, (ID3D12CommandList**)&g_list);
    CHECK(ID3D12Device_CreateFence(g_dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence));
    CHECK(ID3D12CommandQueue_Signal(g_queue, fence, 1));
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, 1, ev));
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
        printf("## %s\nTIMEOUT\n", name);
        return 1;
    }
    CloseHandle(ev);
    ID3D12Fence_Release(fence);
    CHECK(ID3D12Resource_Map(g_readback, 0, NULL, (void**)&w));
    print_words(name, w, words);
    ID3D12Resource_Unmap(g_readback, 0, &none);
    if (so) {
        char so_name[96];
        UINT32 block[32];
        CHECK(ID3D12Resource_Map(g_so_rb, 0, NULL, (void**)&w));
        memcpy(block, w, 30 * 4);
        block[30] = w[(kSoBytes - 16) / 4];
        block[31] = 0;
        ID3D12Resource_Unmap(g_so_rb, 0, &none);
        snprintf(so_name, sizeof(so_name), "%s buffer", name);
        print_words(so_name, block, 32);
    }
    if (queries) {
        static const char* kNames[11] = {"IAVertices", "IAPrimitives", "VSInvocations", "GSInvocations",
                                         "GSPrimitives", "CInvocations", "CPrimitives", "PSInvocations",
                                         "HSInvocations", "DSInvocations", "CSInvocations"};
        const UINT64* q = NULL;
        UINT i, k;
        CHECK(ID3D12Resource_Map(g_query_rb, 0, NULL, (void**)&q));
        for (i = 0; i < queries; ++i) {
            for (k = 0; k < 8; ++k) {
                printf("%s stats %u %s: %llu\n", name, i, kNames[k], (unsigned long long)q[i * 11 + k]);
            }
        }
        ID3D12Resource_Unmap(g_query_rb, 0, &none);
        fflush(stdout);
    }
    return 0;
}

static ID3D12GraphicsCommandList6* list6(void) {
    ID3D12GraphicsCommandList6* l6 = NULL;
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(g_list, &IID_ID3D12GraphicsCommandList6, (void**)&l6))) {
        return NULL;
    }
    return l6;
}

/* ------------------------------------------------------------ sections */
static int run_draw(const Spec* c, const char* name, UINT size, D3D_PRIMITIVE_TOPOLOGY topology, UINT vertices,
                    UINT words) {
    ID3D12PipelineState* state = pso(c, name);
    if (state == NULL) return 0;
    if (begin(state, size)) return 1;
    if (c->render_targets) {
        bind_rt8();
    } else {
        ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 0, NULL, FALSE, NULL);
    }
    if (c->stream_output) {
        D3D12_STREAM_OUTPUT_BUFFER_VIEW v;
        v.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(g_so);
        v.SizeInBytes = kSoBytes - 256;
        v.BufferFilledSizeLocation = v.BufferLocation + kSoBytes - 16;
        ID3D12GraphicsCommandList_SOSetTargets(g_list, 0, 1, &v);
    }
    if (c->ms) {
        ID3D12GraphicsCommandList6* l6 = list6();
        if (l6 == NULL) {
            printf("%s: no ID3D12GraphicsCommandList6\n", name);
            return 0;
        }
        ID3D12GraphicsCommandList6_DispatchMesh(l6, 1, 1, 1);
        ID3D12GraphicsCommandList6_Release(l6);
    } else {
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, topology);
        ID3D12GraphicsCommandList_DrawInstanced(g_list, vertices, 1, 0, 0);
    }
    if (finish(name, words, c->stream_output, 0)) return 1;
    ID3D12PipelineState_Release(state);
    return 0;
}

static int section_bary(void) {
    Spec c;
    int fail = 0;
    memset(&c, 0, sizeof(c));
    c.vs = "vs_c6";
    c.ps = "ps_c6";
    c.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    c.render_targets = 1;
    c.conservative = TRUE;
    fail |= run_draw(&c, "bary cr", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, 2048);
    c.conservative = FALSE;
    c.render_targets = 0;
    c.forced = 4;
    fail |= run_draw(&c, "bary tir", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, 2048);
    c.forced = 0;
    c.render_targets = 1;
    c.stream_output = TRUE;
    fail |= run_draw(&c, "bary so", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, 2048);
    c.stream_output = FALSE;
    c.gs = "gs_c6_lines";
    fail |= run_draw(&c, "bary gs lines", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, 2048);
    /* Geometry-shader lines under TIR (aliased rule) and conservative
     * rasterization (undefined for lines: informative). */
    c.render_targets = 0;
    c.forced = 4;
    fail |= run_draw(&c, "bary tir gs lines", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, 2048);
    c.forced = 0;
    c.render_targets = 1;
    c.conservative = TRUE;
    fail |= run_draw(&c, "bary cr gs lines", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 6, 2048);
    c.conservative = FALSE;
    c.gs = NULL;
    c.vs = NULL;
    c.ms = "ms_c6";
    fail |= run_draw(&c, "bary ms", 8, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED, 0, 2048);
    c.ms = NULL;
    c.vs = "vs_c6";
    c.hs = "hs_c6";
    c.ds = "ds_c6";
    c.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    fail |= run_draw(&c, "bary ds", 8, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, 6, 2048);
    c.render_targets = 0;
    c.forced = 4;
    fail |= run_draw(&c, "bary tir ds", 8, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST, 6, 2048);
    c.render_targets = 1;
    c.forced = 0;
    c.hs = "hs_c6_iso";
    c.ds = "ds_c6_iso";
    fail |= run_draw(&c, "bary ds isolines", 8, D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST, 6, 2048);
    c.conservative = TRUE;
    fail |= run_draw(&c, "bary cr ds isolines", 8, D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST, 6, 2048);
    c.conservative = FALSE;
    c.render_targets = 0;
    c.forced = 4;
    fail |= run_draw(&c, "bary tir ds isolines", 8, D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST, 6, 2048);
    c.forced = 0;
    c.render_targets = 1;
    return fail;
}

static int section_lp(void) {
    static const struct {
        const char* name;
        const char* vs;
        const char* ps;
        int points;
        UINT forced;
        BOOL cr;
        BOOL quad;
    } kCases[] = {
        {"lines tir4", "vs_lines", "ps_cov", 0, 4, FALSE, FALSE},
        {"lines tir4 quad", "vs_lines", "ps_cov", 0, 4, FALSE, TRUE},
        {"lines tir1", "vs_lines", "ps_cov", 0, 1, FALSE, FALSE},
        {"lines tir8 quad", "vs_lines", "ps_cov", 0, 8, FALSE, TRUE},
        {"lines cr", "vs_lines", "ps_icov", 0, 0, TRUE, FALSE},
        {"lines cr cov", "vs_lines", "ps_cov", 0, 0, TRUE, FALSE},
        {"lines cr quad", "vs_lines", "ps_icov", 0, 0, TRUE, TRUE},
        {"points tir4", "vs_points", "ps_cov", 1, 4, FALSE, FALSE},
        {"points tir1", "vs_points", "ps_cov", 1, 1, FALSE, FALSE},
        {"points cr", "vs_points", "ps_icov", 1, 0, TRUE, FALSE},
        {"points cr cov", "vs_points", "ps_cov", 1, 0, TRUE, FALSE},
    };
    Spec c;
    unsigned k;
    int fail = 0;
    for (k = 0; k < sizeof(kCases) / sizeof(kCases[0]); ++k) {
        memset(&c, 0, sizeof(c));
        c.vs = kCases[k].vs;
        c.ps = kCases[k].ps;
        c.topology = kCases[k].points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        c.forced = kCases[k].forced;
        c.conservative = kCases[k].cr;
        c.multisample_lines = kCases[k].quad;
        fail |= run_draw(&c, kCases[k].name, 16,
                         kCases[k].points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                         kCases[k].points ? 6 : 10, 1024);
    }
    memset(&c, 0, sizeof(c));
    c.vs = "vs_lines";
    c.ps = "ps_lines_bary";
    c.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    c.forced = 4;
    fail |= run_draw(&c, "lines bary tir4", 16, D3D_PRIMITIVE_TOPOLOGY_LINELIST, 8, 4096);
    c.forced = 0;
    fail |= run_draw(&c, "lines bary", 16, D3D_PRIMITIVE_TOPOLOGY_LINELIST, 8, 4096);
    return fail;
}

static int section_many(void) {
    Spec c;
    int fail = 0;
    memset(&c, 0, sizeof(c));
    c.vs = "vs_many";
    c.ps = "ps_many";
    c.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    c.render_targets = 1;
    c.conservative = TRUE;
    fail |= run_draw(&c, "many cr", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 512);
    c.conservative = FALSE;
    c.render_targets = 0;
    c.forced = 4;
    fail |= run_draw(&c, "many tir4", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 512);
    c.forced = 0;
    c.render_targets = 1;
    fail |= run_draw(&c, "many plain", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 512);
    /* v1..v30: within Metal's 124 fragment input components with the edge
     * data in a buffer. */
    c.ps = "ps_many30";
    c.conservative = TRUE;
    fail |= run_draw(&c, "many30 cr", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 512);
    c.conservative = FALSE;
    c.render_targets = 0;
    c.forced = 4;
    fail |= run_draw(&c, "many30 tir4", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 512);
    c.forced = 0;
    c.render_targets = 1;
    fail |= run_draw(&c, "many30 plain", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 512);
    return fail;
}

static int section_vi(void) {
    static const struct {
        const char* name;
        const char* vs;
        const char* gs;
        const char* ps;
        const char* ms;
        const char* as;
        const char* ds;
    } kCases[] = {
        {"vi vs ps-id", "vs_vi", NULL, "ps_vi_id", NULL, NULL, NULL},
        {"vi vs-id ps", "vs_vi_id", NULL, "ps_vi", NULL, NULL, NULL},
        {"vi vs gs-id ps", "vs_vi", "gs_vi_id", "ps_vi", NULL, NULL, NULL},
        {"vi vs gs ps-id", "vs_vi", "gs_vi", "ps_vi_id", NULL, NULL, NULL},
        {"vi vs-id gs ps", "vs_vi_id", "gs_vi", "ps_vi", NULL, NULL, NULL},
        {"vi ms ps-id", NULL, NULL, "ps_vi_id", "ms_vi", NULL, NULL},
        {"vi ms-id ps", NULL, NULL, "ps_vi", "ms_vi_id", NULL, NULL},
        {"vi as ms ps-id", NULL, NULL, "ps_vi_id", "ms_vi_ap", "as_vi", NULL},
        {"vi as ms-id ps", NULL, NULL, "ps_vi", "ms_vi_id_ap", "as_vi", NULL},
        {"vi vs hs ds-id ps", "vs_vi", NULL, "ps_vi", NULL, NULL, "ds_vi_id"},
        {"vi vs hs ds ps-id", "vs_vi", NULL, "ps_vi_id", NULL, NULL, "ds_vi"},
        {"vi vs hs ds gs-id ps", "vs_vi", "gs_vi_id", "ps_vi", NULL, NULL, "ds_vi"},
    };
    Spec c;
    unsigned k;
    int fail = 0;
    for (k = 0; k < sizeof(kCases) / sizeof(kCases[0]); ++k) {
        memset(&c, 0, sizeof(c));
        c.vs = kCases[k].vs;
        c.gs = kCases[k].gs;
        c.ps = kCases[k].ps;
        c.ms = kCases[k].ms;
        c.as = kCases[k].as;
        c.ds = kCases[k].ds;
        c.hs = c.ds ? "hs_vi" : NULL;
        c.topology = c.ds ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        c.views = 3;
        fail |= run_draw(&c, kCases[k].name, 8,
                         c.ds ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                         3, 32);
    }
    return fail;
}

static int section_vi_stats(void) {
    static const struct {
        const char* vs;
        const char* gs;
        const char* ps;
    } kDraws[2] = {{"vs_vi", NULL, "ps_vi_id"}, {"vs_vi", "gs_vi_id", "ps_vi"}};
    ID3D12PipelineState* states[2];
    Spec c;
    UINT i;
    for (i = 0; i < 2; ++i) {
        memset(&c, 0, sizeof(c));
        c.vs = kDraws[i].vs;
        c.gs = kDraws[i].gs;
        c.ps = kDraws[i].ps;
        c.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        c.views = 3;
        states[i] = pso(&c, i == 0 ? "vi stats vs ps-id" : "vi stats vs gs-id ps");
        if (states[i] == NULL) return 0;
    }
    if (begin(states[0], 8)) return 1;
    ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 0, NULL, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (i = 0; i < 2; ++i) {
        ID3D12GraphicsCommandList_SetPipelineState(g_list, states[i]);
        ID3D12GraphicsCommandList_BeginQuery(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i);
        ID3D12GraphicsCommandList_DrawInstanced(g_list, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, i);
    }
    return finish("vi stats", 32, 0, 2);
}

/* C10: target-independent / conservative rasterization of lines and points
 * with no pixel shader (the PSO result, then one draw's statistics). */
static int section_crnops(void) {
    static const struct {
        const char* name;
        BOOL points;
        UINT forced;
        BOOL cr;
        BOOL depth;
    } kCases[] = {
        {"crnops cr lines", FALSE, 0, TRUE, FALSE},   {"crnops cr points", TRUE, 0, TRUE, FALSE},
        {"crnops tir4 lines", FALSE, 4, FALSE, FALSE}, {"crnops tir4 points", TRUE, 4, FALSE, FALSE},
        {"crnops tir1 lines", FALSE, 1, FALSE, FALSE}, {"crnops tir16 points", TRUE, 16, FALSE, FALSE},
        {"crnops cr lines depth", FALSE, 0, TRUE, TRUE}, {"crnops cr points depth", TRUE, 0, TRUE, TRUE},
        {"crnops cr lines ps", FALSE, 0, TRUE, FALSE},
    };
    Spec c;
    unsigned k;
    for (k = 0; k < sizeof(kCases) / sizeof(kCases[0]); ++k) {
        ID3D12PipelineState* state;
        memset(&c, 0, sizeof(c));
        c.vs = kCases[k].points ? "vs_points" : "vs_lines";
        c.topology = kCases[k].points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        c.forced = kCases[k].forced;
        c.conservative = kCases[k].cr;
        c.depth = kCases[k].depth;
        if (strstr(kCases[k].name, " ps") != NULL) c.ps = "ps_cov";
        state = pso(&c, kCases[k].name);
        if (state == NULL) continue;
        printf("pso %s: 0x00000000\n", kCases[k].name);
        if (c.depth || c.ps) {
            /* The pipeline result only (no depth view is bound here). */
            ID3D12PipelineState_Release(state);
            continue;
        }
        if (begin(state, 16)) return 1;
        ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 0, NULL, FALSE, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(
            g_list, kCases[k].points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        ID3D12GraphicsCommandList_BeginQuery(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        ID3D12GraphicsCommandList_DrawInstanced(g_list, kCases[k].points ? 6 : 10, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        if (finish(kCases[k].name, 8, 0, 1)) return 1;
        ID3D12PipelineState_Release(state);
    }
    return 0;
}

/* C10: adjacency topologies with no geometry shader (vs_adj + ps_adj, 12
 * vertices, 16x16 UAV-only), with each draw's statistics. */
static int section_adj(void) {
    static const struct {
        const char* name;
        D3D_PRIMITIVE_TOPOLOGY topology;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    } kCases[] = {
        {"adj trilist", D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
        {"adj tristrip", D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE},
        {"adj linelist", D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
        {"adj linestrip", D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE},
    };
    Spec c;
    unsigned k;
    for (k = 0; k < sizeof(kCases) / sizeof(kCases[0]); ++k) {
        ID3D12PipelineState* state;
        memset(&c, 0, sizeof(c));
        c.vs = "vs_adj";
        c.ps = "ps_adj";
        c.topology = kCases[k].type;
        state = pso(&c, kCases[k].name);
        if (state == NULL) continue;
        if (begin(state, 16)) return 1;
        ID3D12GraphicsCommandList_OMSetRenderTargets(g_list, 0, NULL, FALSE, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_list, kCases[k].topology);
        ID3D12GraphicsCommandList_BeginQuery(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        ID3D12GraphicsCommandList_DrawInstanced(g_list, 12, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(g_list, g_queries, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
        if (finish(kCases[k].name, 1024, 0, 1)) return 1;
        ID3D12PipelineState_Release(state);
    }
    return 0;
}

/* C10: ms_mix + ps_mix, 8x8 UAV-only: 256 words. */
static int section_mix(void) {
    Spec c;
    memset(&c, 0, sizeof(c));
    c.ms = "ms_mix";
    c.ps = "ps_mix";
    c.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    return run_draw(&c, "mix", 8, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, 3, 256);
}

int main(int argc, char** argv) {
    static const char* kSections[] = {"caps", "bary", "lp", "many", "vi", "vistats", "crnops", "adj", "mix"};
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    PFN_CREATE create = (PFN_CREATE)GetProcAddress(d3d12, "D3D12CreateDevice");
    PFN_SERIALIZE serialize = (PFN_SERIALIZE)GetProcAddress(d3d12, "D3D12SerializeRootSignature");
    D3D12_COMMAND_QUEUE_DESC qd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    D3D12_DESCRIPTOR_RANGE range;
    D3D12_ROOT_PARAMETER param;
    D3D12_ROOT_SIGNATURE_DESC rs;
    D3D12_RESOURCE_DESC rd;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv;
    D3D12_QUERY_HEAP_DESC qh;
    ID3DBlob *sig = NULL, *err = NULL;
    unsigned char* up = NULL;
    const char* section;
    int k;
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (k = 0; k < (int)(sizeof(kSections) / sizeof(kSections[0])); ++k) printf("%s\n", kSections[k]);
        return 0;
    }
    section = argc > 1 ? argv[1] : "caps";
    if (!create || !serialize) {
        printf("no d3d12\n");
        return 1;
    }
    CHECK(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&g_dev));
    if (strcmp(section, "caps") == 0) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS o;
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3;
        memset(&o, 0, sizeof(o));
        memset(&o3, 0, sizeof(o3));
        CHECK(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)));
        CHECK(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3)));
        printf("caps OPTIONS3: S_OK\n");
        printf("# caps BarycentricsSupported: %d\n", o3.BarycentricsSupported);
        printf("# caps ViewInstancingTier: %d\n", (int)o3.ViewInstancingTier);
        printf("# caps ConservativeRasterizationTier: %d\n", (int)o.ConservativeRasterizationTier);
        return 0;
    }
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(ID3D12Device_CreateCommandQueue(g_dev, &qd, &IID_ID3D12CommandQueue, (void**)&g_queue));
    CHECK(ID3D12Device_CreateCommandAllocator(g_dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                              (void**)&g_alloc));
    CHECK(ID3D12Device_CreateCommandList(g_dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void**)&g_list));
    CHECK(ID3D12GraphicsCommandList_Close(g_list));

    /* Root signature: one table with the raw UAV u1, every stage. */
    memset(&range, 0, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 1;
    memset(&param, 0, sizeof(param));
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    memset(&rs, 0, sizeof(rs));
    rs.NumParameters = 1;
    rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    if (FAILED(serialize(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        printf("FAILED: root signature\n");
        return 1;
    }
    CHECK(ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(sig),
                                           ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature,
                                           (void**)&g_root));

    rd = buffer_desc(kWords * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_out = make(D3D12_HEAP_TYPE_DEFAULT, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rd = buffer_desc(kUpload, D3D12_RESOURCE_FLAG_NONE);
    g_upload = make(D3D12_HEAP_TYPE_UPLOAD, &rd, D3D12_RESOURCE_STATE_GENERIC_READ);
    rd = buffer_desc(kWords * 4, D3D12_RESOURCE_FLAG_NONE);
    g_readback = make(D3D12_HEAP_TYPE_READBACK, &rd, D3D12_RESOURCE_STATE_COPY_DEST);
    rd = buffer_desc(kSoBytes, D3D12_RESOURCE_FLAG_NONE);
    g_so = make(D3D12_HEAP_TYPE_DEFAULT, &rd, D3D12_RESOURCE_STATE_STREAM_OUT);
    g_so_rb = make(D3D12_HEAP_TYPE_READBACK, &rd, D3D12_RESOURCE_STATE_COPY_DEST);
    rd = buffer_desc(2 * 11 * 8, D3D12_RESOURCE_FLAG_NONE);
    g_query_rb = make(D3D12_HEAP_TYPE_READBACK, &rd, D3D12_RESOURCE_STATE_COPY_DEST);
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 8;
    rd.Height = 8;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    g_rt8 = make(D3D12_HEAP_TYPE_DEFAULT, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!g_out || !g_upload || !g_readback || !g_so || !g_so_rb || !g_query_rb || !g_rt8) {
        printf("FAILED: resource creation\n");
        return 1;
    }
    CHECK(ID3D12Resource_Map(g_upload, 0, NULL, (void**)&up));
    memset(up, 0, kUpload);
    ID3D12Resource_Unmap(g_upload, 0, NULL);
    memset(&qh, 0, sizeof(qh));
    qh.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    qh.Count = 2;
    CHECK(ID3D12Device_CreateQueryHeap(g_dev, &qh, &IID_ID3D12QueryHeap, (void**)&g_queries));

    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(ID3D12Device_CreateDescriptorHeap(g_dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g_heap));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(ID3D12Device_CreateDescriptorHeap(g_dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&g_rtvs));
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        g_rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_rtvs, &h);
        ID3D12Device_CreateRenderTargetView(g_dev, g_rt8, NULL, h);
        memset(&uv, 0, sizeof(uv));
        uv.Format = DXGI_FORMAT_R32_TYPELESS;
        uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = kWords;
        uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        g_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_heap, &h);
        ID3D12Device_CreateUnorderedAccessView(g_dev, g_out, NULL, &uv, h);
    }
    if (strcmp(section, "bary") == 0) return section_bary();
    if (strcmp(section, "lp") == 0) return section_lp();
    if (strcmp(section, "many") == 0) return section_many();
    if (strcmp(section, "vi") == 0) return section_vi();
    if (strcmp(section, "vistats") == 0) return section_vi_stats();
    if (strcmp(section, "crnops") == 0) return section_crnops();
    if (strcmp(section, "adj") == 0) return section_adj();
    if (strcmp(section, "mix") == 0) return section_mix();
    printf("unknown section %s\n", section);
    return 1;
}
