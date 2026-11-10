// Windows measurements: tools/oracle_probe/get_random_rgn_probe*.expected.
// Visible geometry and layered rules: get_random_rgn_visible*.expected and
// system_region_layered_visible*.expected, measured on the Windows desktop.
#include <gtest/gtest.h>
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include <algorithm>
using namespace vitreo;
using namespace vitreo::winapi;
namespace facts {
#include "winapi/graphics/gdi32/random_rgn_contract.inc"
#include "winapi/graphics/gdi32/recording_creation_contract.inc"
}

TEST(GetRandomRgnContract, MemoryErrorsAndBothGuestAbis) {
    for (const bool is64:{false,true}) {
        HandleTable handles;
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000,0x4000,MemoryProtection::ReadWrite).is_ok());
        GdiManager mgr(handles);
        const Handle dc=mgr.create_compatible_dc(0);
        const Handle destination=mgr.create_rect_rgn(701,702,711,712);
        auto exports=make_gdi32_exports(mgr,{},is64);
        const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),
            [](const ExportEntry& e){return e.name=="GetRandomRgn";});
        ASSERT_NE(entry,exports.entries.end());
        EXPECT_EQ(entry->ordinal,is64?facts::random_rgn_contract::ordinal64:facts::random_rgn_contract::ordinal32);
        EXPECT_EQ(entry->stdcall_bytes,facts::random_rgn_contract::stdcall_bytes);
        const auto call=[&](Handle hdc,Handle region,i32 selector) {
            cpu::CpuState state;
            if (is64) {
                state.init_long_mode();state.gpr.rsp=0x4000;
                state.gpr.rcx=hdc;state.gpr.rdx=region;state.gpr.r8=static_cast<u32>(selector);
            } else {
                state.init_protected_mode();state.set_sp(0x4000);
                (void)memory.write_u32(0x4004,hdc);(void)memory.write_u32(0x4008,region);
                (void)memory.write_u32(0x400c,static_cast<u32>(selector));
            }
            Marshaler m(memory);m.attach_cpu(state);entry->handler(m);
            return static_cast<i32>(state.gpr.rax);
        };
        for (const i32 selector:{0,2,5,-1}) {
            set_last_error(0x12345678);
            EXPECT_EQ(call(dc,destination,selector),0);
            ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(destination,shape));
            EXPECT_EQ(shape.left,701);EXPECT_EQ(shape.bottom,712);
            EXPECT_EQ(get_last_error(),0x12345678u);
        }
        set_last_error(0x12345678);
        EXPECT_EQ(call(0,destination,facts::random_rgn_contract::SYSRGN),-1);
        EXPECT_EQ(get_last_error(),is64?static_cast<u32>(facts::random_rgn_contract::ERROR_INVALID_HANDLE):0x12345678u);
        set_last_error(0x12345678);
        EXPECT_EQ(call(dc,0,facts::random_rgn_contract::SYSRGN),-1);
        EXPECT_EQ(get_last_error(),0x12345678u);
        ASSERT_EQ(call(dc,destination,facts::random_rgn_contract::SYSRGN),1);
        ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(destination,shape));
        EXPECT_EQ(shape.right,1);EXPECT_EQ(shape.bottom,1);
        const Handle bitmap=mgr.create_compatible_bitmap(dc,8,6);
        ASSERT_NE(bitmap,0u);(void)mgr.select_object(dc,bitmap);
        (void)mgr.set_map_mode(dc,8);(void)mgr.set_viewport_org(dc,80,90);
        const Handle app=mgr.create_rect_rgn(3,4,13,14);(void)mgr.select_clip_rgn(dc,app);
        ASSERT_EQ(call(dc,destination,facts::random_rgn_contract::SYSRGN),1);
        ASSERT_TRUE(mgr.get_region_rect(destination,shape));
        EXPECT_EQ(shape.left,0);EXPECT_EQ(shape.top,0);EXPECT_EQ(shape.right,8);EXPECT_EQ(shape.bottom,6);
    }
}

