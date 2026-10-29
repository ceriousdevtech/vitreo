// SPDX-License-Identifier: LGPL-2.1-only
//
// L3: the in-chain contract class of each D3D11 COM method
// (winapi/graphics/d3d11/d3d11_in_chain.hpp). Expected classes come from the
// methods' Microsoft Learn contracts, keyed by the SDK header's method names
// (CONTEXT_METHOD_NAMES, generated from d3d11_3.h and audited by
// tools/audit_d3d11_vtables.py) -- not from what Vitreo does in a run:
//   * class A returns to its caller and runs no guest code (it may block:
//     GetData flushes unless D3D11_ASYNC_GETDATA_DONOTFLUSH, so Flush and
//     Flush1 are A with it);
//   * the binding methods hold a reference to the interfaces passed in, so
//     replacing a binding can destroy the old object, and destruction
//     releases its private-data interfaces (SetPrivateDataInterface);
//   * draws, copies, clears and value-only state record and return;
//   * Map stays None by program decision, the tiled-resource methods because
//     they are unreviewed.

#include <gtest/gtest.h>

#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"

#include <set>
#include <string_view>

using namespace vitreo;
using namespace vitreo::winapi;

TEST(D3D11InChainClassTest, EveryContextMethodHasExactlyOneRule) {
    // No slot may fall through to a default: a method no rule names would
    // silently be None today and stay unreviewed when the table grows.
    std::set<std::string_view> seen;
    for (usize slot = 0; slot < d3d11::CONTEXT_METHOD_NAMES.size(); ++slot) {
        const std::string_view name = d3d11::CONTEXT_METHOD_NAMES[slot];
        const d3d11::InChainRule* rule = d3d11::context_in_chain_rule(name);
        ASSERT_NE(rule, nullptr) << "slot " << slot << " " << name;
        EXPECT_FALSE(rule->reason.empty()) << name;
        EXPECT_TRUE(seen.insert(name).second) << name << " named twice";
        EXPECT_EQ(d3d11::context_method_in_chain_class(slot), rule->cls)
            << name;
    }
    EXPECT_EQ(seen.size(), d3d11::CONTEXT_METHOD_COUNT);
    EXPECT_EQ(d3d11::context_method_in_chain_class(d3d11::CONTEXT_METHOD_COUNT),
              InChainClass::None);
    EXPECT_EQ(d3d11::context_in_chain_rule("NotAMethod"), nullptr);
}

TEST(D3D11InChainClassTest, MapAndTheUnreviewedTiledMethodsNeverResumeInChain) {
    for (const char* name :
         {"Map", "UpdateTileMappings", "CopyTileMappings", "CopyTiles",
          "UpdateTiles", "ResizeTilePool", "TiledResourceBarrier"}) {
        const d3d11::InChainRule* rule = d3d11::context_in_chain_rule(name);
        ASSERT_NE(rule, nullptr) << name;
        EXPECT_EQ(rule->cls, InChainClass::None) << name;
        EXPECT_FALSE(in_chain_eligible(rule->cls)) << name;
    }
    EXPECT_FALSE(in_chain_eligible(InChainClass::None));
}

TEST(D3D11InChainClassTest, MethodsThatCanCallAGuestInterfaceAreMarked) {
    for (const char* name :
         {"Release", "GetPrivateData", "SetPrivateData",
          "SetPrivateDataInterface", "PSSetShaderResources",
          "IASetVertexBuffers", "IASetIndexBuffer", "IASetInputLayout",
          "VSSetShader", "PSSetShader", "VSSetConstantBuffers",
          "PSSetSamplers", "OMSetRenderTargets",
          "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState",
          "OMSetDepthStencilState", "RSSetState", "SOSetTargets",
          "SetPredication", "CSSetUnorderedAccessViews",
          "PSSetConstantBuffers1", "ClearState", "ExecuteCommandList",
          "FinishCommandList", "SwapDeviceContextState"}) {
        const d3d11::InChainRule* rule = d3d11::context_in_chain_rule(name);
        ASSERT_NE(rule, nullptr) << name;
        EXPECT_EQ(rule->cls, InChainClass::AGuestComRef) << name;
        EXPECT_TRUE(in_chain_eligible(rule->cls)) << name;
    }
}

TEST(D3D11InChainClassTest, RecordsCopiesValuesAndGettersAreClassA) {
    for (const char* name :
         {"QueryInterface", "AddRef", "GetDevice", "DrawIndexed", "Draw",
          "DrawIndexedInstanced", "DrawInstanced", "Dispatch",
          "IASetPrimitiveTopology", "RSSetViewports", "RSSetScissorRects",
          "Unmap", "Begin", "End", "GetData", "UpdateSubresource",
          "CopyResource", "CopySubresourceRegion", "ClearRenderTargetView",
          "ClearDepthStencilView", "PSGetShaderResources", "OMGetBlendState",
          "IAGetPrimitiveTopology", "GetType", "GetContextFlags",
          "SetMarkerInt", "BeginEventInt", "EndEvent", "Signal", "Wait",
          "Flush", "Flush1"}) {
        const d3d11::InChainRule* rule = d3d11::context_in_chain_rule(name);
        ASSERT_NE(rule, nullptr) << name;
        EXPECT_EQ(rule->cls, InChainClass::A) << name;
    }
}

TEST(D3D11InChainClassTest, ClassCountsMatchTheContractInventory) {
    // .docs/contracts/d3d11.md, "In-chain contract classes": 93 / 49 / 7.
    usize a = 0, r = 0, none = 0;
    for (usize slot = 0; slot < d3d11::CONTEXT_METHOD_NAMES.size(); ++slot) {
        switch (d3d11::context_method_in_chain_class(slot)) {
        case InChainClass::A: ++a; break;
        case InChainClass::AGuestComRef: ++r; break;
        case InChainClass::None: ++none; break;
        }
    }
    EXPECT_EQ(a, 93u);
    EXPECT_EQ(r, 49u);
    EXPECT_EQ(none, 7u);
}

TEST(D3D11InChainClassTest, DeviceChildrenClassifyOnlyTheirIUnknownSlots) {
    EXPECT_EQ(d3d11::device_child_in_chain_class(0), InChainClass::A);
    EXPECT_EQ(d3d11::device_child_in_chain_class(1), InChainClass::A);
    EXPECT_EQ(d3d11::device_child_in_chain_class(2),
              InChainClass::AGuestComRef);
    for (usize slot = 3; slot < 80; ++slot) {
        EXPECT_EQ(d3d11::device_child_in_chain_class(slot), InChainClass::None)
            << slot;
    }
}
