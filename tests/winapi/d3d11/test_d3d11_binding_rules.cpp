// SPDX-License-Identifier: LGPL-2.1-only
//
// Device-context binding rules: output-merger UAV model, hazards, IA slots, constant-buffer windows.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_binding_rules.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

using OmArray = std::array<GuestAddr, 8>;

TEST_F(D3D11BindingRulesTest, OMSetRenderTargetsUnbindsEveryOutputMergerUav) {
    std::array<GuestAddr, 2> tex{texture2d(16, 1, 1, RGBA8, COLOUR),
                                 texture2d(16, 1, 1, RGBA8, COLOUR)};
    const GuestAddr uav1 = uav(tex[0]);
    const GuestAddr multi = buffer(256, BIND_VB | BIND_IB | BIND_SRV | BIND_UAV);
    const GuestAddr uavB = uav(multi, {R32_UINT, 1 /*BUFFER*/, 0, 64, 0});
    const GuestAddr rtvO = rtv(tex[1]);
    const GuestAddr depth = texture2d(16, 1, 1, R24G8_TYPELESS, BIND_DS | BIND_SRV);
    const GuestAddr dsv0 = dsv(depth, 0);
    // a05 "uavs at 1-2 / 0-1 / 3-4, then OMSetRenderTargets(...)": every OM UAV
    // reads back NULL, whatever slots it was in.
    for (const u32 start : {1u, 0u, 3u}) {
        ctx(110, {});
        om(0, {}, 0, start, 2, {uav1, uavB});
        ASSERT_EQ(om_get().uav[start], uav1);
        set_rts({rtvO}, 0);
        const OmState after = om_get();
        EXPECT_EQ(after.rtv[0], rtvO);
        EXPECT_EQ(after.uav, OmArray{}) << "start " << start;
    }
    ctx(110, {});
    om(0, {}, 0, 3, 2, {uav1, uavB});
    set_rts({}, dsv0);
    EXPECT_EQ(om_get().dsv, dsv0);
    EXPECT_EQ(om_get().uav, OmArray{});
}