TEST(GetRandomRgnContract, DocumentedVisiblePaintAndScreenGeometry) {
    HandleTable handles;GdiManager mgr(handles);windowing::HwndManager windows;
    mgr.set_hwnd_manager(&windows);
    windowing::WindowData data;
    data.rect={100,200,220,300};data.client_rect={0,0,100,60};data.client_offset={5,20};
    data.style=windowing::WS::VISIBLE;data.visible=true;
    const auto hwnd=windows.create(data);
    const Handle dc=mgr.get_dc(hwnd),destination=mgr.create_rect_rgn(701,702,711,712);
    const auto query=[&](Handle context) {
        return mgr.get_random_region(context,destination,facts::random_rgn_contract::SYSRGN,true);
    };
    ASSERT_EQ(query(dc),1);ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,105);EXPECT_EQ(shape.top,220);EXPECT_EQ(shape.right,205);EXPECT_EQ(shape.bottom,280);
    const auto whole=mgr.get_window_dc(hwnd);ASSERT_EQ(query(whole),1);
    ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,100);EXPECT_EQ(shape.top,200);EXPECT_EQ(shape.right,220);EXPECT_EQ(shape.bottom,300);
    mgr.set_dc_paint_region(dc,ClipRect{10,10,30,40,true,{}});
    const Handle app=mgr.create_rect_rgn(1,1,2,2);(void)mgr.select_clip_rgn(dc,app);
    ASSERT_EQ(query(dc),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,115);EXPECT_EQ(shape.top,230);EXPECT_EQ(shape.right,135);EXPECT_EQ(shape.bottom,260);
    windowing::WindowData cover;cover.visible=true;cover.style=windowing::WS::VISIBLE;
    cover.rect={120,240,150,260};cover.client_rect={0,0,30,20};const auto cover_hwnd=windows.create(cover);
    ASSERT_EQ(query(dc),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_TRUE(shape.contains(116,235));EXPECT_TRUE(shape.contains(125,245));
    windows.find(hwnd)->visible=false;
    ASSERT_EQ(query(dc),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));EXPECT_TRUE(shape.is_empty());
    windows.destroy(cover_hwnd);windows.find(hwnd)->visible=true;
    mgr.set_dc_paint_region(dc,ClipRect{});
    GdiManager::SystemRegionState scene;
    scene.window_shapes.emplace_back(hwnd,ClipRect{0,0,60,100,true,{}});
    const auto query_shape=[&] {return mgr.get_random_region(dc,destination,
        facts::random_rgn_contract::SYSRGN,true,&scene);};
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,105);EXPECT_EQ(shape.right,160);
    windows.find(hwnd)->ex_style=static_cast<u32>(facts::random_rgn_contract::WS_EX_LAYOUTRTL);
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,160);EXPECT_EQ(shape.right,205);
    windows.find(hwnd)->ex_style=0;
    scene.window_shapes[0].second=ClipRect{0,0,120,100,true,{{0,0,20,100},{100,0,120,100}}};
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_TRUE(shape.contains(110,230));EXPECT_FALSE(shape.contains(150,230));EXPECT_TRUE(shape.contains(202,230));
    scene.window_shapes[0].second=ClipRect{0,0,0,0,true,{}};
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));EXPECT_TRUE(shape.is_empty());
    ASSERT_TRUE(mgr.set_rect_region(destination,701,702,711,712));
    // An unrelated process presence cannot reject this local region.
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));EXPECT_TRUE(shape.is_empty());
    scene.window_shapes.clear();
    windows.find(hwnd)->ex_style=static_cast<u32>(facts::random_rgn_contract::WS_EX_LAYERED);
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_TRUE(shape.is_empty()); // Layering has not yet been initialized.
    scene.layered_windows.emplace_back(hwnd,false);
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,105);EXPECT_EQ(shape.right,205);
    scene.window_shapes.emplace_back(hwnd,ClipRect{0,0,60,100,true,{}});
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,105);EXPECT_EQ(shape.right,160);
    scene.window_shapes.clear();
    mgr.set_dc_paint_region(dc,ClipRect{10,20,30,40,true,{}});
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,115);EXPECT_EQ(shape.top,240);EXPECT_EQ(shape.right,135);EXPECT_EQ(shape.bottom,260);
    scene.window_shapes.emplace_back(hwnd,ClipRect{15,25,45,55,true,{}});
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,115);EXPECT_EQ(shape.top,240);EXPECT_EQ(shape.right,135);EXPECT_EQ(shape.bottom,255);
    scene.window_shapes.clear();
    mgr.set_dc_paint_region(dc,ClipRect{});
    scene.layered_windows[0].second=true;
    ASSERT_TRUE(mgr.set_rect_region(destination,701,702,711,712));
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));EXPECT_TRUE(shape.is_empty());
    windows.find(hwnd)->ex_style=static_cast<u32>(facts::random_rgn_contract::WS_EX_TRANSPARENT);
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,105);EXPECT_EQ(shape.right,205);
    ASSERT_TRUE(mgr.set_rect_region(destination,701,702,711,712));
    windows.find(hwnd)->ex_style=0;
    windows.find(hwnd)->class_style=static_cast<u32>(facts::random_rgn_contract::CS_PARENTDC);
    windows.find(hwnd)->style|=windowing::WS::CHILD;
    ASSERT_EQ(query_shape(),1);ASSERT_TRUE(mgr.get_region_rect(destination,shape));
    EXPECT_EQ(shape.left,0);EXPECT_EQ(shape.top,0); // CS_PARENTDC inherits the desktop.
}

