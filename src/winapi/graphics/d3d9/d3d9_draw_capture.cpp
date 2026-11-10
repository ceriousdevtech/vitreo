// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_draw_capture.cpp
/// @brief One-frame capture of every programmable draw, for replay against
///        real Windows (tests/d3d9_oracle/d3d9_replay_runner.c).
///
/// Creating the marker file /tmp/vitreo-d3d9-capture arms it; the next whole
/// frame (Present to Present) is written to the directory the marker names
/// (one line of text), or /tmp/vitreo-d3d9-capture.d when it is empty. Off
/// unless the marker exists: the only cost when idle is one stat per Present.
///
/// Layout of the directory:
///   frame.txt     one block per draw (format below)
///   buf_<id>.bin  the guest contents of every vertex/index buffer referenced
///   vs_<id>.bin   the vertex shader token stream
///
///   D <n> <indexed> <prim> <base> <min_index> <num_vertices> <start>
///     <prim_count> <ib id> <ib format> <vs id>
///   E <stream> <offset> <type> <method> <usage> <usage_index>   (per element)
///   S <slot> <buffer id> <offset> <stride> <frequency>          (bound streams)
///   F <256*4 floats as hex words>   vertex shader float constants
///   I <16*4 ints>                   vertex shader int constants
///   R <state>=<value> ...           render states the guest has set
///   P <ps id>                       pixel shader (ps_<id>.bin)
///   T <stage>:<texture>:<format>    bound textures
///   B <16 bools>                    vertex shader bool constants
///
/// Only draws with a programmable vertex shader and a declaration are
/// captured: the replay compares vertex processing, the contract a
/// silhouette can judge. Draws from user-pointer memory are skipped.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr const char* kMarker = "/tmp/vitreo-d3d9-capture";

struct CaptureSession {
    bool active{false};
    // "offscreen <dir>" in the marker: keep recording across presents, but
    // only draws into 256x256..512x512 render targets (thumbnails, composited
    // textures), until kOffscreenDraws of them.
    bool offscreen{false};
    std::string dir;
    std::FILE* frame{nullptr};
    u32 draws{0};
    std::set<u32> buffers_written;
    std::set<u32> shaders_written;
    std::set<u32> textures_written;
};

CaptureSession& session() {
    static CaptureSession s;
    return s;
}

} // namespace

void D3D9Manager::draw_capture_on_present() {
    constexpr u32 kOffscreenDraws = 4000;
    CaptureSession& s = session();
    if (s.active && s.offscreen && s.draws < kOffscreenDraws) return;
    if (s.active) {
        std::fclose(s.frame);
        std::fprintf(stderr, "[d3d9-capture] wrote %u draws to %s\n", s.draws,
                     s.dir.c_str());
        s = CaptureSession{};
        return;
    }
    struct stat st{};
    if (::stat(kMarker, &st) != 0) return;
    std::string dir;
    if (std::FILE* marker = std::fopen(kMarker, "r")) {
        char line[512] = {};
        if (std::fgets(line, sizeof(line), marker) != nullptr) dir = line;
        std::fclose(marker);
    }
    std::remove(kMarker);
    while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r' || dir.back() == ' ')) {
        dir.pop_back();
    }
    bool offscreen = false;
    if (dir.rfind("offscreen ", 0) == 0) {
        offscreen = true;
        dir = dir.substr(10);
    }
    if (dir.empty()) dir = "/tmp/vitreo-d3d9-capture.d";
    ::mkdir(dir.c_str(), 0755);
    s.frame = std::fopen((dir + "/frame.txt").c_str(), "w");
    if (s.frame == nullptr) {
        std::fprintf(stderr, "[d3d9-capture] cannot write %s\n", dir.c_str());
        return;
    }
    s.dir = dir;
    s.active = true;
    s.offscreen = offscreen;
    std::fprintf(stderr, "[d3d9-capture] capturing the next frame into %s\n", dir.c_str());
}

