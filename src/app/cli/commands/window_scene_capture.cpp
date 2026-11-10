// SPDX-License-Identifier: LGPL-2.1-only
#include "window_scene_capture.hpp"
#include "runtime/process/runtime_context.hpp"
#include "windowing/model/window_scene.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_registry.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace vitreo::cli {
namespace {
std::optional<std::vector<Byte>> local_values(RuntimeContext& ctx,u64 generation) {
    const char* scope=std::getenv("VITREO_IPC_SCOPE");if(!scope || !*scope)return std::nullopt;
    windowing::WindowScene scene;
    scene.owner_host_pid=static_cast<u32>(::getpid());scene.owner_guest_pid=ipc::guest_process_id();
    scene.generation=generation;scene.capsule_scope=scope;
    scene.windows=ctx.hwnd_mgr.geometry_snapshot();
    std::sort(scene.windows.begin(),scene.windows.end(),[](const auto& a,const auto& b){return a.hwnd<b.hwnd;});
    std::unordered_set<u64> ids;for(const auto& g:scene.windows)ids.insert(g.hwnd);
    for(auto& [hwnd,mask]:ctx.u32_ext_mgr.window_region_snapshot())
        if(ids.contains(hwnd))scene.shapes.push_back({hwnd,{mask.left,mask.top,mask.right,mask.bottom},std::move(mask.rectangles)});
    scene.layers=ctx.u32_ext_mgr.layered_window_snapshot();
    std::erase_if(scene.layers,[&](const auto& layer){return !ids.contains(layer.first);});
    std::sort(scene.shapes.begin(),scene.shapes.end(),[](const auto& a,const auto& b){return a.hwnd<b.hwnd;});
    std::sort(scene.layers.begin(),scene.layers.end());
    return windowing::encode_window_scene(scene);
}
}
std::optional<std::vector<Byte>> capture_local_window_scene(RuntimeContext& ctx) {
    // Each owning lock is released before entering another owner. Compare two
    // complete local copies and the publication epoch, never outbound RPC here.
    if(!ipc::hwnd_registry_scene_publication_ok())return std::nullopt;
    for(int attempt=0;attempt<3;++attempt) {
        const auto generation=ipc::hwnd_registry_scene_generation();if(!generation)return std::nullopt;
        auto first=local_values(ctx,*generation),second=local_values(ctx,*generation);
        if(first && second && *first==*second && generation==ipc::hwnd_registry_scene_generation() &&
           ipc::hwnd_registry_scene_publication_ok())return first;
    }
    return std::nullopt;
}
winapi::GdiManager::SystemRegionState capture_system_region_scene(RuntimeContext& ctx) {
    using State=winapi::GdiManager::SystemRegionState;
    for(int attempt=0;attempt<3;++attempt) {
        const auto epoch=ipc::hwnd_registry_scene_generation();
        const auto order=ipc::hwnd_registry_child_order();
        const auto own_bytes=capture_local_window_scene(ctx);
        if(!epoch || !order || !own_bytes)continue;
        const auto own=windowing::decode_window_scene(*own_bytes);if(!own || own->generation!=*epoch)continue;
        const auto entries=ipc::hwnd_registry_enumerate();
        std::unordered_map<u64,u32> owners;std::unordered_map<u32,u32> processes;
        for(const auto& process:ipc::list_processes(own->capsule_scope))
            if(process.host_pid && process.pid)processes.emplace(process.host_pid,process.pid);
        for(const auto& e:entries)owners[e.hwnd]=e.owner_host_pid;
        std::unordered_set<u32> peers;
        for(const auto& [host,guest]:processes)if(host!=own->owner_host_pid)peers.insert(host);
        for(const auto& e:entries)if(e.owner_host_pid!=own->owner_host_pid)peers.insert(e.owner_host_pid);
        State state;state.has_snapshot=true;state.snapshot_complete=true;
        std::unordered_set<u64> seen;
        const auto append=[&](const windowing::WindowScene& scene,bool local) {
            for(const auto& g:scene.windows) {
                if(g.hwnd==windowing::HWND_DESKTOP && !local)continue;
                if(g.hwnd!=windowing::HWND_DESKTOP) {
                    const auto owner=owners.find(g.hwnd);
                    // A valid HWND is already owned during WM_CREATE, before its
                    // discovery record is published. Owner-local attestation is
                    // sufficient; an existing conflicting owner is not.
                    if(owner!=owners.end() && owner->second!=scene.owner_host_pid)return false;
                }
                if(!seen.insert(g.hwnd).second)return false;
                state.window_geometry.push_back(g);
            }
            for(const auto& mask:scene.shapes)if(local || mask.hwnd!=windowing::HWND_DESKTOP)
                state.window_shapes.emplace_back(mask.hwnd,winapi::ClipRect{mask.bounds[0],mask.bounds[1],mask.bounds[2],mask.bounds[3],true,mask.parts});
            for(const auto& layer:scene.layers)if(local || layer.first!=windowing::HWND_DESKTOP)state.layered_windows.push_back(layer);
            return true;
        };
        if(!append(*own,true))continue;
        bool complete=true;
        for(const auto pid:peers) {
            const auto bytes=ipc::remote_window_scene_snapshot(static_cast<pid_t>(pid));
            const auto peer=bytes?windowing::decode_window_scene(*bytes):std::nullopt;
            const auto expected=processes.find(pid);
            if(!peer || expected==processes.end() || peer->owner_guest_pid!=expected->second ||
               peer->owner_host_pid!=pid || peer->capsule_scope!=own->capsule_scope ||
               peer->generation!=*epoch || !append(*peer,false)){complete=false;break;}
        }
        if(!complete)continue;
        for(const auto& [hwnd,pid]:owners)if(!seen.contains(hwnd)){complete=false;break;}
        if(!complete)continue;
        std::unordered_map<u64,std::pair<u64,usize>> ranks;std::unordered_map<u64,usize> next;
        for(const auto& [parent,child]:order->parent_child)
            if(seen.contains(child))ranks.emplace(child,std::pair<u64,usize>{parent,next[parent]++});
        for(auto& g:state.window_geometry) {
            if(!(g.style&windowing::WS::CHILD) || g.parent==windowing::HWND_MESSAGE)continue;
            const auto rank=ranks.find(g.hwnd);
            if(rank==ranks.end() || rank->second.first!=g.parent || !seen.contains(g.parent)){complete=false;break;}
            g.z_order=rank->second.second;
        }
        const auto final_order=ipc::hwnd_registry_child_order();
        std::unordered_map<u32,u32> final_processes;
        for(const auto& process:ipc::list_processes(own->capsule_scope))
            if(process.host_pid && process.pid)final_processes.emplace(process.host_pid,process.pid);
        const auto final_own=capture_local_window_scene(ctx);
        if(complete && processes==final_processes && final_order && final_order->generation==order->generation &&
           epoch==ipc::hwnd_registry_scene_generation() && final_own && *final_own==*own_bytes)return state;
    }
    State failed;failed.has_snapshot=true;failed.snapshot_complete=false;return failed;
}
}