TEST(GetRandomRgnContract, LayeredAndTransparentChildrenDoNotOcclude) {
    // Both ABIs measured by system_region_layered_visible*.expected:
    // ordinary child excludes, transparent/layered child does not.
    for (const bool is64:{false,true}) {
        HandleTable handles;GdiManager mgr(handles);windowing::HwndManager windows;
        mgr.set_hwnd_manager(&windows);
        windowing::WindowData parent;
        parent.rect={100,120,300,280};parent.client_rect={0,0,200,160};
        parent.style=windowing::WS::VISIBLE|windowing::WS::CLIPCHILDREN;parent.visible=true;
        const auto p=windows.create(parent);
        windowing::WindowData child;
        child.parent=p;child.rect={10,20,70,80};child.client_rect={0,0,60,60};
        child.style=windowing::WS::VISIBLE|windowing::WS::CHILD|windowing::WS::CLIPSIBLINGS;
        child.visible=true;const auto c=windows.create(child);
        const auto pd=mgr.get_dc(p),cd=mgr.get_dc(c),r=mgr.create_rect_rgn(0,0,0,0);
        windowing::WindowData cover=child;const auto top=windows.create(cover);
        ASSERT_TRUE(windows.set_z_order(top,windowing::HWND_TOP));
        for (const auto ex:{0u,static_cast<u32>(facts::random_rgn_contract::WS_EX_TRANSPARENT),
                           static_cast<u32>(facts::random_rgn_contract::WS_EX_LAYERED)}) {
            windows.find(top)->ex_style=ex;windows.find(c)->ex_style=ex;
            ASSERT_EQ(mgr.get_random_region(pd,r,facts::random_rgn_contract::SYSRGN,is64),1);
            ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(r,shape));
            EXPECT_EQ(shape.contains(120,150),ex!=0);
            windows.find(c)->ex_style=0;
            ASSERT_EQ(mgr.get_random_region(cd,r,facts::random_rgn_contract::SYSRGN,is64),1);
            ASSERT_TRUE(mgr.get_region_rect(r,shape));EXPECT_EQ(shape.contains(120,150),ex!=0);
        }
    }
}