TEST_F(D3D11BindingRulesTest, OutputMergerUavCallsFollowLearnsThreeCases) {
    std::array<GuestAddr, 4> t{}, r{}, u{};
    for (usize i = 0; i < 4; ++i) {
        t[i] = texture2d(16, 1, 1, RGBA8, COLOUR);
        r[i] = rtv(t[i]);
        u[i] = uav(t[i]);
    }
    const GuestAddr tO = texture2d(16, 1, 1, RGBA8, COLOUR);
    const GuestAddr rtvO = rtv(tO);
    const GuestAddr uavO = uav(tO);
    const GuestAddr multi = buffer(256, BIND_SRV | BIND_UAV);
    const GuestAddr uavB = uav(multi, {R32_UINT, 1, 0, 64, 0});
    const GuestAddr other = buffer(256, BIND_SRV | BIND_UAV);
    const GuestAddr uavC = uav(other, {R32_UINT, 1, 0, 64, 0});
    const GuestAddr depth = texture2d(16, 1, 1, R24G8_TYPELESS, BIND_DS | BIND_SRV);
    const GuestAddr dsv0 = dsv(depth, 0);

    ctx(110, {});
    om(1, {r[0]}, dsv0, 1, 2, {u[1], uavB});
    OmState s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[0]}));
    EXPECT_EQ(s.dsv, dsv0);
    EXPECT_EQ(s.uav, (OmArray{0, u[1], uavB}));

    // "rtv1/uav1 conflict": the whole call is dropped.
    om(4, {r[0], r[1], r[2], r[3]}, 0, 4, 2, {u[1], uavB});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[0]}));
    EXPECT_EQ(s.dsv, dsv0);
    EXPECT_EQ(s.uav, (OmArray{0, u[1], uavB}));

    // A new UAV range replaces every OM UAV slot; the DSV goes with NULL.
    om(4, {r[0], r[1], r[2], r[3]}, 0, 4, 2, {uavB, uavC});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[0], r[1], r[2], r[3]}));
    EXPECT_EQ(s.dsv, 0u);
    EXPECT_EQ(s.uav, (OmArray{0, 0, 0, 0, uavB, uavC}));

    // KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL: RTVs from UAVStartSlot on are
    // unbound, pDepthStencilView is ignored, UAVs outside the range cleared.
    om(KEEP, {}, dsv0, 2, 1, {uavO});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[0], r[1]}));
    EXPECT_EQ(s.dsv, 0u);
    EXPECT_EQ(s.uav, (OmArray{0, 0, uavO}));

    // KEEP_UNORDERED_ACCESS_VIEWS: UAVs in slots < NumRTVs and UAVs of a new
    // RTV's subresource are unbound; UAVStartSlot is ignored.
    ctx(110, {});
    om(0, {}, 0, 0, 4, {u[0], u[1], u[2], u[3]});
    EXPECT_EQ(om_get().uav, (OmArray{u[0], u[1], u[2], u[3]}));
    om(2, {r[2], r[3]}, 0, 0, KEEP, {});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[2], r[3]}));
    EXPECT_EQ(s.uav, OmArray{});
    om(1, {rtvO}, 0, 5, KEEP, {});
    EXPECT_EQ(om_get().rtv, (OmArray{rtvO}));

    // UAVStartSlot < NumRTVs: the RTVs from UAVStartSlot on are not bound, and
    // such an RTV is discarded before the conflict check (a05b).
    ctx(110, {});
    om(1, {rtvO}, 0, 1, 1, {uavB});
    om(2, {r[0], r[1]}, 0, 1, 1, {u[2]});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[0]}));
    EXPECT_EQ(s.uav, (OmArray{0, u[2]}));
    om(2, {r[0], r[1]}, 0, 1, 1, {u[1]});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[0]}));
    EXPECT_EQ(s.uav, (OmArray{0, u[1]}));
    // ... but with NumUAVs 0 every RTV binds (a05b "(1,{rtv16b},start 0,
    // num 0)"), and with one UAV at u0 the RTV at 0 is dropped.
    om(1, {r[3]}, 0, 0, 0, {});
    EXPECT_EQ(om_get().rtv, (OmArray{r[3]}));
    om(1, {r[3]}, 0, 0, 1, {u[0]});
    s = om_get();
    EXPECT_EQ(s.rtv, OmArray{});
    EXPECT_EQ(s.uav, (OmArray{u[0]}));

    // Conflicts drop the call: RTV and UAV of one subresource, one UAV twice.
    om(1, {rtvO}, 0, 1, 1, {uavB});
    om(1, {r[0]}, 0, 1, 8, {u[0], u[1], u[2], u[3], 0, 0, 0, 0});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{rtvO}));
    EXPECT_EQ(s.uav, (OmArray{0, uavB}));
    om(1, {r[0]}, 0, 1, 2, {u[2], u[2]});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{rtvO}));
    EXPECT_EQ(s.uav, (OmArray{0, uavB}));
    // Start + num past eight is not itself a reason to drop: the slots past
    // u7 are ignored (a05b "start 1,num 8 ... no conflict").
    om(1, {r[1]}, 0, 1, 8, {u[0], 0, 0, 0, 0, 0, 0, 0});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[1]}));
    EXPECT_EQ(s.uav, (OmArray{0, u[0]}));
    om(0, {}, 0, 7, 2, {u[2], u[3]});
    EXPECT_EQ(om_get().uav, (OmArray{0, 0, 0, 0, 0, 0, 0, u[2]}));

    // Start 8 or 9 with no UAVs, and (0,NULL,NULL,0,0): everything unbound.
    for (const u32 start : {8u, 9u, 0u}) {
        om(1, {rtvO}, 0, 1, 1, {uavB});
        om(0, {}, 0, start, 0, {});
        s = om_get();
        EXPECT_EQ(s.rtv, OmArray{}) << start;
        EXPECT_EQ(s.uav, OmArray{}) << start;
    }

    // KEEP_RTV with a UAV of a bound RTV's subresource unbinds that RTV; a
    // KEEP_RTV call with one UAV twice is dropped (a05b).
    om(1, {r[0]}, 0, 1, 1, {uavB});
    om(KEEP, {}, 0, 1, 1, {u[0]});
    s = om_get();
    EXPECT_EQ(s.rtv, OmArray{});
    EXPECT_EQ(s.uav, (OmArray{0, u[0]}));
    om(1, {r[1]}, 0, 1, 1, {uavB});
    om(KEEP, {}, 0, 1, 2, {u[2], u[2]});
    s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{r[1]}));
    EXPECT_EQ(s.uav, (OmArray{0, uavB}));
}

