// SPDX-License-Identifier: LGPL-2.1-only
#include <gtest/gtest.h>
#include "windowing/model/window_scene.hpp"
#include "ipc/hwnd_registry.hpp"
#include <filesystem>
#include <thread>
#include <cstdlib>
#include <unistd.h>
using namespace vitreo;

TEST(WindowSceneContract, CodecPreservesEveryValueAndRejectsMalformedFrames) {
    windowing::WindowScene scene;scene.owner_host_pid=42;scene.owner_guest_pid=84;
    scene.generation=0x123456789abcdef0;scene.capsule_scope="/capsule/scene";
    scene.windows.push_back({0x10001u,0x20002u,0x12345678,0x87654321,128,
        {-100,-200,300,400},{0,0,370,550},{10,20},true,false,true,0x123456789ull});
    scene.shapes.push_back({scene.windows[0].hwnd,{0,0,40,50},{{0,0,10,50},{20,0,40,50}}});
    scene.layers.emplace_back(scene.windows[0].hwnd,true);
    const auto bytes=windowing::encode_window_scene(scene);ASSERT_TRUE(bytes);
    const auto copy=windowing::decode_window_scene(*bytes);ASSERT_TRUE(copy);
    EXPECT_EQ(windowing::encode_window_scene(*copy),bytes);
    for(usize n=0;n<bytes->size();++n)EXPECT_FALSE(windowing::decode_window_scene(std::span<const Byte>(*bytes).first(n)));
    auto trailing=*bytes;trailing.push_back(Byte{});EXPECT_FALSE(windowing::decode_window_scene(trailing));
    auto corrupt=*bytes;corrupt[0]=Byte{};EXPECT_FALSE(windowing::decode_window_scene(corrupt));
    std::vector<Byte> huge(windowing::kWindowSceneMaxBytes+1);EXPECT_FALSE(windowing::decode_window_scene(huge));
    scene.windows.push_back(scene.windows[0]);const auto duplicate=windowing::encode_window_scene(scene);ASSERT_TRUE(duplicate);
    EXPECT_FALSE(windowing::decode_window_scene(*duplicate));
    scene.windows.pop_back();scene.shapes[0].parts.clear();scene.shapes[0].bounds={0,0,0,0};
    const auto empty=windowing::encode_window_scene(scene);ASSERT_TRUE(empty);
    const auto empty_copy=windowing::decode_window_scene(*empty);ASSERT_TRUE(empty_copy);
    ASSERT_EQ(empty_copy->shapes.size(),1u);EXPECT_TRUE(empty_copy->shapes[0].parts.empty());
    EXPECT_EQ(empty_copy->shapes[0].bounds,(std::array<i32,4>{0,0,0,0}));
}

namespace {
struct ChildOrderScope {
    std::string path,old;bool had_old=false;
    ChildOrderScope(){const auto prior=std::getenv("VITREO_IPC_SCOPE");had_old=prior;if(prior)old=prior;
        char name[]="/tmp/vitreo-child-order-XXXXXX";const auto directory=::mkdtemp(name);if(directory)path=directory;
        if(!path.empty())::setenv("VITREO_IPC_SCOPE",path.c_str(),1);}
    ~ChildOrderScope(){if(had_old)::setenv("VITREO_IPC_SCOPE",old.c_str(),1);else ::unsetenv("VITREO_IPC_SCOPE");
        if(!path.empty()){std::error_code ec;std::filesystem::remove_all(path,ec);}}
};
void bind_order(windowing::HwndManager& model) {
    model.set_child_order_observer([](const auto& event){
        using M=windowing::HwndManager::ChildOrderAction;using A=ipc::ChildOrderAction;
        auto action=A::Append;if(event.action==M::Remove)action=A::Remove;
        if(event.action==M::Reorder)action=event.after==windowing::HWND_TOP?A::First:
            event.after==windowing::HWND_BOTTOM?A::Last:A::After;
        EXPECT_TRUE(ipc::hwnd_registry_update_child_order(action,event.hwnd,event.parent,event.after));
    });
}
}
TEST(WindowSceneContract, CapsuleChildOrderingMergesOwnersAndForeignParents) {
    // Native system_region_peer_visible{32,64}.expected: firstchild is above
    // laterchild; TOP/BOTTOM and foreign-sibling placement change that order.
    ChildOrderScope scope;ASSERT_FALSE(scope.path.empty());
    windowing::HwndManager first,second;bind_order(first);bind_order(second);
    u64 next=0x50000;second.set_handle_allocator([&]{return next++;});
    windowing::WindowData parent;parent.rect={0,0,240,180};parent.client_rect=parent.rect;
    const auto p=first.create(parent);
    windowing::WindowData child;child.parent=p;child.style=windowing::WS::CHILD;
    const auto a=first.create(child),b=second.create(child);
    const auto expect=[&](std::vector<std::pair<u32,u32>> wanted){auto order=ipc::hwnd_registry_child_order();ASSERT_TRUE(order);EXPECT_EQ(order->parent_child,wanted);};
    expect({{p,a},{p,b}});
    ASSERT_TRUE(second.set_z_order(b,windowing::HWND_TOP));expect({{p,b},{p,a}});
    ASSERT_TRUE(first.set_z_order(a,b));expect({{p,b},{p,a}});
    ASSERT_TRUE(second.set_z_order(b,windowing::HWND_BOTTOM));expect({{p,a},{p,b}});
    ASSERT_TRUE(first.set_z_order(a,0x87654321));expect({{p,a},{p,b}});
    windowing::HWND previous=0;ASSERT_TRUE(second.set_parent(b,windowing::HWND_DESKTOP,previous));
    EXPECT_EQ(previous,p);expect({{p,a},{windowing::HWND_DESKTOP,b}});
    second.destroy(b);first.destroy(a);expect({});
    ASSERT_TRUE(ipc::hwnd_registry_scene_generation());EXPECT_GT(*ipc::hwnd_registry_scene_generation(),0u);
}
TEST(WindowSceneContract, ConcurrentPublishersDoNotLoseChildOrdering) {
    ChildOrderScope scope;ASSERT_FALSE(scope.path.empty());
    std::thread one([]{for(u32 i=0;i<20;++i)EXPECT_TRUE(ipc::hwnd_registry_update_child_order(ipc::ChildOrderAction::Append,100+i,42));});
    std::thread two([]{for(u32 i=0;i<20;++i)EXPECT_TRUE(ipc::hwnd_registry_update_child_order(ipc::ChildOrderAction::Append,200+i,42));});
    one.join();two.join();const auto order=ipc::hwnd_registry_child_order();ASSERT_TRUE(order);
    EXPECT_EQ(order->generation,40u);EXPECT_EQ(order->parent_child.size(),40u);
}