TEST(GetRandomRgnContract, ParentDcUsesParentVisibilityAndChildPaintOrigin) {
    // Native get_random_rgn_visible{32,64}.expected parentdc cases.
    for (const bool is64:{false,true}) {
        HandleTable handles;GdiManager mgr(handles);windowing::HwndManager windows;
        mgr.set_hwnd_manager(&windows);
        windowing::WindowData parent;
        parent.rect={400,200,640,380};parent.client_rect={0,0,240,180};
        parent.style=windowing::WS::VISIBLE|windowing::WS::CLIPCHILDREN;parent.visible=true;
        const auto p=windows.create(parent);
        windowing::WindowData child;
        child.parent=p;child.rect={40,30,100,80};child.client_rect={0,0,60,50};
        child.style=windowing::WS::VISIBLE|windowing::WS::CHILD;child.visible=true;
        child.class_style=static_cast<u32>(facts::random_rgn_contract::CS_PARENTDC);
        const auto c=windows.create(child);
        const auto dc=mgr.get_dc(c),whole=mgr.get_window_dc(c),r=mgr.create_rect_rgn(0,0,0,0);
        GdiManager::SystemRegionState scene;
        scene.window_shapes.emplace_back(c,ClipRect{5,6,45,46,true,{}});
        const auto check=[&](Handle context,i32 l,i32 t,i32 rr,i32 b) {
            ASSERT_EQ(mgr.get_random_region(context,r,facts::random_rgn_contract::SYSRGN,is64,&scene),1);
            ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(r,shape));
            EXPECT_EQ(shape.left,l);EXPECT_EQ(shape.top,t);EXPECT_EQ(shape.right,rr);EXPECT_EQ(shape.bottom,b);
        };
        check(dc,400,200,640,380);check(whole,445,236,485,276);
        mgr.set_dc_paint_region(dc,ClipRect{0,0,20,20,true,{}});check(dc,440,230,460,250);
        scene.window_shapes.emplace_back(p,ClipRect{60,40,200,160,true,{}});
        mgr.set_dc_paint_region(dc,ClipRect{0,0,40,30,true,{}});check(dc,460,240,480,260);
        scene.window_shapes.clear();mgr.set_dc_paint_region(dc,ClipRect{});
        for (const auto flags:{windowing::WS::CLIPSIBLINGS,windowing::WS::CLIPCHILDREN}) {
            windows.find(c)->style|=flags;check(dc,440,230,500,280);windows.find(c)->style&=~flags;
        }
    }
}

namespace {
#include "../../oracle_data/random_rgn_meta_cases.inc"
}
TEST(GetRandomRgnContract, NativeMetaRegionMatrixAndBothAbis) {
    for (const bool is64:{false,true}) for (const auto& row:meta_cases) {
        SCOPED_TRACE(::testing::Message()<<is64<<":"<<row.query_before<<","<<row.meta<<","<<row.app<<","<<row.disjoint);
        HandleTable handles;GdiManager mgr(handles);
        const auto dc=mgr.create_compatible_dc(0),r=mgr.create_rect_rgn(701,702,711,712);
        const auto make=[&](int kind,bool meta) {
            const int d=row.disjoint;
            ClipRect shape=meta?ClipRect{10,20,70,80,true,{}}:
                ClipRect{d?100:20,d?110:30,d?120:60,d?130:70,true,{}};
            if (kind==1)shape=ClipRect{0,0,0,0,true,{}};
            if (kind==3) shape=meta?ClipRect{10,20,70,80,true,{{10,20,30,40},{50,60,70,80}}}:
                ClipRect{d?100:20,d?110:30,d?120:60,d?130:70,true,
                    {{d?100:20,d?110:30,d?105:25,d?115:35},{d?115:55,d?125:65,d?120:60,d?130:70}}};
            const auto h=mgr.create_rect_rgn(0,0,0,0);EXPECT_TRUE(mgr.set_region(h,shape));return h;
        };
        if (row.meta) {(void)mgr.select_clip_rgn(dc,make(row.meta,true));(void)mgr.set_meta_region(dc);}
        if (row.app) (void)mgr.select_clip_rgn(dc,make(row.app,false));
        if (row.query_before)(void)mgr.get_random_region(dc,r,facts::random_rgn_contract::APIRGN,is64);
        set_last_error(0x12345678);
        EXPECT_EQ(mgr.set_meta_region(dc),row.first);EXPECT_EQ(get_last_error(),0x12345678u);
        EXPECT_EQ(mgr.set_meta_region(dc),row.repeat);
        EXPECT_EQ(mgr.get_random_region(dc,r,facts::random_rgn_contract::CLIPRGN,is64),0);
    }
}