TEST_F(D3D11BindingRulesTest, OutputMergerAndComputeWritersDisplaceEachOther) {
    std::array<GuestAddr, 4> t{}, u{};
    for (usize i = 0; i < 4; ++i) {
        t[i] = texture2d(16, 1, 1, RGBA8, COLOUR);
        u[i] = uav(t[i]);
    }
    const GuestAddr rtv3 = rtv(t[3]);
    const GuestAddr u0b = uav(t[0]);  // a second view of t0
    const auto cs = [&](u32 n) { return get_array(106, 0, n); };

    // a05 "om uav2 bound after cs uav2" and the reverse.
    ctx(110, {});
    set_cs_uavs(0, {u[2]});
    om(0, {}, 0, 0, 1, {u[2]});
    EXPECT_EQ(om_get().uav[0], u[2]);
    EXPECT_EQ(cs(1)[0], 0u);
    set_cs_uavs(1, {u[2]});
    EXPECT_EQ(cs(2), (std::vector<GuestAddr>{0, u[2]}));
    EXPECT_EQ(om_get().uav[0], 0u);

    // a05 "rtv3 bound after cs uav3" and "cs uav3 bound while rtv3 bound".
    ctx(110, {});
    set_cs_uavs(0, {u[3]});
    set_rts({rtv3}, 0);
    EXPECT_EQ(om_get().rtv[0], rtv3);
    EXPECT_EQ(cs(1)[0], 0u);
    set_cs_uavs(1, {u[3]});
    EXPECT_EQ(cs(2), (std::vector<GuestAddr>{0, u[3]}));
    EXPECT_EQ(om_get().rtv[0], 0u);

    // a05b: a CS UAV unbinds another CS slot of the same subresource ...
    ctx(110, {});
    set_cs_uavs(0, {u[0]});
    set_cs_uavs(1, {u0b});
    EXPECT_EQ(cs(2), (std::vector<GuestAddr>{0, u0b}));
    // ... but one call naming a subresource twice is dropped whole.
    set_cs_uavs(0, {u[1], u[2]});
    set_cs_uavs(0, {u[0], u[0]});
    EXPECT_EQ(cs(2), (std::vector<GuestAddr>{u[1], u[2]}));
    set_cs_uavs(0, {u[0], u0b});
    EXPECT_EQ(cs(2), (std::vector<GuestAddr>{u[1], u[2]}));
    set_cs_uavs(1, {u[0], u[0]});
    EXPECT_EQ(cs(3), (std::vector<GuestAddr>{u[1], u[2], 0}));
}

TEST_F(D3D11BindingRulesTest, OutputBindsUnbindAndRefuseShaderResourcesOnEveryStage) {
    std::array<GuestAddr, 4> t{}, s{}, r{}, u{};
    for (usize i = 0; i < 4; ++i) {
        t[i] = texture2d(16, 1, 1, RGBA8, COLOUR);
        s[i] = srv(t[i]);
        r[i] = rtv(t[i]);
        u[i] = uav(t[i]);
    }
    const GuestAddr srvO = srv(texture2d(16, 1, 1, RGBA8, COLOUR));
    const auto check = [&](const char* label, GuestAddr output_srv,
                           const std::function<void()>& bind_output) {
        ctx(110, {});
        set_srv_all_stages(0, {output_srv, srvO});
        bind_output();
        // a05 hazard rows a/c/e: slot 0 unbound on all six stages, slot 1 kept.
        EXPECT_EQ(srv_on_all_stages(0), all6(0)) << label;
        EXPECT_EQ(srv_on_all_stages(1), all6(srvO)) << label;
        // Rows b/d/f: binding it now is refused on all six stages.
        set_srv_all_stages(2, {output_srv});
        EXPECT_EQ(srv_on_all_stages(2), all6(0)) << label;
    };
    check("rtv", s[0], [&] { set_rts({r[0]}, 0); });
    check("cs uav", s[1], [&] { set_cs_uavs(0, {u[1]}); });
    check("om uav", s[2], [&] { om(0, {}, 0, 0, 1, {u[2]}); });
    // Row p: once the output is unbound the SRV binds again.
    set_rts({}, 0);
    ctx(110, {});
    set_srv_all_stages(0, {s[0]});
    EXPECT_EQ(srv_on_all_stages(0), all6(s[0]));

    // Depth-stencil planes (rows g/h).
    const GuestAddr depth =
        texture2d(16, 1, 1, R24G8_TYPELESS, BIND_DS | BIND_SRV);
    const GuestAddr srvDepth = srv(depth, {R24_UNORM_X8, 4, 0, 1, 0, 0});
    const GuestAddr srvStencil = srv(depth, {X24_G8_UINT, 4, 0, 1, 0, 0});
    const GuestAddr dsvW = dsv(depth, 0);
    const GuestAddr dsvRO = dsv(depth, d3d11::DSV_READ_ONLY_DEPTH);
    const GuestAddr dsvROS = dsv(depth, d3d11::DSV_READ_ONLY_DEPTH |
                                            d3d11::DSV_READ_ONLY_STENCIL);
    struct Case { GuestAddr view; bool depth_kept; bool stencil_kept; };
    for (const Case& c : {Case{dsvRO, true, false}, Case{dsvROS, true, true},
                          Case{dsvW, false, false}}) {
        ctx(110, {});
        set_srv_all_stages(0, {srvDepth, srvStencil});
        set_rts({}, c.view);
        EXPECT_EQ(srv_on_all_stages(0), all6(c.depth_kept ? srvDepth : 0));
        EXPECT_EQ(srv_on_all_stages(1), all6(c.stencil_kept ? srvStencil : 0));
        // Binding while the DSV is bound follows the same planes -- on CS too.
        set_srv_all_stages(2, {srvDepth, srvStencil});
        EXPECT_EQ(srv_on_all_stages(2), all6(c.depth_kept ? srvDepth : 0));
        EXPECT_EQ(srv_on_all_stages(3), all6(c.stencil_kept ? srvStencil : 0));
    }
}