void D3D9Manager::draw_capture_record(Marshaler& m, DeviceState& state, bool indexed,
                                      u32 primitive_type, i32 base_vertex, u32 min_index,
                                      u32 num_vertices, u32 index_start, u32 primitive_count) {
    CaptureSession& s = session();
    if (!s.active || state.vertex_shader == 0 || state.vertex_declaration == 0 ||
        state.up_override.has_value()) {
        return;
    }
    u32 rt_width = 0;
    u32 rt_height = 0;
    if (ComObject* rt = find(state.render_target_surface, D3D9InterfaceId::Surface);
        rt != nullptr && rt->surface) {
        rt_width = rt->surface->width;
        rt_height = rt->surface->height;
    }
    if (s.offscreen && (state.render_target_surface == state.back_buffer_surface ||
                        rt_width < 256 || rt_height < 256 || rt_width > 512 ||
                        rt_height > 512)) {
        return;
    }
    ComObject* vs = find(state.vertex_shader, D3D9InterfaceId::VertexShader);
    ComObject* decl = find(state.vertex_declaration, D3D9InterfaceId::VertexDeclaration);
    if (vs == nullptr || !vs->shader || decl == nullptr || !decl->vertex_declaration) return;

    const auto write_buffer = [&](u32 id) -> u32 {
        ComObject* entry = find(id, D3D9InterfaceId::VertexBuffer);
        if (entry == nullptr) entry = find(id, D3D9InterfaceId::IndexBuffer);
        if (entry == nullptr || !entry->buffer || entry->buffer->storage == 0) return 0;
        if (s.buffers_written.insert(id).second) {
            std::vector<u8> bytes(entry->buffer->length);
            if (m.memory().read_bytes(entry->buffer->storage, bytes.data(), bytes.size()).is_ok()) {
                char path[600];
                std::snprintf(path, sizeof(path), "%s/buf_%08x.bin", s.dir.c_str(), id);
                if (std::FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), f);
                    std::fclose(f);
                }
            }
        }
        return entry->buffer->index_buffer ? entry->buffer->fvf_or_format : 1u;
    };

    if (s.shaders_written.insert(state.vertex_shader).second) {
        char path[600];
        std::snprintf(path, sizeof(path), "%s/vs_%08x.bin", s.dir.c_str(), state.vertex_shader);
        if (std::FILE* f = std::fopen(path, "wb")) {
            std::fwrite(vs->shader->bytecode.data(), 4, vs->shader->bytecode.size(), f);
            std::fclose(f);
        }
    }
    u32 ib_format = 0;
    if (indexed && state.indices != 0) ib_format = write_buffer(state.indices);

    std::FILE* f = s.frame;
    std::fprintf(f, "D %u %u %u %d %u %u %u %u %08x %u %08x\n", s.draws++, indexed ? 1u : 0u,
                 primitive_type, base_vertex, min_index, num_vertices, index_start,
                 primitive_count, indexed ? state.indices : 0u, ib_format, state.vertex_shader);
    for (const auto& raw : decl->vertex_declaration->raw) {
        if (raw.stream == 0xFF) break;
        std::fprintf(f, "E %u %u %u %u %u %u\n", raw.stream, raw.offset, raw.type, raw.method,
                     raw.usage, raw.usage_index);
    }
    for (u32 slot = 0; slot < D3D9_MAX_STREAMS; ++slot) {
        const DeviceState::StreamSource& stream = state.streams[slot];
        if (stream.buffer == 0) continue;
        if (write_buffer(stream.buffer) == 0) continue;
        std::fprintf(f, "S %u %08x %u %u %08x\n", slot, stream.buffer, stream.offset,
                     stream.stride, stream.frequency);
    }
    std::fputc('F', f);
    for (const float value : state.vs_const_f) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        std::fprintf(f, " %08x", bits);
    }
    // Non-default render states, the pixel shader and the bound texture
    // formats: what decides the pixel side of the draw.
    std::fputc('\n', f);
    std::fputc('R', f);
    for (const auto& [rs, value] : state.render_states) {
        std::fprintf(f, " %u=%08x", rs, value);
    }
    std::fprintf(f, "\nP %08x", state.pixel_shader);
    if (state.pixel_shader != 0 && s.shaders_written.insert(state.pixel_shader).second) {
        if (ComObject* ps = find(state.pixel_shader, D3D9InterfaceId::PixelShader);
            ps != nullptr && ps->shader) {
            char path[600];
            std::snprintf(path, sizeof(path), "%s/ps_%08x.bin", s.dir.c_str(),
                          state.pixel_shader);
            if (std::FILE* pf = std::fopen(path, "wb")) {
                std::fwrite(ps->shader->bytecode.data(), 4, ps->shader->bytecode.size(), pf);
                std::fclose(pf);
            }
        }
    }
    std::fputs("\nT", f);
    for (u32 stage = 0; stage < D3D9_MAX_SAMPLERS; ++stage) {
        const u32 texture = state.textures[stage];
        if (texture == 0) continue;
        ComObject* t = find(texture, D3D9InterfaceId::Texture);
        if (t == nullptr) t = find(texture, D3D9InterfaceId::CubeTexture);
        if (t == nullptr) t = find(texture, D3D9InterfaceId::VolumeTexture);
        std::fprintf(f, " %u:%08x:%u", stage, texture,
                     t != nullptr && t->texture ? t->texture->format : 0u);
        // Level 0 of a 2D colour texture as the GPU holds it (tex_<id>.pam,
        // RGBA), once per texture.
        if (t != nullptr && t->texture && t->texture->backend &&
            t->texture->backend->valid() && s.textures_written.insert(texture).second) {
            const auto& desc = t->texture->backend->desc();
            if (!desc.cube && desc.array_size <= 1 && desc.width <= 4096 &&
                desc.height <= 4096 &&
                desc.pixel_format == graphics::PixelFormat::BGRA8Unorm) {
                std::vector<u8> bgra(static_cast<usize>(desc.width) * desc.height * 4u);
                d3d9_cs_drain();
                if (t->texture->backend->readback(0, bgra.data(), desc.width * 4u)) {
                    char path[600];
                    std::snprintf(path, sizeof(path), "%s/tex_%08x.pam", s.dir.c_str(), texture);
                    if (std::FILE* tf = std::fopen(path, "wb")) {
                        std::fprintf(tf, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\n"
                                     "TUPLTYPE RGB_ALPHA\nENDHDR\n", desc.width, desc.height);
                        for (usize i = 0; i + 3 < bgra.size(); i += 4) {
                            const u8 px[4] = {bgra[i + 2], bgra[i + 1], bgra[i], bgra[i + 3]};
                            std::fwrite(px, 1, 4, tf);
                        }
                        std::fclose(tf);
                    }
                }
            }
        }
    }
    // Pixel shader float constants c0-c7, as floats.
    std::fputs("\nG", f);
    for (usize i = 0; i < 32 && i < state.ps_const_f.size(); ++i) {
        std::fprintf(f, " %g", state.ps_const_f[i]);
    }
    std::fprintf(f, "\nO %08x %ux%u", state.render_target_surface, rt_width, rt_height);
    std::fputs("\nI", f);
    for (const i32 value : state.vs_const_i) std::fprintf(f, " %d", value);
    std::fputs("\nB", f);
    for (const u32 value : state.vs_const_b) std::fprintf(f, " %u", value);
    std::fputc('\n', f);
}

} // namespace vitreo::winapi