TEST(GetRandomRgnContract, PrivateSelectorsErrorsMappingSaveRestoreAndPaint) {
    for (const bool is64:{false,true}) {
        HandleTable handles;GdiManager mgr(handles);
        const auto dc=mgr.create_compatible_dc(0),r=mgr.create_rect_rgn(701,702,711,712);
        const auto bitmap=mgr.create_compatible_bitmap(dc,200,160);(void)mgr.select_object(dc,bitmap);
        for (const int selector:{-2147483647-1,-2,-1,0,1,2,3,4,5,6,2147483647}) {
            set_last_error(0x12345678);
            EXPECT_EQ(mgr.get_random_region(0,r,selector,is64),selector==5?0:-1);
            EXPECT_EQ(get_last_error(),selector==5||!is64?0x12345678u:6u);
            set_last_error(0x12345678);
            EXPECT_EQ(mgr.get_random_region(dc,0,selector,is64),selector==5?0:-1);
            EXPECT_EQ(get_last_error(),0x12345678u);
        }
        const auto query=[&](int selector,int ret,int l,int t,int rr,int b) {
            ASSERT_EQ(mgr.get_random_region(dc,r,selector,is64),ret);
            ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(r,shape));
            EXPECT_EQ(shape.left,l);EXPECT_EQ(shape.top,t);EXPECT_EQ(shape.right,rr);EXPECT_EQ(shape.bottom,b);
        };
        query(1,0,701,702,711,712);query(2,0,701,702,711,712);query(3,0,701,702,711,712);
        EXPECT_EQ(mgr.set_meta_region(dc),2);query(2,0,701,702,711,712);
        const int original=mgr.save_dc(dc);
        const auto a=mgr.create_rect_rgn(10,20,70,80),b=mgr.create_rect_rgn(30,10,90,60);
        (void)mgr.select_clip_rgn(dc,a);query(1,1,10,20,70,80);query(3,1,10,20,70,80);
        EXPECT_EQ(mgr.set_meta_region(dc),2);query(2,1,10,20,70,80);
        (void)mgr.select_clip_rgn(dc,b);query(1,1,30,10,90,60);query(3,1,30,20,70,60);
        EXPECT_FALSE(mgr.pt_visible(dc,80,30));EXPECT_TRUE(mgr.pt_visible(dc,40,30));
        const int nested=mgr.save_dc(dc);(void)mgr.select_clip_rgn(dc,0);
        query(3,1,10,20,70,80);ASSERT_TRUE(mgr.restore_dc(dc,nested));query(3,1,30,20,70,60);
        (void)mgr.set_map_mode(dc,8);(void)mgr.set_window_ext(dc,1,1,nullptr);
        (void)mgr.set_viewport_ext(dc,2,3,nullptr);(void)mgr.set_window_org(dc,20,30);
        (void)mgr.set_viewport_org(dc,40,50);query(1,1,30,10,90,60);
        (void)mgr.select_clip_rgn(dc,a);(void)mgr.intersect_clip_rect(dc,25,35,35,45);
        query(1,1,50,65,70,80);query(3,1,50,65,70,80);query(4,1,0,0,200,160);
        ASSERT_TRUE(mgr.restore_dc(dc,original));
        (void)mgr.set_rect_region(r,701,702,711,712);query(2,0,701,702,711,712);
        mgr.set_dc_paint_region(dc,ClipRect{10,20,70,80,true,{}});
        query(1,0,701,702,711,712);(void)mgr.select_clip_rgn(dc,b);
        query(1,1,30,10,90,60);EXPECT_EQ(mgr.set_meta_region(dc),2);
        query(2,1,30,10,90,60);query(4,1,0,0,200,160);
        set_last_error(0x12345678);EXPECT_EQ(mgr.set_meta_region(0),0);EXPECT_EQ(get_last_error(),6u);
        auto exports=make_gdi32_exports(mgr,{},is64);
        for (const auto& e:exports.entries) {
            if(e.name=="GetMetaRgn") {EXPECT_EQ(e.ordinal,is64?facts::random_rgn_contract::GetMetaRgn_ordinal64:facts::random_rgn_contract::GetMetaRgn_ordinal32);EXPECT_EQ(e.stdcall_bytes,facts::random_rgn_contract::GetMetaRgn_stdcall_bytes);}
            if(e.name=="SetMetaRgn") {EXPECT_EQ(e.ordinal,is64?facts::random_rgn_contract::SetMetaRgn_ordinal64:facts::random_rgn_contract::SetMetaRgn_ordinal32);EXPECT_EQ(e.stdcall_bytes,facts::random_rgn_contract::SetMetaRgn_stdcall_bytes);}
        }
    }
}