TEST_F(D3D11BindingRulesTest, HazardsAreTrackedPerSubresource) {
    const auto ps = [&](u32 n) { return get_array(73, 0, n); };
    // Mips (rows i/j): only the mip written conflicts.
    const GuestAddr tMip = texture2d(16, 2, 1, RGBA8, COLOUR);
    const GuestAddr srvMip0 = srv(tMip, {RGBA8, 4, 0, 1});
    const GuestAddr srvMipAll = srv(tMip);
    const GuestAddr srvMip1 = srv(tMip, {RGBA8, 4, 1, 1});
    const GuestAddr rtvMip1 = rtv(tMip, {RGBA8, 4, 1});
    const GuestAddr uavMip1 = uav(tMip, {RGBA8, 4, 1});
    for (const bool compute : {false, true}) {
        ctx(110, {});
        set_srvs(8, 0, {srvMip0, srvMipAll, srvMip1});
        if (compute) set_cs_uavs(0, {uavMip1});
        else set_rts({rtvMip1}, 0);
        EXPECT_EQ(ps(3), (std::vector<GuestAddr>{srvMip0, 0, 0})) << compute;
        set_srvs(8, 0, {srvMip0, srvMipAll, srvMip1});
        EXPECT_EQ(ps(3), (std::vector<GuestAddr>{srvMip0, 0, 0})) << compute;
    }
    // Array slices (row k), for an RTV and -- the old defect -- a UAV.
    const GuestAddr tArr = texture2d(16, 1, 2, RGBA8, COLOUR);
    const GuestAddr srvSl0 = srv(tArr, {RGBA8, 5, 0, 1, 0, 1});
    const GuestAddr srvSlBoth = srv(tArr);
    const GuestAddr srvSl1 = srv(tArr, {RGBA8, 5, 0, 1, 1, 1});
    const GuestAddr rtvSl1 = rtv(tArr, {RGBA8, 5, 0, 1, 1});
    const GuestAddr uavSl1 = uav(tArr, {RGBA8, 5, 0, 1, 1});
    for (const bool compute : {false, true}) {
        ctx(110, {});
        set_srvs(8, 0, {srvSl0, srvSlBoth, srvSl1});
        if (compute) set_cs_uavs(0, {uavSl1});
        else set_rts({rtvSl1}, 0);
        EXPECT_EQ(ps(3), (std::vector<GuestAddr>{srvSl0, 0, 0})) << compute;
        set_srvs(8, 0, {srvSl0, srvSlBoth, srvSl1});
        EXPECT_EQ(ps(3), (std::vector<GuestAddr>{srvSl0, 0, 0})) << compute;
    }
    // A volume's W slices are not subresources (row l).
    const GuestAddr t3d = texture3d(16, 4, RGBA8, COLOUR);
    const GuestAddr srv3d = srv(t3d);
    const GuestAddr rtv3dW2 = rtv(t3d, {RGBA8, 8, 0, 2, 1});
    const GuestAddr uav3dW2 = uav(t3d, {RGBA8, 8, 0, 2, 1});
    for (const bool compute : {false, true}) {
        ctx(110, {});
        set_srvs(8, 0, {srv3d});
        if (compute) set_cs_uavs(0, {uav3dW2});
        else set_rts({rtv3dW2}, 0);
        EXPECT_EQ(ps(1)[0], 0u) << compute;
    }
}

