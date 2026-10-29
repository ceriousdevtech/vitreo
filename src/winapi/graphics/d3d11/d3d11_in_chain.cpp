/// @file d3d11_in_chain.cpp
/// @brief D3D11 COM method in-chain classes (see d3d11_in_chain.hpp).
///
/// Sources: Microsoft Learn, the ID3D11DeviceContext / ID3D11DeviceContext1-4
/// and ID3D11DeviceChild reference pages, and the COM IUnknown contract. The
/// method names are the SDK header's (CONTEXT_METHOD_NAMES).
///
/// The invariant a class rests on (winapi::InChainClass): A returns to its
/// caller and runs no guest code; AGuestComRef may also run guest code, but
/// only through run_guest_callback. Neither promises not to block -- several
/// A methods may (the device lock, an implicit or explicit flush, a
/// synchronous pipeline compile), exactly as they do on the dispatcher path.

#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"

#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"

#include <array>

namespace vitreo::winapi::d3d11 {

namespace {

constexpr InChainClass A = InChainClass::A;
constexpr InChainClass R = InChainClass::AGuestComRef;
constexpr InChainClass N = InChainClass::None;

// Reasons, shared by the rows they describe.
constexpr std::string_view kIUnknownGet =
    "IUnknown: returns one of Vitreo's own interfaces with a host reference "
    "count; runs no guest code";
constexpr std::string_view kRelease =
    "IUnknown::Release: a final Release destroys the object, which releases "
    "every private-data interface set on it (ID3D11DeviceChild::"
    "SetPrivateDataInterface: Release is called on the interface)";
constexpr std::string_view kPrivateGet =
    "ID3D11DeviceChild::GetPrivateData: an IUnknown stored with "
    "SetPrivateDataInterface is returned AddRef'd -- a guest AddRef";
constexpr std::string_view kPrivateSet =
    "ID3D11DeviceChild::SetPrivateData/SetPrivateDataInterface: the new "
    "interface is AddRef'd and data previously set for the GUID is replaced "
    "(an interface there is Released)";
constexpr std::string_view kBind =
    "binding method: the context holds a reference to the interfaces passed "
    "in, so replacing a binding can drop the last reference to the old "
    "object, whose destruction releases its private-data interfaces";
constexpr std::string_view kReset =
    "resets or swaps bound state (Learn: ClearState unbinds everything; "
    "ExecuteCommandList/FinishCommandList reset state when told not to "
    "restore it; SwapDeviceContextState swaps it): releases bindings";
constexpr std::string_view kValueState =
    "sets value-only pipeline state (no interface held) and returns";
constexpr std::string_view kRecord =
    "records GPU work into the context's command stream and returns to the "
    "caller; it may block (the device lock, the submission of a full command "
    "buffer, a synchronous pipeline compile) and runs no guest code";
constexpr std::string_view kCopy =
    "records a copy/update/clear/resolve, reading its source from guest "
    "memory or a resource, and returns; it may block (the device lock, a "
    "staging upload) and runs no guest code";
constexpr std::string_view kGetBound =
    "returns bound Vitreo objects AddRef'd with a host reference count, or "
    "values; runs no guest code";
constexpr std::string_view kQuery =
    "Begin/End mark a query; GetData returns the data or S_FALSE, and unless "
    "D3D11_ASYNC_GETDATA_DONOTFLUSH is given it first flushes the command "
    "buffer (Learn), so it may block on that submission -- as Flush does; "
    "runs no guest code";
constexpr std::string_view kMap =
    "kept off the in-chain path by program decision (L3 review, 2026-10-12), "
    "not by its contract: Map returns to its caller and runs no guest code, "
    "though it waits for the GPU unless D3D11_MAP_FLAG_DO_NOT_WAIT is given "
    "(Learn ID3D11DeviceContext::Map)";
constexpr std::string_view kFlush =
    "Learn ID3D11DeviceContext::Flush/Flush1: sends the queued commands to "
    "the GPU and returns (Flush1's optional event is set by the host when "
    "the work completes); it may block on the submission, like GetData's "
    "implicit flush, and runs no guest code";
constexpr std::string_view kTiles =
    "tiled-resource management; not reviewed, so not classified";
constexpr std::string_view kAnnotate =
    "annotation/markers and protection state: values only, and returns";
constexpr std::string_view kFence =
    "ID3D11DeviceContext4::Signal/Wait queue a fence operation on the GPU "
    "timeline and return to the caller; runs no guest code";

constexpr std::array<InChainRule, 149> kContextRules{{
    {"QueryInterface", A, kIUnknownGet},
    {"AddRef", A, kIUnknownGet},
    {"Release", R, kRelease},
    {"GetDevice", A, kIUnknownGet},
    {"GetPrivateData", R, kPrivateGet},
    {"SetPrivateData", R, kPrivateSet},
    {"SetPrivateDataInterface", R, kPrivateSet},
    {"VSSetConstantBuffers", R, kBind},
    {"PSSetShaderResources", R, kBind},
    {"PSSetShader", R, kBind},
    {"PSSetSamplers", R, kBind},
    {"VSSetShader", R, kBind},
    {"DrawIndexed", A, kRecord},
    {"Draw", A, kRecord},
    {"Map", N, kMap},
    {"Unmap", A, kCopy},
    {"PSSetConstantBuffers", R, kBind},
    {"IASetInputLayout", R, kBind},
    {"IASetVertexBuffers", R, kBind},
    {"IASetIndexBuffer", R, kBind},
    {"DrawIndexedInstanced", A, kRecord},
    {"DrawInstanced", A, kRecord},
    {"GSSetConstantBuffers", R, kBind},
    {"GSSetShader", R, kBind},
    {"IASetPrimitiveTopology", A, kValueState},
    {"VSSetShaderResources", R, kBind},
    {"VSSetSamplers", R, kBind},
    {"Begin", A, kQuery},
    {"End", A, kQuery},
    {"GetData", A, kQuery},
    {"SetPredication", R, kBind},
    {"GSSetShaderResources", R, kBind},
    {"GSSetSamplers", R, kBind},
    {"OMSetRenderTargets", R, kBind},
    {"OMSetRenderTargetsAndUnorderedAccessViews", R, kBind},
    {"OMSetBlendState", R, kBind},
    {"OMSetDepthStencilState", R, kBind},
    {"SOSetTargets", R, kBind},
    {"DrawAuto", A, kRecord},
    {"DrawIndexedInstancedIndirect", A, kRecord},
    {"DrawInstancedIndirect", A, kRecord},
    {"Dispatch", A, kRecord},
    {"DispatchIndirect", A, kRecord},
    {"RSSetState", R, kBind},
    {"RSSetViewports", A, kValueState},
    {"RSSetScissorRects", A, kValueState},
    {"CopySubresourceRegion", A, kCopy},
    {"CopyResource", A, kCopy},
    {"UpdateSubresource", A, kCopy},
    {"CopyStructureCount", A, kCopy},
    {"ClearRenderTargetView", A, kCopy},
    {"ClearUnorderedAccessViewUint", A, kCopy},
    {"ClearUnorderedAccessViewFloat", A, kCopy},
    {"ClearDepthStencilView", A, kCopy},
    {"GenerateMips", A, kCopy},
    {"SetResourceMinLOD", A, kValueState},
    {"GetResourceMinLOD", A, kGetBound},
    {"ResolveSubresource", A, kCopy},
    {"ExecuteCommandList", R, kReset},
    {"HSSetShaderResources", R, kBind},
    {"HSSetShader", R, kBind},
    {"HSSetSamplers", R, kBind},
    {"HSSetConstantBuffers", R, kBind},
    {"DSSetShaderResources", R, kBind},
    {"DSSetShader", R, kBind},
    {"DSSetSamplers", R, kBind},
    {"DSSetConstantBuffers", R, kBind},
    {"CSSetShaderResources", R, kBind},
    {"CSSetUnorderedAccessViews", R, kBind},
    {"CSSetShader", R, kBind},
    {"CSSetSamplers", R, kBind},
    {"CSSetConstantBuffers", R, kBind},
    {"VSGetConstantBuffers", A, kGetBound},
    {"PSGetShaderResources", A, kGetBound},
    {"PSGetShader", A, kGetBound},
    {"PSGetSamplers", A, kGetBound},
    {"VSGetShader", A, kGetBound},
    {"PSGetConstantBuffers", A, kGetBound},
    {"IAGetInputLayout", A, kGetBound},
    {"IAGetVertexBuffers", A, kGetBound},
    {"IAGetIndexBuffer", A, kGetBound},
    {"GSGetConstantBuffers", A, kGetBound},
    {"GSGetShader", A, kGetBound},
    {"IAGetPrimitiveTopology", A, kGetBound},
    {"VSGetShaderResources", A, kGetBound},
    {"VSGetSamplers", A, kGetBound},
    {"GetPredication", A, kGetBound},
    {"GSGetShaderResources", A, kGetBound},
    {"GSGetSamplers", A, kGetBound},
    {"OMGetRenderTargets", A, kGetBound},
    {"OMGetRenderTargetsAndUnorderedAccessViews", A, kGetBound},
    {"OMGetBlendState", A, kGetBound},
    {"OMGetDepthStencilState", A, kGetBound},
    {"SOGetTargets", A, kGetBound},
    {"RSGetState", A, kGetBound},
    {"RSGetViewports", A, kGetBound},
    {"RSGetScissorRects", A, kGetBound},
    {"HSGetShaderResources", A, kGetBound},
    {"HSGetShader", A, kGetBound},
    {"HSGetSamplers", A, kGetBound},
    {"HSGetConstantBuffers", A, kGetBound},
    {"DSGetShaderResources", A, kGetBound},
    {"DSGetShader", A, kGetBound},
    {"DSGetSamplers", A, kGetBound},
    {"DSGetConstantBuffers", A, kGetBound},
    {"CSGetShaderResources", A, kGetBound},
    {"CSGetUnorderedAccessViews", A, kGetBound},
    {"CSGetShader", A, kGetBound},
    {"CSGetSamplers", A, kGetBound},
    {"CSGetConstantBuffers", A, kGetBound},
    {"ClearState", R, kReset},
    {"Flush", A, kFlush},
    {"GetType", A, kGetBound},
    {"GetContextFlags", A, kGetBound},
    {"FinishCommandList", R, kReset},
    {"CopySubresourceRegion1", A, kCopy},
    {"UpdateSubresource1", A, kCopy},
    {"DiscardResource", A, kCopy},
    {"DiscardView", A, kCopy},
    {"VSSetConstantBuffers1", R, kBind},
    {"HSSetConstantBuffers1", R, kBind},
    {"DSSetConstantBuffers1", R, kBind},
    {"GSSetConstantBuffers1", R, kBind},
    {"PSSetConstantBuffers1", R, kBind},
    {"CSSetConstantBuffers1", R, kBind},
    {"VSGetConstantBuffers1", A, kGetBound},
    {"HSGetConstantBuffers1", A, kGetBound},
    {"DSGetConstantBuffers1", A, kGetBound},
    {"GSGetConstantBuffers1", A, kGetBound},
    {"PSGetConstantBuffers1", A, kGetBound},
    {"CSGetConstantBuffers1", A, kGetBound},
    {"SwapDeviceContextState", R, kReset},
    {"ClearView", A, kCopy},
    {"DiscardView1", A, kCopy},
    {"UpdateTileMappings", N, kTiles},
    {"CopyTileMappings", N, kTiles},
    {"CopyTiles", N, kTiles},
    {"UpdateTiles", N, kTiles},
    {"ResizeTilePool", N, kTiles},
    {"TiledResourceBarrier", N, kTiles},
    {"IsAnnotationEnabled", A, kAnnotate},
    {"SetMarkerInt", A, kAnnotate},
    {"BeginEventInt", A, kAnnotate},
    {"EndEvent", A, kAnnotate},
    {"Flush1", A, kFlush},
    {"SetHardwareProtectionState", A, kAnnotate},
    {"GetHardwareProtectionState", A, kAnnotate},
    {"Signal", A, kFence},
    {"Wait", A, kFence},
}};

}  // namespace

const InChainRule* context_in_chain_rule(std::string_view method) noexcept {
    for (const InChainRule& rule : kContextRules) {
        if (rule.method == method) return &rule;
    }
    return nullptr;
}

InChainClass context_method_in_chain_class(usize slot) noexcept {
    if (slot >= CONTEXT_METHOD_NAMES.size()) return InChainClass::None;
    const InChainRule* rule = context_in_chain_rule(CONTEXT_METHOD_NAMES[slot]);
    return rule != nullptr ? rule->cls : InChainClass::None;
}

InChainClass device_child_in_chain_class(usize slot) noexcept {
    // Every COM interface starts with IUnknown's three methods.
    switch (slot) {
    case 0:  // QueryInterface
    case 1:  // AddRef
        return InChainClass::A;
    case 2:  // Release
        return InChainClass::AGuestComRef;
    default:
        return InChainClass::None;
    }
}

}  // namespace vitreo::winapi::d3d11