TEST(GetRandomRgnContract, DisplayDeviceContextUsesResolutionRatherThanScreenOrigin) {
    HandleTable handles;GdiManager mgr(handles);windowing::HwndManager windows;
    mgr.set_hwnd_manager(&windows);mgr.set_virtual_screen_size(1000,600);
    windows.find(windowing::HWND_DESKTOP)->rect={-200,0,800,600};
    windows.find(windowing::HWND_DESKTOP)->client_rect={0,0,1000,600};
    const auto dc=mgr.create_ic_w(),screen=mgr.get_dc(0),r=mgr.create_rect_rgn(0,0,0,0);
    for (const bool is64:{false,true}) {
        ASSERT_EQ(mgr.get_random_region(dc,r,4,is64),1);ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(r,shape));
        EXPECT_EQ(shape.left,0);EXPECT_EQ(shape.top,0);EXPECT_EQ(shape.right,1000);EXPECT_EQ(shape.bottom,600);
        ASSERT_EQ(mgr.get_random_region(screen,r,4,is64),1);ASSERT_TRUE(mgr.get_region_rect(r,shape));
        EXPECT_EQ(shape.left,-200);EXPECT_EQ(shape.right,800);
    }
}

TEST(GetRandomRgnContract, ImmutablePeerSceneClipsChildrenAndRemoteAncestors) {
    // Windows system_region_peer_visible{32,64}.expected, translated origin.
    for(const bool is64:{false,true}) {
        HandleTable handles;GdiManager mgr(handles);windowing::HwndManager local,peer;
        mgr.set_hwnd_manager(&local);u64 next=0x50000;peer.set_handle_allocator([&]{return next++;});
        windowing::WindowData parent;parent.rect={400,200,640,380};parent.client_rect={0,0,240,180};
        parent.visible=true;parent.style=windowing::WS::VISIBLE|windowing::WS::CLIPCHILDREN;
        const auto p=local.create(parent);
        windowing::WindowData child;child.parent=p;child.rect={40,30,100,80};child.client_rect={0,0,60,50};
        child.visible=true;child.style=windowing::WS::CHILD|windowing::WS::VISIBLE|windowing::WS::CLIPSIBLINGS;
        const auto a=local.create(child);child.rect={60,40,100,70};child.client_rect={0,0,40,30};
        const auto b=peer.create(child),dc=mgr.get_dc(a),remote=mgr.get_dc(b),r=mgr.create_rect_rgn(701,702,711,712);
        GdiManager::SystemRegionState scene;scene.has_snapshot=true;scene.window_geometry=local.geometry_snapshot();
        for(auto& g:scene.window_geometry)if(g.hwnd==a)g.z_order=1;
        for(auto g:peer.geometry_snapshot())if(g.hwnd==b){g.z_order=0;scene.window_geometry.push_back(g);}
        const auto peer_geometry=[&]()->windowing::HwndManager::Geometry& {
            return *std::find_if(scene.window_geometry.begin(),scene.window_geometry.end(),[&](const auto& g){return g.hwnd==b;});
        };
        const auto query=[&](Handle context){EXPECT_EQ(mgr.get_random_region(context,r,4,is64,&scene),1);
            ClipRect shape;EXPECT_TRUE(mgr.get_region_rect(r,shape));return shape;};
        auto shape=query(dc);EXPECT_TRUE(shape.contains(445,235));EXPECT_FALSE(shape.contains(465,245));
        peer_geometry().z_order=2;shape=query(dc);EXPECT_TRUE(shape.contains(465,245));
        peer_geometry().z_order=0;
        for(const auto ex:{facts::random_rgn_contract::WS_EX_TRANSPARENT,facts::random_rgn_contract::WS_EX_LAYERED}) {
            peer_geometry().ex_style=static_cast<u32>(ex);shape=query(dc);EXPECT_TRUE(shape.contains(465,245));
        }
        peer_geometry().ex_style=0;peer_geometry().rect={220,160,260,190};
        shape=query(remote);EXPECT_EQ(shape.left,620);EXPECT_EQ(shape.top,360);EXPECT_EQ(shape.right,640);EXPECT_EQ(shape.bottom,380);
        mgr.set_dc_paint_region(remote,ClipRect{10,10,30,25,true,{}});
        shape=query(remote);EXPECT_EQ(shape.left,630);EXPECT_EQ(shape.top,370);EXPECT_EQ(shape.right,640);EXPECT_EQ(shape.bottom,380);
        scene.snapshot_complete=false;(void)mgr.set_rect_region(r,701,702,711,712);set_last_error(0x12345678);
        EXPECT_EQ(mgr.get_random_region(remote,r,4,is64,&scene),-1);ASSERT_TRUE(mgr.get_region_rect(r,shape));
        EXPECT_EQ(shape.left,701);EXPECT_EQ(get_last_error(),0x12345678u);
    }
}