TEST_F(D3D11BindingRulesTest, BufferOutputsUnbindAndRefuseVertexIndexAndShaderInputs) {
    const GuestAddr multi = buffer(256, BIND_VB | BIND_IB | BIND_SRV | BIND_UAV);
    const GuestAddr srvB = srv(multi, {R32_UINT, 1, 0, 64});
    const GuestAddr uavB = uav(multi, {R32_UINT, 1, 0, 64, 0});
    // Rows m: a CS or OM UAV of the buffer unbinds its VB (stride/offset
    // zeroed), IB (format/offset zeroed) and SRVs on every stage; later binds
    // are refused but record their stride/offset/format.
    for (const bool compute : {true, false}) {
        ctx(110, {});
        vb_set(0, {multi}, {4}, {8});
        ctx(19, {multi, R32_UINT, 4});
        set_srv_all_stages(0, {srvB});
        if (compute) set_cs_uavs(0, {uavB});
        else om(0, {}, 0, 0, 1, {uavB});
        VbState vb = vb_get(0, 1);
        EXPECT_EQ(vb.buffers[0], 0u) << compute;
        EXPECT_EQ(vb.strides[0], 0u) << compute;
        EXPECT_EQ(vb.offsets[0], 0u) << compute;
        IbState ib = ib_get();
        EXPECT_EQ(ib.buffer, 0u) << compute;
        EXPECT_EQ(ib.format, 0u) << compute;
        EXPECT_EQ(ib.offset, 0u) << compute;
        EXPECT_EQ(srv_on_all_stages(0), all6(0)) << compute;
        vb_set(1, {multi}, {4}, {0});
        vb = vb_get(1, 1);
        EXPECT_EQ(vb.buffers[0], 0u) << compute;
        EXPECT_EQ(vb.strides[0], 4u) << compute;
        ctx(19, {multi, R16_UINT, 2});
        ib = ib_get();
        EXPECT_EQ(ib.buffer, 0u) << compute;
        EXPECT_EQ(ib.format, R16_UINT) << compute;
        EXPECT_EQ(ib.offset, 2u) << compute;
        set_srv_all_stages(1, {srvB});
        EXPECT_EQ(srv_on_all_stages(1), all6(0)) << compute;
    }
    // Rows n/n2 and a05b "so bSI after ib bSI": an SO target is an output
    // for the VB, the IB and SRVs alike.
    const GuestAddr bSo = buffer(256, BIND_VB | BIND_SO);
    const GuestAddr bSoSrv = buffer(256, BIND_SO | BIND_SRV);
    const GuestAddr srvSo = srv(bSoSrv, {R32_UINT, 1, 0, 64});
    const GuestAddr bSI = buffer(256, BIND_SO | BIND_IB);
    ctx(110, {});
    vb_set(0, {bSo}, {16}, {0});
    so_set({bSo});
    EXPECT_EQ(vb_get(0, 1).buffers[0], 0u);
    vb_set(1, {bSo}, {16}, {0});
    VbState vb = vb_get(1, 1);
    EXPECT_EQ(vb.buffers[0], 0u);
    EXPECT_EQ(vb.strides[0], 16u);
    EXPECT_EQ(so_get()[0], bSo);
    ctx(110, {});
    set_srv_all_stages(0, {srvSo});
    so_set({bSoSrv});
    EXPECT_EQ(srv_on_all_stages(0), all6(0));
    set_srv_all_stages(0, {srvSo});
    EXPECT_EQ(srv_on_all_stages(0), all6(0));
    ctx(110, {});
    ctx(19, {bSI, R16_UINT, 4});
    so_set({bSI});
    IbState ib = ib_get();
    EXPECT_EQ(ib.buffer, 0u);
    EXPECT_EQ(ib.format, 0u);
    ctx(19, {bSI, R32_UINT, 8});
    ib = ib_get();
    EXPECT_EQ(ib.buffer, 0u);
    EXPECT_EQ(ib.format, R32_UINT);
    EXPECT_EQ(ib.offset, 8u);
    EXPECT_EQ(so_get()[0], bSI);
}

