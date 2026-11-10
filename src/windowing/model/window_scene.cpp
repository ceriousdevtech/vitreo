// SPDX-License-Identifier: LGPL-2.1-only
#include "window_scene.hpp"
#include <bit>
#include <limits>
#include <unordered_set>

namespace vitreo::windowing {
namespace {
constexpr u32 kSceneMagic=0x56575343,kSceneVersion=1;
struct Writer {
    std::vector<Byte> bytes;bool ok=true;
    void u32_value(u32 v){if(bytes.size()+4>kWindowSceneMaxBytes){ok=false;return;}for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<Byte>((v>>(i*8))&255));}
    void u64_value(u64 v){u32_value(static_cast<u32>(v));u32_value(static_cast<u32>(v>>32));}
    void integer(i32 v){u32_value(std::bit_cast<u32>(v));}
    void rect(const Rect& r){for(auto v:{r.left,r.top,r.right,r.bottom})integer(v);}
};
struct Reader {
    std::span<const Byte> bytes;usize at=0;bool ok=true;
    u32 u32_value(){if(bytes.size()-at<4){ok=false;return 0;}u32 v=0;for(unsigned i=0;i<4;++i)v|=static_cast<u32>(bytes[at++])<<(i*8);return v;}
    u64 u64_value(){const u64 lo=u32_value();const u64 hi=u32_value();return lo|(hi<<32);}
    i32 integer(){return std::bit_cast<i32>(u32_value());}
    Rect rect(){const auto l=integer(),t=integer(),r=integer(),b=integer();return {l,t,r,b};}
    u32 count(){const auto n=u32_value();if(n>65536 || n>bytes.size()-at)ok=false;return ok?n:0;}
};
}
std::optional<std::vector<Byte>> encode_window_scene(const WindowScene& scene) {
    if(scene.capsule_scope.size()>4096 || scene.windows.size()>65536 || scene.shapes.size()>65536 || scene.layers.size()>65536)return std::nullopt;
    Writer w;w.u32_value(kSceneMagic);w.u32_value(kSceneVersion);
    w.u32_value(scene.owner_host_pid);w.u32_value(scene.owner_guest_pid);w.u64_value(scene.generation);
    w.u32_value(static_cast<u32>(scene.capsule_scope.size()));
    for(const auto c:scene.capsule_scope)w.bytes.push_back(static_cast<Byte>(static_cast<unsigned char>(c)));
    w.u32_value(static_cast<u32>(scene.windows.size()));
    for(const auto& g:scene.windows) {
        w.u64_value(g.hwnd);w.u64_value(g.parent);w.u32_value(g.style);w.u32_value(g.ex_style);w.u32_value(g.class_style);
        w.rect(g.rect);w.rect(g.client_rect);w.integer(g.client_offset.x);w.integer(g.client_offset.y);
        w.u32_value((g.visible?1u:0u)|(g.minimized?2u:0u)|(g.hidden_by_owner?4u:0u));w.u64_value(g.z_order);
        if(!w.ok)return std::nullopt;
    }
    w.u32_value(static_cast<u32>(scene.shapes.size()));
    for(const auto& s:scene.shapes) {
        if(s.parts.size()>65536)return std::nullopt;
        w.u64_value(s.hwnd);for(auto v:s.bounds)w.integer(v);w.u32_value(static_cast<u32>(s.parts.size()));
        for(const auto& part:s.parts) {for(auto v:part)w.integer(v);if(!w.ok)return std::nullopt;}
    }
    w.u32_value(static_cast<u32>(scene.layers.size()));
    for(const auto& [hwnd,pixels]:scene.layers){w.u64_value(hwnd);w.u32_value(pixels?1:0);}
    if(!w.ok)return std::nullopt;return std::move(w.bytes);
}
std::optional<WindowScene> decode_window_scene(std::span<const Byte> bytes) {
    if(bytes.size()>kWindowSceneMaxBytes)return std::nullopt;
    Reader r{bytes};if(r.u32_value()!=kSceneMagic || r.u32_value()!=kSceneVersion)return std::nullopt;
    WindowScene scene;scene.owner_host_pid=r.u32_value();scene.owner_guest_pid=r.u32_value();scene.generation=r.u64_value();
    const auto length=r.u32_value();if(length>4096 || length>bytes.size()-r.at)return std::nullopt;
    for(u32 i=0;i<length;++i)scene.capsule_scope.push_back(static_cast<char>(bytes[r.at++]));
    std::unordered_set<u64> ids;
    const auto windows=r.count();
    for(u32 i=0;i<windows && r.ok;++i) {
        HwndManager::Geometry g{};const auto hwnd=r.u64_value(),parent=r.u64_value();
        if(hwnd>std::numeric_limits<HWND>::max() || parent>std::numeric_limits<HWND>::max())return std::nullopt;
        g.hwnd=static_cast<HWND>(hwnd);g.parent=static_cast<HWND>(parent);
        g.style=r.u32_value();g.ex_style=r.u32_value();g.class_style=r.u32_value();
        g.rect=r.rect();g.client_rect=r.rect();g.client_offset.x=r.integer();g.client_offset.y=r.integer();
        const auto flags=r.u32_value();if(flags&~7u)r.ok=false;
        g.visible=(flags&1)!=0;g.minimized=(flags&2)!=0;g.hidden_by_owner=(flags&4)!=0;g.z_order=r.u64_value();
        if(!g.hwnd || !ids.insert(g.hwnd).second)return std::nullopt;
        scene.windows.push_back(g);
    }
    std::unordered_set<u64> masked;
    const auto shapes=r.count();
    for(u32 i=0;i<shapes && r.ok;++i) {
        WindowScene::Shape shape{};shape.hwnd=r.u64_value();for(auto& v:shape.bounds)v=r.integer();
        if(!ids.contains(shape.hwnd) || !masked.insert(shape.hwnd).second)return std::nullopt;
        const auto parts=r.count();if(parts>(bytes.size()-r.at)/16)return std::nullopt;
        for(u32 j=0;j<parts && r.ok;++j){std::array<i32,4> part;for(auto& v:part)v=r.integer();shape.parts.push_back(part);}
        scene.shapes.push_back(std::move(shape));
    }
    std::unordered_set<u64> layered;
    const auto layers=r.count();
    for(u32 i=0;i<layers && r.ok;++i) {
        const auto hwnd=r.u64_value();const auto pixels=r.u32_value();
        if(pixels>1 || !ids.contains(hwnd) || !layered.insert(hwnd).second)return std::nullopt;
        scene.layers.emplace_back(hwnd,pixels!=0);
    }
    if(!r.ok || r.at!=bytes.size() || !scene.owner_host_pid || !scene.owner_guest_pid)return std::nullopt;
    return scene;
}
}