namespace {
#include "../../oracle_data/random_rgn_producer_cases.inc"
}
TEST(GetRandomRgnContract, AllNativeImplicitClipProducerCasesBothAbis) {
    for(const bool is64:{false,true})for(const auto& row:producer_cases) {
        SCOPED_TRACE(::testing::Message()<<is64<<":"<<row.dc<<","<<row.meta<<","<<row.op<<","<<row.geometry);
        HandleTable handles;GdiManager mgr(handles);windowing::HwndManager windows;mgr.set_hwnd_manager(&windows);
        Handle dc=0;
        const std::array<ClipRect,5> bases{{{0,0,8,6,true,{}},{0,0,200,160,true,{}},
            {-40,-30,200,150,true,{}},{0,0,240,180,true,{}},{0,0,200,160,true,{}}}};
        if(row.dc==0) {dc=mgr.create_compatible_dc(0);(void)mgr.select_object(dc,mgr.create_compatible_bitmap(dc,8,6));}
        else {
            windowing::WindowData data;data.visible=true;data.style=windowing::WS::VISIBLE;
            if(row.dc==2) {
                data.rect={400,200,640,380};data.client_rect={0,0,240,180};const auto parent=windows.create(data);
                data.parent=parent;data.style|=windowing::WS::CHILD;
                data.class_style=static_cast<u32>(facts::random_rgn_contract::CS_PARENTDC);
                data.rect={40,30,100,80};data.client_rect={0,0,60,50};
            } else if(row.dc==3) {data.rect={700,100,940,280};data.client_rect={0,0,224,141};data.client_offset={8,31};}
            else {data.rect={100,120,300,280};data.client_rect={0,0,200,160};}
            const auto hwnd=windows.create(data);dc=row.dc==3?mgr.get_window_dc(hwnd):mgr.get_dc(hwnd);
            if(row.dc==4)mgr.set_dc_paint_region(dc,ClipRect{10,20,70,80,true,{}});
        }
        if(row.meta) {
            const auto& base=bases[row.dc];const auto meta=mgr.create_rect_rgn(base.left+1,base.top+1,base.right-1,base.bottom-1);
            (void)mgr.select_clip_rgn(dc,meta);(void)mgr.set_meta_region(dc);
        }
        const auto& a=row.input;const auto operand=mgr.create_rect_rgn(a[0],a[1],a[2],a[3]);
        set_last_error(0x12345678);i32 result=0;
        if(row.op==0)result=mgr.intersect_clip_rect(dc,a[0],a[1],a[2],a[3]);
        else if(row.op==1)result=mgr.exclude_clip_rect(dc,a[0],a[1],a[2],a[3]);
        else result=mgr.ext_select_clip_rgn(dc,operand,row.op-1);
        EXPECT_EQ(result,row.mutation);EXPECT_EQ(get_last_error(),0x12345678u);
        const auto out=mgr.create_rect_rgn(701,702,711,712);
        for(int selector=1;selector<=4;++selector) {
            (void)mgr.set_rect_region(out,701,702,711,712);
            EXPECT_EQ(mgr.get_random_region(dc,out,selector,is64),row.queries[selector-1].ret);
            ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(out,shape));
            EXPECT_EQ(shape.parts(),row.queries[selector-1].parts)<<"selector="<<selector;
        }
    }
}