TEST_F(D3D11BindingRulesTest, AllThirtyTwoVertexBufferSlotsRoundTrip) {
    const GuestAddr vbA = buffer(256, BIND_VB);
    const GuestAddr vbB = buffer(256, BIND_VB);
    // a05 "vb set(8,1)", "vb set(16,1)", "vb set(31,1) get(30,2)".
    vb_set(8, {vbA}, {16}, {32});
    VbState vb = vb_get(8, 1);
    EXPECT_EQ(vb.buffers[0], vbA);
    EXPECT_EQ(vb.strides[0], 16u);
    EXPECT_EQ(vb.offsets[0], 32u);
    vb_set(16, {vbB}, {44}, {64});
    vb = vb_get(16, 1);
    EXPECT_EQ(vb.buffers[0], vbB);
    EXPECT_EQ(vb.strides[0], 44u);
    EXPECT_EQ(vb.offsets[0], 64u);
    vb_set(31, {vbB}, {20}, {40});
    vb = vb_get(30, 2);
    EXPECT_EQ(vb.buffers, (std::vector<GuestAddr>{0, vbB}));
    EXPECT_EQ(vb.strides, (std::vector<u32>{0, 20}));
    EXPECT_EQ(vb.offsets, (std::vector<u32>{0, 40}));

    // ClearState resets strides/offsets only up to the highest slot holding
    // a buffer (a05b "ClearState vertex-buffer strides/offsets").
    ctx(110, {});
    vb_set(0, {0, vbA, 0}, {5, 7, 9}, {1, 3, 5});
    ctx(110, {});
    vb = vb_get(0, 3);
    EXPECT_EQ(vb.buffers, (std::vector<GuestAddr>{0, 0, 0}));
    EXPECT_EQ(vb.strides, (std::vector<u32>{0, 0, 9}));
    EXPECT_EQ(vb.offsets, (std::vector<u32>{0, 0, 5}));
    vb_set(0, {vbA, 0, vbB}, {5, 7, 9}, {1, 3, 5});
    ctx(110, {});
    vb = vb_get(0, 3);
    EXPECT_EQ(vb.strides, (std::vector<u32>{0, 0, 0}));
    vb_set(0, {vbA, 0, 0}, {5, 7, 9}, {1, 3, 5});
    vb_set(0, {0}, {5}, {1});
    ctx(110, {});
    vb = vb_get(0, 3);
    EXPECT_EQ(vb.strides, (std::vector<u32>{5, 7, 9}));
    EXPECT_EQ(vb.offsets, (std::vector<u32>{1, 3, 5}));
}