TEST(WindowSceneContract, PublicationFailurePoisonsOnlyItsCapsuleSnapshot) {
    {
        ChildOrderScope scope;ASSERT_FALSE(scope.path.empty());
        ASSERT_TRUE(ipc::hwnd_registry_scene_publication_ok());
        ASSERT_TRUE(ipc::hwnd_registry_update_child_order(ipc::ChildOrderAction::Append,100,42));
        const auto path=scope.path+"/runtime/hwnds/child-order";
        std::filesystem::rename(path,path+".saved");std::filesystem::create_directory(path);
        EXPECT_FALSE(ipc::hwnd_registry_update_child_order(ipc::ChildOrderAction::First,100,42));
        std::filesystem::remove(path);std::filesystem::rename(path+".saved",path);
        ASSERT_TRUE(ipc::hwnd_registry_child_order());
        // A valid stale file cannot silently restore snapshot trust.
        EXPECT_FALSE(ipc::hwnd_registry_scene_publication_ok());
    }
    {
        ChildOrderScope scope;ASSERT_FALSE(scope.path.empty());
        ASSERT_TRUE(ipc::hwnd_registry_scene_publication_ok());
        ASSERT_TRUE(ipc::hwnd_registry_update_child_order(ipc::ChildOrderAction::Append,100,42));
        const auto epoch=scope.path+"/runtime/hwnds/generation";
        std::filesystem::remove(epoch);std::filesystem::create_directory(epoch);
        EXPECT_FALSE(ipc::hwnd_registry_update_child_order(ipc::ChildOrderAction::First,100,42));
        EXPECT_FALSE(ipc::hwnd_registry_scene_publication_ok());
        EXPECT_FALSE(ipc::hwnd_registry_scene_generation());
    }
}

TEST(WindowSceneContract, GeometryCommitIsAtomicForSnapshotReaders) {
    windowing::HwndManager model;windowing::WindowData data;
    data.rect={0,0,10,10};data.client_rect={0,0,10,10};const auto hwnd=model.create(data);
    std::atomic<bool> done=false;
    std::thread writer([&]{for(i32 i=1;i<2000;++i) {
        const auto committed=model.update_geometry(hwnd,[&](auto& window) {
            window.rect.left=i;window.client_rect.right=i;
            window.visible=(i&1)!=0;window.style=window.visible?windowing::WS::VISIBLE:0;
        });
        EXPECT_TRUE(committed);
    }done=true;});
    do {
        for(const auto& g:model.geometry_snapshot())if(g.hwnd==hwnd) {
            // The initial row precedes either field change.
            if(g.rect.left)EXPECT_EQ(g.rect.left,g.client_rect.right);
            EXPECT_EQ(g.visible,(g.style&windowing::WS::VISIBLE)!=0);
        }
    }while(!done.load());
    writer.join();model.destroy(hwnd);
    EXPECT_FALSE(model.update_geometry(hwnd,[](auto&){}));
}