TEST(GetRandomRgnContract, MetaExportsMarshalBothGuestAbisAndPreserveErrors) {
    for(const bool is64:{false,true}) {
        HandleTable handles;GdiManager mgr(handles);GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000,0x4000,MemoryProtection::ReadWrite).is_ok());
        const auto dc=mgr.create_compatible_dc(0),out=mgr.create_rect_rgn(701,702,711,712);
        auto exports=make_gdi32_exports(mgr,{},is64);
        const auto call=[&](const char* name,std::initializer_list<u32> args) {
            cpu::CpuState cpu;if(is64){cpu.init_long_mode();cpu.gpr.rsp=0x4000;}
            else{cpu.init_protected_mode();cpu.set_sp(0x4000);}
            usize index=0;for(const auto value:args) {
                if(is64){if(index==0)cpu.gpr.rcx=value;else if(index==1)cpu.gpr.rdx=value;}
                else (void)memory.write_u32(0x4004+index*4,value);++index;
            }
            const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),[&](const auto& e){return e.name==name;});
            EXPECT_NE(entry,exports.entries.end());if(entry==exports.entries.end())return -999;
            Marshaler m(memory);m.attach_cpu(cpu);entry->handler(m);return static_cast<i32>(cpu.gpr.rax);
        };
        set_last_error(0x12345678);EXPECT_EQ(call("GetMetaRgn",{dc,out}),0);EXPECT_EQ(get_last_error(),0x12345678u);
        EXPECT_EQ(call("SetMetaRgn",{dc}),2);EXPECT_EQ(call("GetMetaRgn",{dc,out}),0);
        ClipRect shape;ASSERT_TRUE(mgr.get_region_rect(out,shape));EXPECT_EQ(shape.left,701);
        const auto clip=mgr.create_rect_rgn(10,20,70,80);(void)mgr.select_clip_rgn(dc,clip);
        EXPECT_EQ(call("SetMetaRgn",{dc}),2);EXPECT_EQ(call("GetMetaRgn",{dc,out}),1);
        ASSERT_TRUE(mgr.get_region_rect(out,shape));EXPECT_EQ(shape.left,10);EXPECT_EQ(shape.bottom,80);
        set_last_error(0x12345678);EXPECT_EQ(call("GetMetaRgn",{0,out}),-1);
        EXPECT_EQ(get_last_error(),is64?6u:0x12345678u);
        set_last_error(0x12345678);EXPECT_EQ(call("GetMetaRgn",{dc,0}),-1);EXPECT_EQ(get_last_error(),0x12345678u);
        set_last_error(0x12345678);EXPECT_EQ(call("SetMetaRgn",{0}),0);EXPECT_EQ(get_last_error(),6u);
    }
}

TEST(GetRandomRgnContract, UnsupportedRecordingCreationNeverReturnsSyntheticDc) {
    // Native creators need real recording semantics. The runtime's explicit
    // unsupported policy must return their documented NULL failure instead of
    // a generic missing-export integer that masquerades as a recording HDC.
    for(const bool is64:{false,true}) {
        HandleTable handles;GdiManager mgr(handles);GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000,0x4000,MemoryProtection::ReadWrite).is_ok());
        auto exports=make_gdi32_exports(mgr,{},is64);
        for(const auto& abi:facts::kRecordingCreationAbi) {
            const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),[&](const auto& e){return e.name==abi.name;});
            ASSERT_NE(entry,exports.entries.end());EXPECT_EQ(entry->ordinal,is64?abi.ordinal64:abi.ordinal32);
            EXPECT_EQ(entry->stdcall_bytes,abi.stdcall_bytes);
            cpu::CpuState cpu;
            if(is64){cpu.init_long_mode();cpu.gpr.rsp=0x4000;}
            else{cpu.init_protected_mode();cpu.set_sp(0x4000);for(u32 n=0;n<4;++n)(void)memory.write_u32(0x4004+n*4,0);}
            Marshaler m(memory);m.attach_cpu(cpu);set_last_error(0x12345678);entry->handler(m);
            EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            EXPECT_FALSE(mgr.has_dc(static_cast<Handle>(cpu.gpr.rax)));
        }
    }
}