TEST_F(D3D11BindingRulesTest, InvalidRenderTargetSetsAreDropped) {
    const GuestAddr t16 = texture2d(16, 1, 1, RGBA8, COLOUR);
    const GuestAddr t16b = texture2d(16, 1, 1, RGBA8, COLOUR);
    const GuestAddr t32 = texture2d(32, 1, 1, RGBA8, BIND_RT);
    const GuestAddr t32m = texture2d(32, 2, 1, RGBA8, COLOUR);
    const GuestAddr tA2 = texture2d(16, 1, 2, RGBA8, COLOUR);
    const GuestAddr tMS = texture2d(16, 1, 1, RGBA8, BIND_RT, 4);
    const GuestAddr tD = texture2d(16, 1, 1, R24G8_TYPELESS, BIND_DS | BIND_SRV);
    const GuestAddr rtv16 = rtv(t16);
    const GuestAddr rtv16dup = rtv(t16);
    const GuestAddr rtv16b = rtv(t16b);
    const GuestAddr rtv32 = rtv(t32);
    const GuestAddr rtv32m0 = rtv(t32m, {RGBA8, 4, 0});
    const GuestAddr rtv32m1 = rtv(t32m, {RGBA8, 4, 1});
    const GuestAddr rtvA0 = rtv(tA2, {RGBA8, 5, 0, 0, 1});
    const GuestAddr rtvA1 = rtv(tA2, {RGBA8, 5, 0, 1, 1});
    const GuestAddr rtvAboth = rtv(tA2, {RGBA8, 5, 0, 0, 2});
    const GuestAddr rtvMS = rtv(tMS);
    const GuestAddr dsv16 = dsv(tD, 0);
    ASSERT_NE(rtvMS, 0u);

    struct Case {
        const char* label;
        std::vector<GuestAddr> rtvs;
        GuestAddr depth;
        bool accepted;
    };
    const std::vector<Case> cases{
        {"a05 duplicate rtv", {rtv16, rtv16}, 0, false},
        {"a05 32x32 rtv + 16x16 dsv", {rtv32}, dsv16, false},
        {"a05 16x16 + 32x32", {rtv16, rtv32}, 0, false},
        {"a05b mip 1 of 32x32 + 16x16 dsv", {rtv32m1}, dsv16, true},
        {"a05b 16x16 + mip 1 of 32x32", {rtv16, rtv32m1}, 0, true},
        {"a05b mip 0 + mip 1", {rtv32m0, rtv32m1}, 0, false},
        {"a05b two views of one subresource", {rtv16, rtv16dup}, 0, false},
        {"a05b slice 0 + slice 1", {rtvA0, rtvA1}, 0, true},
        {"a05b slice 0 + both slices", {rtvA0, rtvAboth}, 0, false},
        {"a05b 2D + 2-slice array", {rtv16, rtvAboth}, 0, false},
        {"a05b 2D + 1-slice array view", {rtv16, rtvA1}, 0, true},
        {"a05b 2-slice rtv + 2D dsv", {rtvAboth}, dsv16, false},
        {"a05b 4x rtv + 1x dsv", {rtvMS}, dsv16, false},
        {"a05b 1x + 4x", {rtv16, rtvMS}, 0, false},
        {"a05b {-,rtv16,-} + dsv", {0, rtv16, 0}, dsv16, true},
    };
    for (const Case& c : cases) {
        set_rts({rtv16b}, 0);
        ptrs(PTRS_A, {0, 0, 0});
        for (usize i = 0; i < c.rtvs.size(); ++i) {
            ASSERT_TRUE(memory.write_u64(PTRS_A + i * 8, c.rtvs[i]).is_ok());
        }
        ctx(33, {c.rtvs.size(), PTRS_A, c.depth});
        const OmState s = om_get();
        if (c.accepted) {
            for (usize i = 0; i < c.rtvs.size(); ++i) {
                EXPECT_EQ(s.rtv[i], c.rtvs[i]) << c.label;
            }
            EXPECT_EQ(s.dsv, c.depth) << c.label;
        } else {
            EXPECT_EQ(s.rtv, (OmArray{rtv16b})) << c.label;
            EXPECT_EQ(s.dsv, 0u) << c.label;
        }
    }
    // A dropped OMSetRenderTargets leaves the OM UAVs bound too (a05b).
    const GuestAddr uavA0 = uav(tA2, {RGBA8, 5, 0, 0, 1});
    ctx(110, {});
    om(1, {rtv16b}, 0, 1, 1, {uavA0});
    set_rts({rtv16, rtv16dup}, 0);
    const OmState s = om_get();
    EXPECT_EQ(s.rtv, (OmArray{rtv16b}));
    EXPECT_EQ(s.uav, (OmArray{0, uavA0}));
}

TEST_F(D3D11BindingRulesTest, ConstantBuffer1WindowsAreValidatedWhole) {
    const GuestAddr cbA = buffer(4096, 0x4);
    const GuestAddr cbB = buffer(4096, 0x4);
    const GuestAddr cbC = buffer(256, 0x4);
    struct Stage { usize set1, get1, plain; };
    const std::array<Stage, 6> stages{{{119, 125, 7}, {120, 126, 62},
                                       {121, 127, 66}, {122, 128, 22},
                                       {123, 129, 16}, {124, 130, 71}}};
    const auto get1 = [&](const Stage& st, u32 n) {
        ctx(st.get1, {0, n, GET, GET_U32_A, GET_U32_B});
        std::vector<u32> out;
        for (u32 i = 0; i < n; ++i) {
            out.push_back(static_cast<u32>(read_ptr(GET + i * 8) != 0));
            out.push_back(read32(GET_U32_A + i * 4));
            out.push_back(read32(GET_U32_B + i * 4));
        }
        return out; // {bound, first, num} per slot
    };
    const auto set1 = [&](const Stage& st, std::initializer_list<GuestAddr> b,
                          std::initializer_list<u32> first,
                          std::initializer_list<u32> num) {
        ptrs(PTRS_A, b);
        words(U32_A, first);
        words(U32_B, num);
        ctx(st.set1, {0, b.size(), PTRS_A, first.size() ? U32_A : 0,
                      num.size() ? U32_B : 0});
    };
    for (const Stage& st : stages) {
        ctx(110, {});
        // a05 "fresh ... cb1": an unbound slot reads first 0, num 4096.
        EXPECT_EQ(get1(st, 2), (std::vector<u32>{0, 0, 4096, 0, 0, 4096}));
        set1(st, {cbA, cbB}, {16, 32}, {16, 48});
        EXPECT_EQ(get1(st, 3),
                  (std::vector<u32>{1, 16, 16, 1, 32, 48, 0, 0, 4096}));
        // Plain Set: the whole buffer, reported as first 0, num 4096.
        ptrs(PTRS_A, {cbA});
        ctx(st.plain, {0, 1, PTRS_A});
        EXPECT_EQ(get1(st, 2), (std::vector<u32>{1, 0, 4096, 1, 32, 48}));
    }
    const Stage& vs = stages[0];
    ctx(110, {});
    ptrs(PTRS_A, {cbC});
    ctx(vs.plain, {0, 1, PTRS_A});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 0, 4096}));
    set1(vs, {cbA}, {}, {});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 0, 4096}));
    // Each invalid window drops the call and keeps cbA 16/16.
    set1(vs, {cbA}, {16}, {16});
    set1(vs, {cbB}, {8}, {16});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 16}));
    set1(vs, {cbB}, {16}, {20});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 16}));
    set1(vs, {cbB}, {16}, {4112});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 16}));
    set1(vs, {cbB}, {32}, {});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 16}));
    set1(vs, {cbB}, {}, {16});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 16}));
    // a05b: a NULL buffer is validated too, and so is every later entry.
    set1(vs, {0}, {8}, {16});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 16}));
    set1(vs, {cbB, cbB}, {32, 8}, {16, 16});
    EXPECT_EQ(get1(vs, 2), (std::vector<u32>{1, 16, 16, 0, 0, 4096}));
    set1(vs, {cbB, cbB}, {0, 0}, {16, 4112});
    EXPECT_EQ(get1(vs, 2), (std::vector<u32>{1, 16, 16, 0, 0, 4096}));
    // Valid windows are stored as given, even past the buffer's end, and a
    // count of 0 is a real (empty) window.
    set1(vs, {cbB}, {16}, {4096});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 16, 4096}));
    set1(vs, {cbB}, {4096}, {16});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 4096, 16}));
    set1(vs, {cbB}, {0}, {0});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 0, 0}));
    set1(vs, {cbB}, {65536}, {16});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{1, 65536, 16}));
    set1(vs, {0}, {16}, {16});
    EXPECT_EQ(get1(vs, 1), (std::vector<u32>{0, 0, 4096}));
}

TEST_F(D3D11BindingRulesTest, ViewportCountsStencilRefAndStreamTargetsFollowWindows) {
    // a05 "rs set 17 after 2: vp 2 sc 2".
    std::array<float, 6 * 17> viewports{};
    for (usize i = 0; i < 17; ++i) viewports[i * 6 + 2] = 10.0f;
    ASSERT_TRUE(memory.write_bytes(
        BLOB, reinterpret_cast<const Byte*>(viewports.data()),
        sizeof(viewports)).is_ok());
    ctx(44, {2, BLOB});
    ctx(45, {2, BLOB});
    ctx(44, {17, BLOB});
    ctx(45, {17, BLOB});
    ASSERT_TRUE(memory.write_u32(GET_U32_A, 0).is_ok());
    ctx(95, {GET_U32_A, 0});
    EXPECT_EQ(read32(GET_U32_A), 2u);
    ctx(96, {GET_U32_A, 0});
    EXPECT_EQ(read32(GET_U32_A), 2u);

    // a05 "dss set(NULL,0x1FF): ref 0xff", "dss set(DSS,0xFFFFFFFF): ref 0xff".
    for (const u32 ref : {0x1FFu, 0xFFFFFFFFu}) {
        ctx(36, {0, ref});
        ctx(92, {GET, GET_U32_A});
        EXPECT_EQ(read32(GET_U32_A), 0xFFu) << ref;
    }
    ctx(36, {0, 0x55});
    ctx(92, {GET, GET_U32_A});
    EXPECT_EQ(read32(GET_U32_A), 0x55u);

    // a05 "so set {bSoA,bSoA} duplicate get(2): bSoB -".
    const GuestAddr bSoA = buffer(256, BIND_SO);
    const GuestAddr bSoB = buffer(256, BIND_SO);
    so_set({bSoB});
    so_set({bSoA, bSoA});
    EXPECT_EQ(so_get(), (std::vector<GuestAddr>{bSoB, 0, 0, 0}));
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
