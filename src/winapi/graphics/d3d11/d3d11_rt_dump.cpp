/// @file d3d11_rt_dump.cpp
/// @brief Diagnostic: read back every render target / depth / UAV texture.
///
/// A wrong image (a scene washed to fog colour, a magenta block) comes from
/// one pass writing wrong data, and nothing on the host says which. Creating
/// the marker file /tmp/vitreo-rt-dump makes the next Present (checked every
/// 30) write each render-target, depth-stencil and UAV texture of at least
/// 64x64 to $HOME/.vitreo/rt-dump/<pid>/ as a PPM, plus one line of
/// per-channel min / max / mean / NaN-count in stats.txt. Floats are clamped
/// to [0,1] for the image; the stats carry the real values. The marker is
/// removed once the dump is written. App-agnostic: it reads resources, it
/// never interprets them.
///
/// The first dump also arms a per-target draw log: from then on each target
/// keeps every distinct (VS, GS, PS) that drew into it, with the t# views
/// (typed-buffer element biases included) and b# slices its first draw
/// bound, and the next dump lists them under the target and saves each
/// shader's DXBC once. Until a dump arms it a draw pays one branch per
/// target.
///
/// A target that is also a shader resource and has mips (a virtual
/// texture's page table: R16_UINT, 13 mips) gets one stats line per mip;
/// R16_UINT and R32_UINT decode as their integer values (distinct-value
/// counts included), and their image shows value / max.
///
/// The first dump opens <dump dir>/transfers.txt at its start, before any
/// image (or, when it cannot be opened, logs to stderr and says why in
/// stats.txt): a readback trace, one "[d3d11-xfer] frame=<presents> <event>"
/// line per copy into STAGING, EVENT query End and GetData, Map of STAGING
/// (HRESULT, DO_NOT_WAIT, frames since its copy, first 64 bytes), UAV clear
/// of a buffer, OM u# binding, and copy / update into a texture larger than
/// 4096 x 4096 -- the GPU -> CPU -> GPU loop of a virtual texture's page
/// feedback and of a grass map, event by event (diag_log_*). Until then each
/// of those calls pays one branch.
///
/// Images are at most 4096 texels on a side (a larger level is drawn from
/// every n-th texel, "_every<n>" in its name), and a colour level is kept as
/// its bytes: the dump of three 9288^2 pools wrote 259 MB images and held
/// 1.7 GB per pool, and every image after the second pool failed to open.
/// A failed or short image names its error in stats.txt.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

namespace {

float half_to_float(u16 h) {
    const u32 sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1Fu,
              mantissa = h & 0x3FFu;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u),
                           static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

float unsigned_small_float(u32 bits, u32 mantissa_bits) {
    const u32 exponent = bits >> mantissa_bits;
    const u32 mantissa = bits & ((1u << mantissa_bits) - 1u);
    if (exponent == 0) {
        return std::ldexp(static_cast<float>(mantissa),
                          -14 - static_cast<int>(mantissa_bits));
    }
    if (exponent == 31) return mantissa ? NAN : INFINITY;
    return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)),
                      static_cast<int>(exponent) - 15 -
                          static_cast<int>(mantissa_bits));
}

/// Decode one texel of @p dxgi_format to up to four floats; false if the
/// format is not decoded here.
bool decode_texel(u32 dxgi_format, const u8* p, float out[4], int& channels) {
    u32 w32;
    switch (dxgi_format) {
    case 27: case 28: case 29: case 30:  // R8G8B8A8
        for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.0f;
        channels = 4; return true;
    case 87: case 90: case 91:  // B8G8R8A8
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f;
        out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        channels = 4; return true;
    case 23: case 24: case 25:  // R10G10B10A2
        std::memcpy(&w32, p, 4);
        out[0] = (w32 & 0x3FF) / 1023.0f; out[1] = ((w32 >> 10) & 0x3FF) / 1023.0f;
        out[2] = ((w32 >> 20) & 0x3FF) / 1023.0f; out[3] = (w32 >> 30) / 3.0f;
        channels = 4; return true;
    case 26:  // R11G11B10_FLOAT
        std::memcpy(&w32, p, 4);
        out[0] = unsigned_small_float(w32 & 0x7FF, 6);
        out[1] = unsigned_small_float((w32 >> 11) & 0x7FF, 6);
        out[2] = unsigned_small_float(w32 >> 22, 5);
        channels = 3; return true;
    case 9: case 10: {  // R16G16B16A16_FLOAT
        u16 h[4]; std::memcpy(h, p, 8);
        for (int i = 0; i < 4; ++i) out[i] = half_to_float(h[i]);
        channels = 4; return true;
    }
    case 33: case 34: {  // R16G16_FLOAT
        u16 h[2]; std::memcpy(h, p, 4);
        out[0] = half_to_float(h[0]); out[1] = half_to_float(h[1]);
        channels = 2; return true;
    }
    case 53: case 54: {  // R16_FLOAT
        u16 h; std::memcpy(&h, p, 2); out[0] = half_to_float(h);
        channels = 1; return true;
    }
    case 1: case 2: std::memcpy(out, p, 16); channels = 4; return true;  // RGBA32F
    case 15: case 16: std::memcpy(out, p, 8); channels = 2; return true;  // RG32F
    case 39: case 41: std::memcpy(out, p, 4); channels = 1; return true;  // R32F
    case 61: case 62: out[0] = p[0] / 255.0f; channels = 1; return true;  // R8
    case 57: {  // R16_UINT: the integer value
        u16 v; std::memcpy(&v, p, 2); out[0] = static_cast<float>(v);
        channels = 1; return true;
    }
    case 42: {  // R32_UINT: the integer value (exact below 2^24)
        std::memcpy(&w32, p, 4); out[0] = static_cast<float>(w32);
        channels = 1; return true;
    }
    default: return false;
    }
}

/// R16_UINT / R32_UINT: decode_texel gives the integer value, and the
/// distinct values are worth counting (a page table's entries).
bool is_uint_format(u32 f) { return f == 57 || f == 42; }

/// The raw integer of one R16_UINT / R32_UINT texel.
u32 uint_texel(u32 dxgi_format, const u8* p) {
    if (dxgi_format == 57) {
        u16 v; std::memcpy(&v, p, 2); return v;
    }
    u32 v; std::memcpy(&v, p, 4); return v;
}

bool is_depth_format(u32 f) {
    // D32_FLOAT_S8X24 family, D32_FLOAT, D24_UNORM_S8 family, D16_UNORM.
    return f == 19 || f == 20 || f == 21 || f == 22 || f == 39 || f == 40 ||
           f == 44 || f == 45 || f == 46 || f == 47 || f == 53 || f == 55;
}

}  // namespace

void D3D11Manager::note_diag_draw_pair(Texture2D& texture,
                                       const ContextState& state) {
    for (auto& pair : texture.diag_draw_pairs) {
        if (pair.vs == state.vertex_shader && pair.gs == state.geometry_shader &&
            pair.ps == state.pixel_shader) {
            ++pair.draws;
            return;
        }
    }
    constexpr usize kMaxPairs = 64;
    if (texture.diag_draw_pairs.size() >= kMaxPairs) {
        ++texture.diag_draw_pairs_dropped;
        return;
    }
    DiagDrawPair pair{};
    pair.vs = state.vertex_shader;
    pair.gs = state.geometry_shader;
    pair.ps = state.pixel_shader;
    pair.draws = 1;
    std::string& text = pair.bindings;
    char line[256];
    const auto views = [&](const char* stage,
                           std::span<const GuestAddr> bound) {
        for (usize slot = 0; slot < bound.size(); ++slot) {
            if (bound[slot] == 0) continue;
            const ShaderResourceView* srv = find_shader_resource_view(bound[slot]);
            if (srv == nullptr) {
                std::snprintf(line, sizeof(line), " %s.t%zu=?0x%llx", stage,
                              slot, static_cast<unsigned long long>(bound[slot]));
            } else if (srv->buffer != 0) {
                std::snprintf(
                    line, sizeof(line),
                    " %s.t%zu=buf 0x%llx fmt=%u first=%u n=%u bias=%u", stage,
                    slot, static_cast<unsigned long long>(srv->buffer),
                    srv->format, srv->first_element, srv->num_elements,
                    srv->texture_buffer ? srv->texture_buffer->element_bias()
                                        : 0u);
            } else {
                std::snprintf(line, sizeof(line),
                              " %s.t%zu=tex 0x%llx fmt=%u dim=%u mips=%u+%u "
                              "slices=%u+%u",
                              stage, slot,
                              static_cast<unsigned long long>(srv->texture),
                              srv->format, srv->view_dimension,
                              srv->most_detailed_mip, srv->mip_levels,
                              srv->first_array_slice, srv->array_size);
            }
            text += line;
        }
    };
    const auto slices = [&](const char* stage,
                            std::span<const GuestAddr> buffers,
                            std::span<const u32> first,
                            std::span<const u32> count) {
        for (usize slot = 0; slot < buffers.size(); ++slot) {
            if (buffers[slot] == 0) continue;
            std::snprintf(line, sizeof(line), " %s.b%zu=0x%llx@%u+%u", stage,
                          slot, static_cast<unsigned long long>(buffers[slot]),
                          first[slot], count[slot]);
            text += line;
        }
    };
    views("vs", state.vertex_shader_resources);
    slices("vs", state.vertex_constant_buffers, state.vertex_constant_first,
           state.vertex_constant_count);
    views("ps", state.pixel_shader_resources);
    slices("ps", state.pixel_constant_buffers, state.pixel_constant_first,
           state.pixel_constant_count);
    texture.diag_draw_pairs.push_back(std::move(pair));
}

void D3D11Manager::dump_render_targets_if_requested() {
    static unsigned presents = 0;
    if (++presents % 30 != 0) return;
    // /tmp/vitreo-rt-dump.<pid> targets one process; the bare name is taken
    // by whichever process presents first (another process in the session
    // presents too).
    const std::string own = "/tmp/vitreo-rt-dump." + std::to_string(::getpid());
    struct stat st{};
    const char* marker = nullptr;
    if (::stat(own.c_str(), &st) == 0) {
        marker = own.c_str();
    } else if (::stat("/tmp/vitreo-rt-dump", &st) == 0) {
        marker = "/tmp/vitreo-rt-dump";
    } else {
        return;
    }
    ::unlink(marker);

    const char* home = std::getenv("HOME");
    const std::string root = std::string(home ? home : "/tmp") + "/.vitreo";
    ::mkdir(root.c_str(), 0755);
    ::mkdir((root + "/rt-dump").c_str(), 0755);
    (void)dump_render_targets_to(root + "/rt-dump/" +
                                 std::to_string(::getpid()));
}

bool D3D11Manager::dump_render_targets_to(const std::string& dir) {
    ::mkdir(dir.c_str(), 0755);
    std::FILE* stats = std::fopen((dir + "/stats.txt").c_str(), "w");
    if (stats == nullptr) {
        const int error = errno;
        std::fprintf(stderr,
                     "[d3d11] render-target dump -> %s: stats.txt not "
                     "created: %s\n",
                     dir.c_str(), std::strerror(error));
        return false;
    }
    std::fprintf(stderr, "[d3d11] render-target dump -> %s\n", dir.c_str());
    // First, before the images: the dump writes hundreds of MB of them, and
    // anything opened after a full disk or a memory-pressure failure is lost
    // with them (2026-10-20: every image after the second 9288^2 pool, and
    // the transfer log opened last, were missing from all three dumps).
    open_diag_transfer_log(dir, stats);

    // Every (VS, GS, PS) that drew into a target since the previous dump
    // (note_diag_draw_pair), with what its first draw bound; each shader's
    // DXBC is saved once per dump as shader_<stage>_<address>.dxbc.
    std::set<GuestAddr> saved_shaders;
    const auto save_shader = [&](GuestAddr address, const char* stage) {
        if (address == 0 || !saved_shaders.insert(address).second) return;
        const std::vector<u8>* bytecode = nullptr;
        if (std::strcmp(stage, "vs") == 0) {
            const auto it = m_vertex_shaders.find(address);
            if (it != m_vertex_shaders.end()) bytecode = &it->second.bytecode;
        } else if (std::strcmp(stage, "gs") == 0) {
            const auto it = m_geometry_shaders.find(address);
            if (it != m_geometry_shaders.end()) bytecode = &it->second.bytecode;
        } else {
            const auto it = m_pixel_shaders.find(address);
            if (it != m_pixel_shaders.end()) bytecode = &it->second.bytecode;
        }
        if (bytecode == nullptr || bytecode->empty()) return;
        char name[96];
        std::snprintf(name, sizeof(name), "/shader_%s_%llx.dxbc", stage,
                      static_cast<unsigned long long>(address));
        if (std::FILE* blob = std::fopen((dir + name).c_str(), "wb")) {
            std::fwrite(bytecode->data(), 1, bytecode->size(), blob);
            std::fclose(blob);
        }
    };
    const auto print_pairs = [&](const Texture2D& texture) {
        if (!m_diag_draw_pairs_armed ||
            (texture.diag_draw_pairs.empty() &&
             texture.diag_draw_pairs_dropped == 0)) {
            return;
        }
        std::fprintf(stats,
                     "    draws since the previous dump: %zu combinations "
                     "(+%u not kept)\n",
                     texture.diag_draw_pairs.size(),
                     texture.diag_draw_pairs_dropped);
        for (const DiagDrawPair& pair : texture.diag_draw_pairs) {
            std::fprintf(stats, "      vs 0x%llx gs 0x%llx ps 0x%llx draws=%u%s\n",
                         static_cast<unsigned long long>(pair.vs),
                         static_cast<unsigned long long>(pair.gs),
                         static_cast<unsigned long long>(pair.ps), pair.draws,
                         pair.bindings.c_str());
            save_shader(pair.vs, "vs");
            save_shader(pair.gs, "gs");
            save_shader(pair.ps, "ps");
        }
    };

    // Every mip past 0 of a target that is also sampled -- a virtual
    // texture's page table is rendered mip by mip and read with `ld` at the
    // mip its derivatives pick -- one stats line each, read a mip at a time.
    const auto print_mips = [&](const Texture2D& texture) {
        const auto& d = texture.desc;
        if (texture.volume || !texture.backend || d.mip_levels <= 1 ||
            d.sample_count > 1 ||
            (d.bind_flags & D3D11_BIND_RENDER_TARGET) == 0 ||
            (d.bind_flags & D3D11_BIND_SHADER_RESOURCE) == 0) {
            return;
        }
        const u32 bpp = graphics::format_table(
                            texture.backend->desc().pixel_format)
                            .bytes_per_pixel;
        float probe[4];
        int channels = 0;
        u8 zero[16] = {};
        if (bpp == 0 || is_depth_format(d.format) ||
            !decode_texel(d.format, zero, probe, channels)) {
            std::fprintf(stats, "    mips 1..%u: fmt=%u not decoded\n",
                         d.mip_levels - 1, d.format);
            return;
        }
        const bool uint = is_uint_format(d.format);
        std::vector<u8> raw;
        std::unordered_set<u32> distinct;
        constexpr usize kMaxDistinct = 65536;
        for (u32 mip = 1; mip < d.mip_levels; ++mip) {
            const u32 w = std::max(1u, d.width >> mip);
            const u32 h = std::max(1u, d.height >> mip);
            raw.assign(static_cast<usize>(w) * h * bpp, 0);
            if (!texture.backend->readback(mip, raw.data(), w * bpp)) {
                std::fprintf(stats, "    mip %u %ux%u: not read\n", mip, w, h);
                continue;
            }
            float mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
            float mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
            double mean[4] = {};
            u64 nans = 0;
            distinct.clear();
            const usize count = static_cast<usize>(w) * h;
            for (usize i = 0; i < count; ++i) {
                const u8* texel = raw.data() + i * bpp;
                float t[4] = {};
                int c = 0;
                (void)decode_texel(d.format, texel, t, c);
                for (int k = 0; k < channels; ++k) {
                    if (std::isnan(t[k])) { ++nans; continue; }
                    mn[k] = std::min(mn[k], t[k]);
                    mx[k] = std::max(mx[k], t[k]);
                    mean[k] += t[k];
                }
                if (uint && distinct.size() < kMaxDistinct) {
                    distinct.insert(uint_texel(d.format, texel));
                }
            }
            std::fprintf(stats, "    mip %u %ux%u nan=%llu", mip, w, h,
                         static_cast<unsigned long long>(nans));
            for (int k = 0; k < channels; ++k) {
                std::fprintf(stats, " | c%d min=%g max=%g mean=%g", k, mn[k],
                             mx[k], mean[k] / static_cast<double>(count));
            }
            if (uint) {
                std::fprintf(stats, " distinct=%zu%s", distinct.size(),
                             distinct.size() >= kMaxDistinct ? "+" : "");
            }
            std::fprintf(stats, "\n");
        }
    };

    unsigned index = 0;
    // Volume textures: the middle W slice of mip 0, with the same stats. A
    // colour LUT (UE's tonemapper) lives here, and an unwritten one tints
    // every tonemapped pixel.
    for (auto& [address, texture] : m_textures) {
        if (!texture.volume || !texture.backend_3d) continue;
        const auto& vd = texture.volume_desc;
        const u32 bpp = graphics::format_table(
                            texture.backend_3d->desc().pixel_format)
                            .bytes_per_pixel;
        float probe[4];
        int channels = 0;
        u8 zero[16] = {};
        if (bpp == 0 || !decode_texel(vd.format, zero, probe, channels)) {
            std::fprintf(stats, "#%u 0x%llx VOLUME fmt=%u %ux%ux%u: not decoded\n",
                         index++, static_cast<unsigned long long>(address),
                         vd.format, vd.width, vd.height, vd.depth);
            continue;
        }
        std::vector<u8> raw(static_cast<usize>(vd.width) * vd.height * bpp);
        const bool ok = texture.backend_3d->readback_w_slice(
            0, vd.depth / 2, raw.data(), vd.width * bpp);
        std::fprintf(stats,
                     "#%u 0x%llx VOLUME fmt=%u %ux%ux%u bind=0x%x "
                     "writes[draw=%u clear=%u copy=%u uav=%u uavdrop=%u]",
                     index++, static_cast<unsigned long long>(address),
                     vd.format, vd.width, vd.height, vd.depth, vd.bind_flags,
                     texture.diag_rt_draws, texture.diag_clears,
                     texture.diag_copy_dst, texture.diag_uav_binds,
                     texture.diag_uav_drops);
        if (!ok) { std::fprintf(stats, " not read\n"); continue; }
        float mn[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
        float mx[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
        double mean[4] = {};
        u64 nans = 0;
        const usize count = static_cast<usize>(vd.width) * vd.height;
        for (usize i = 0; i < count; ++i) {
            float t[4] = {};
            int c = 0;
            (void)decode_texel(vd.format, raw.data() + i * bpp, t, c);
            for (int k = 0; k < channels; ++k) {
                if (std::isnan(t[k])) { ++nans; continue; }
                mn[k] = std::min(mn[k], t[k]); mx[k] = std::max(mx[k], t[k]);
                mean[k] += t[k];
            }
        }
        std::fprintf(stats, " nan=%llu", static_cast<unsigned long long>(nans));
        for (int k = 0; k < channels; ++k) {
            std::fprintf(stats, " | c%d min=%g max=%g mean=%g", k, mn[k], mx[k],
                         mean[k] / static_cast<double>(count));
        }
        // Texels holding any non-zero byte, per W slice: tells "wrote the
        // wrong slices" apart from "wrote zeros".
        std::fprintf(stats, "\n    nonzero-per-slice:");
        for (u32 z = 0; z < vd.depth; ++z) {
            if (!texture.backend_3d->readback_w_slice(0, z, raw.data(),
                                                      vd.width * bpp)) {
                std::fprintf(stats, " ?");
                continue;
            }
            usize nonzero = 0;
            for (usize i = 0; i < count; ++i) {
                for (u32 b = 0; b < bpp; ++b) {
                    if (raw[i * bpp + b] != 0) { ++nonzero; break; }
                }
            }
            std::fprintf(stats, " %zu", nonzero);
        }
        std::fprintf(stats, "\n");
        if (texture.diag_uav_writer != 0) {
            const std::vector<u8>* bytecode = nullptr;
            if (texture.diag_uav_writer_compute) {
                const auto cs = m_compute_shaders.find(texture.diag_uav_writer);
                if (cs != m_compute_shaders.end()) bytecode = &cs->second.bytecode;
            } else {
                const auto ps = m_pixel_shaders.find(texture.diag_uav_writer);
                if (ps != m_pixel_shaders.end()) bytecode = &ps->second.bytecode;
            }
            if (bytecode != nullptr && !bytecode->empty()) {
                char name[64];
                std::snprintf(name, sizeof(name), "/vol%u_writer_%s.dxbc",
                              index - 1,
                              texture.diag_uav_writer_compute ? "cs" : "ps");
                if (std::FILE* blob = std::fopen((dir + name).c_str(), "wb")) {
                    std::fwrite(bytecode->data(), 1, bytecode->size(), blob);
                    std::fclose(blob);
                }
                std::fprintf(stats, "    writer %s 0x%llx -> %s\n",
                             texture.diag_uav_writer_compute ? "cs" : "ps",
                             static_cast<unsigned long long>(texture.diag_uav_writer),
                             name + 1);
            }
        }
        // The last draw that rendered into it: its VS, GS (which routes
        // each primitive to a W slice through SV_RenderTargetArrayIndex)
        // and PS, as for a 2D target's last-ps, for an offline replay.
        if (texture.diag_rt_draws != 0) {
            const std::vector<u8>* stages[3] = {nullptr, nullptr, nullptr};
            const GuestAddr shaders[3] = {texture.diag_rt_writer_vs,
                                          texture.diag_rt_writer_gs,
                                          texture.diag_rt_writer};
            if (const auto vs = m_vertex_shaders.find(shaders[0]);
                vs != m_vertex_shaders.end()) {
                stages[0] = &vs->second.bytecode;
            }
            if (const auto gs = m_geometry_shaders.find(shaders[1]);
                gs != m_geometry_shaders.end()) {
                stages[1] = &gs->second.bytecode;
            }
            if (const auto ps = m_pixel_shaders.find(shaders[2]);
                ps != m_pixel_shaders.end()) {
                stages[2] = &ps->second.bytecode;
            }
            static constexpr const char* kStage[3] = {"vs", "gs", "ps"};
            std::fprintf(stats, "    draw-writer");
            for (int s = 0; s < 3; ++s) {
                if (stages[s] == nullptr || stages[s]->empty()) {
                    std::fprintf(stats, " %s 0x%llx", kStage[s],
                                 static_cast<unsigned long long>(shaders[s]));
                    continue;
                }
                char name[64];
                std::snprintf(name, sizeof(name), "/vol%u_draw_writer_%s.dxbc",
                              index - 1, kStage[s]);
                if (std::FILE* blob = std::fopen((dir + name).c_str(), "wb")) {
                    std::fwrite(stages[s]->data(), 1, stages[s]->size(), blob);
                    std::fclose(blob);
                }
                std::fprintf(stats, " %s 0x%llx -> %s", kStage[s],
                             static_cast<unsigned long long>(shaders[s]),
                             name + 1);
            }
            std::fprintf(stats, "\n");
        }
        print_pairs(texture);
    }
    for (auto& [address, texture] : m_textures) {
        const auto& d = texture.desc;
        constexpr u32 kInteresting = D3D11_BIND_RENDER_TARGET |
                                     D3D11_BIND_DEPTH_STENCIL |
                                     D3D11_BIND_UNORDERED_ACCESS;
        if (!texture.backend || (d.bind_flags & kInteresting) == 0 ||
            d.sample_count > 1) {
            continue;
        }
        // Small targets (UE's 1x1 eye-adaptation exposure, luminance
        // histograms) get stats and their texel values, but no image.
        const bool tiny = d.width < 64 || d.height < 64;
        const u32 w = d.width, h = d.height;
        const bool depth = (d.bind_flags & D3D11_BIND_DEPTH_STENCIL) != 0 ||
                           is_depth_format(d.format);
        // Colour texels stay as the bytes read back and are decoded where
        // used; depth is one float per texel. (Four floats per texel cost
        // 1.4 GB for a 9288^2 pool, on top of its bytes.)
        std::vector<u8> raw;
        std::vector<float> plane;
        u32 bpp = 0;
        int channels = 1;
        bool ok = false;
        const graphics::PixelFormat host_format =
            texture.backend->desc().pixel_format;
        if (depth && (host_format == graphics::PixelFormat::D16Unorm ||
                      host_format == graphics::PixelFormat::D32Float)) {
            // A depth-only format blits like a colour one (no plane to name).
            const bool d16 = host_format == graphics::PixelFormat::D16Unorm;
            const u32 depth_bpp = d16 ? 2u : 4u;
            raw.resize(static_cast<usize>(w) * h * depth_bpp);
            ok = texture.backend->readback(0, raw.data(), w * depth_bpp);
            plane.resize(ok ? static_cast<usize>(w) * h : 0u);
            for (usize i = 0; ok && i < plane.size(); ++i) {
                if (d16) {
                    u16 v = 0;
                    std::memcpy(&v, raw.data() + i * 2, 2);
                    plane[i] = static_cast<float>(v) / 65535.0f;
                } else {
                    std::memcpy(&plane[i], raw.data() + i * 4, 4);
                }
            }
            std::vector<u8>().swap(raw);
        } else if (depth) {
            plane.resize(static_cast<usize>(w) * h);
            ok = texture.backend->readback_depth_stencil_plane(
                0, 0, false, plane.data(), w * sizeof(float));
        } else {
            bpp = graphics::format_table(texture.backend->desc().pixel_format)
                      .bytes_per_pixel;
            float probe[4];
            int ch = 0;
            u8 zero[16] = {};
            if (bpp != 0 && decode_texel(d.format, zero, probe, ch)) {
                raw.resize(static_cast<usize>(w) * h * bpp);
                ok = texture.backend->readback(0, raw.data(), w * bpp);
                channels = ch;
            }
        }
        const auto texel_at = [&](usize i, float t[4]) {
            t[0] = t[1] = t[2] = t[3] = 0.0f;
            if (depth) {
                t[0] = plane[i];
                return;
            }
            int c = 0;
            (void)decode_texel(d.format, raw.data() + i * bpp, t, c);
        };
        if (!ok) {
            std::fprintf(stats, "#%u 0x%llx fmt=%u %ux%u bind=0x%x: not read\n",
                         index++, static_cast<unsigned long long>(address),
                         d.format, w, h, d.bind_flags);
            print_pairs(texture);
            continue;
        }
        float mn[4], mx[4];
        double mean[4] = {};
        u64 nans = 0;
        for (int c = 0; c < 4; ++c) { mn[c] = INFINITY; mx[c] = -INFINITY; }
        const usize count = static_cast<usize>(w) * h;
        for (usize i = 0; i < count; ++i) {
            float t[4];
            texel_at(i, t);
            for (int c = 0; c < channels; ++c) {
                const float v = t[c];
                if (std::isnan(v)) { ++nans; continue; }
                mn[c] = std::min(mn[c], v); mx[c] = std::max(mx[c], v);
                mean[c] += v;
            }
        }
        std::fprintf(stats,
                     "#%u 0x%llx fmt=%u %ux%u bind=0x%x%s writes[draw=%u "
                     "clear=%u clearfail=%u copy=%u uav=%u uavdrop=%u] nan=%llu",
                     index, static_cast<unsigned long long>(address), d.format,
                     w, h, d.bind_flags, depth ? " DEPTH" : "",
                     texture.diag_rt_draws, texture.diag_clears,
                     texture.diag_clear_failures, texture.diag_copy_dst,
                     texture.diag_uav_binds, texture.diag_uav_drops,
                     static_cast<unsigned long long>(nans));
        for (int c = 0; c < channels; ++c) {
            std::fprintf(stats, " | c%d min=%g max=%g mean=%g", c, mn[c], mx[c],
                         mean[c] / static_cast<double>(count));
        }
        if (texture.diag_rt_writer != 0) {
            const auto ps = m_pixel_shaders.find(texture.diag_rt_writer);
            if (ps != m_pixel_shaders.end() && !ps->second.bytecode.empty()) {
                char blob_name[64];
                std::snprintf(blob_name, sizeof(blob_name),
                              "/rt%u_writer_ps.dxbc", index);
                if (std::FILE* blob =
                        std::fopen((dir + blob_name).c_str(), "wb")) {
                    std::fwrite(ps->second.bytecode.data(), 1,
                                ps->second.bytecode.size(), blob);
                    std::fclose(blob);
                }
                std::fprintf(stats, " | last-ps 0x%llx -> %s",
                             static_cast<unsigned long long>(
                                 texture.diag_rt_writer),
                             blob_name + 1);
            }
        }
        if (texture.diag_uav_writer != 0) {
            // A 2D texture filled through a UAV (UE compute passes) names
            // its last dispatcher too; without it an all-NaN UAV target has
            // no owner in the dump.
            const std::vector<u8>* bytecode = nullptr;
            if (texture.diag_uav_writer_compute) {
                const auto cs = m_compute_shaders.find(texture.diag_uav_writer);
                if (cs != m_compute_shaders.end()) bytecode = &cs->second.bytecode;
            } else {
                const auto ps = m_pixel_shaders.find(texture.diag_uav_writer);
                if (ps != m_pixel_shaders.end()) bytecode = &ps->second.bytecode;
            }
            if (bytecode != nullptr && !bytecode->empty()) {
                char blob_name[64];
                std::snprintf(blob_name, sizeof(blob_name),
                              "/rt%u_uav_writer_%s.dxbc", index,
                              texture.diag_uav_writer_compute ? "cs" : "ps");
                if (std::FILE* blob =
                        std::fopen((dir + blob_name).c_str(), "wb")) {
                    std::fwrite(bytecode->data(), 1, bytecode->size(), blob);
                    std::fclose(blob);
                }
                std::fprintf(stats, " | uav-writer %s 0x%llx -> %s |%s",
                             texture.diag_uav_writer_compute ? "cs" : "ps",
                             static_cast<unsigned long long>(
                                 texture.diag_uav_writer),
                             blob_name + 1, texture.diag_uav_note.c_str());
            }
        }
        if (tiny && ok && static_cast<usize>(w) * h <= 16) {
            std::fprintf(stats, " | texels:");
            for (usize i = 0; i < static_cast<usize>(w) * h; ++i) {
                float t[4];
                texel_at(i, t);
                std::fprintf(stats, " (%g,%g,%g,%g)", t[0], t[1], t[2], t[3]);
            }
        }
        std::fprintf(stats, "\n");
        print_pairs(texture);
        print_mips(texture);
        if (tiny) {
            ++index;
            continue;
        }
        // An integer target's image shows value / max (else nearly all of
        // it clamps to white).
        const float image_scale =
            is_uint_format(d.format) && mx[0] > 0.0f ? 1.0f / mx[0] : 1.0f;
        // At most 4096 texels on a side: a larger level is drawn from every
        // step-th texel (a 9288^2 pool is a 3096^2 image, not 259 MB).
        const u32 step = (std::max(w, h) + 4095u) / 4096u;
        const u32 iw = (w + step - 1) / step;
        const u32 ih = (h + step - 1) / step;
        char name[200];
        std::snprintf(name, sizeof(name), "%s/%03u_fmt%u_%ux%u%s%s.ppm",
                      dir.c_str(), index, d.format, w, h,
                      depth ? "_depth" : "",
                      step > 1 ? ("_every" + std::to_string(step)).c_str()
                               : "");
        std::FILE* img = std::fopen(name, "wb");
        if (img == nullptr) {
            const int error = errno;
            std::fprintf(stats, "    image %s not written: %s\n",
                         name + dir.size() + 1, std::strerror(error));
        }
        if (img != nullptr) {
            std::fprintf(img, "P6\n%u %u\n255\n", iw, ih);
            std::vector<u8> row(static_cast<usize>(iw) * 3);
            bool written = true;
            for (u32 iy = 0; iy < ih; ++iy) {
                const u32 y = iy * step;
                for (u32 ix = 0; ix < iw; ++ix) {
                    const u32 x = ix * step;
                    float t[4];
                    texel_at(static_cast<usize>(y) * w + x, t);
                    for (int c = 0; c < 3; ++c) {
                        float v = (channels == 1 ? t[0]
                                                 : (c < channels ? t[c] : 0.0f)) *
                                  image_scale;
                        if (std::isnan(v)) v = 1.0f;
                        row[ix * 3 + c] = static_cast<u8>(
                            std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                }
                written = written &&
                          std::fwrite(row.data(), 1, row.size(), img) ==
                              row.size();
            }
            const int error = written ? 0 : errno;
            if (std::fclose(img) != 0 && written) {
                written = false;
            }
            if (!written) {
                std::fprintf(stats, "    image %s incomplete: %s\n",
                             name + dir.size() + 1,
                             std::strerror(error != 0 ? error : errno));
            }
        }
        ++index;
    }
    // From here on every target keeps the draws until the next dump; this
    // one starts the window (or ends the previous one).
    if (!m_diag_draw_pairs_armed) {
        std::fprintf(stats,
                     "draw combinations: recorded from this dump on -- "
                     "create the marker again for the next dump\n");
    }
    m_diag_draw_pairs_armed = true;
    for (auto& [address, texture] : m_textures) {
        (void)address;
        texture.diag_draw_pairs.clear();
        texture.diag_draw_pairs_dropped = 0;
    }
    std::fclose(stats);
    return true;
}

void D3D11Manager::open_diag_transfer_log(const std::string& dir,
                                          std::FILE* stats) {
    if (m_diag_transfer_log != nullptr) {
        std::fprintf(stats, "transfers: %s\n",
                     m_diag_transfer_to_stderr
                         ? "logging to stderr (the Vitreo log)"
                         : "logging to the transfers.txt opened earlier");
        return;
    }
    const std::string path = dir + "/transfers.txt";
    errno = 0;
    m_diag_transfer_log = std::fopen(path.c_str(), "w");
    if (m_diag_transfer_log == nullptr) {
        const int error = errno;
        std::fprintf(stats,
                     "transfers: %s not opened: %s (errno %d); the lines go "
                     "to stderr (the Vitreo log)\n",
                     path.c_str(), std::strerror(error), error);
        std::fprintf(stderr,
                     "[d3d11-xfer] frame=%llu log %s not opened: %s (errno "
                     "%d); logging here\n",
                     static_cast<unsigned long long>(m_diag_frame),
                     path.c_str(), std::strerror(error), error);
        m_diag_transfer_log = stderr;
        m_diag_transfer_to_stderr = true;
        return;
    }
    // Line-buffered: the log is read while the guest runs.
    std::setvbuf(m_diag_transfer_log, nullptr, _IOLBF, 0);
    m_diag_transfer_to_stderr = false;
    std::fprintf(stats, "transfers: logging to %s\n", path.c_str());
}

void D3D11Manager::diag_xfer(const char* format, ...) {
    if (m_diag_transfer_log == nullptr ||
        m_diag_transfer_lines >= kDiagTransferLines) {
        return;
    }
    if (++m_diag_transfer_lines == kDiagTransferLines) {
        std::fprintf(m_diag_transfer_log,
                     "[d3d11-xfer] frame=%llu log-full lines=%llu: later "
                     "events are not logged\n",
                     static_cast<unsigned long long>(m_diag_frame),
                     static_cast<unsigned long long>(kDiagTransferLines));
        return;
    }
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fprintf(m_diag_transfer_log, "[d3d11-xfer] frame=%llu %s\n",
                 static_cast<unsigned long long>(m_diag_frame), line);
}

namespace {

/// "(l,t,f)-(r,b,k)" of the guest D3D11_BOX at @p box, "whole" for none.
std::string box_text(GuestMemory& memory, GuestAddr box) {
    if (box == 0) return "whole";
    u32 b[6] = {};
    for (u32 i = 0; i < 6; ++i) {
        const auto field = memory.read_u32(box + i * sizeof(u32));
        if (field.is_err()) return "unreadable";
        b[i] = field.value();
    }
    char text[96];
    std::snprintf(text, sizeof(text), "(%u,%u,%u)-(%u,%u,%u)", b[0], b[1],
                  b[2], b[3], b[4], b[5]);
    return text;
}

const char* map_type_name(u32 type) {
    switch (type) {
    case D3D11_MAP_READ: return "READ";
    case D3D11_MAP_WRITE: return "WRITE";
    case D3D11_MAP_READ_WRITE: return "READ_WRITE";
    case D3D11_MAP_WRITE_DISCARD: return "WRITE_DISCARD";
    case D3D11_MAP_WRITE_NO_OVERWRITE: return "WRITE_NO_OVERWRITE";
    default: return "?";
    }
}

}  // namespace

void D3D11Manager::diag_log_large_texture_write(
    const char* what, GuestAddr dst, u32 dst_sub, u32 dst_x, u32 dst_y,
    u32 dst_z, GuestAddr dst_box, GuestAddr src, u32 src_sub,
    GuestAddr src_box, u32 row_pitch, u32 depth_pitch, const char* result) {
    const auto texture = m_textures.find(dst);
    if (m_diag_transfer_log == nullptr || texture == m_textures.end()) return;
    const Texture2D& t = texture->second;
    const u32 width = t.volume ? t.volume_desc.width : t.desc.width;
    const u32 height = t.volume ? t.volume_desc.height : t.desc.height;
    if (static_cast<u64>(width) * height <= 4096ull * 4096ull) return;
    const u32 mips = std::max(1u, t.volume ? t.volume_desc.mip_levels
                                           : t.desc.mip_levels);
    char head[256];
    std::snprintf(head, sizeof(head),
                  "large-write op=%s dst=0x%llx fmt=%u %ux%u sub=%u mip=%u "
                  "slice=%u",
                  what, static_cast<unsigned long long>(dst),
                  t.volume ? t.volume_desc.format : t.desc.format, width,
                  height, dst_sub, dst_sub % mips, dst_sub / mips);
    if (src != 0) {
        u32 sf = 0, sw = 0, sh = 0;
        const auto source = m_textures.find(src);
        if (source != m_textures.end()) {
            const Texture2D& s = source->second;
            sf = s.volume ? s.volume_desc.format : s.desc.format;
            sw = s.volume ? s.volume_desc.width : s.desc.width;
            sh = s.volume ? s.volume_desc.height : s.desc.height;
        }
        diag_xfer("%s at=(%u,%u,%u) src=0x%llx src_fmt=%u src_size=%ux%u "
                  "src_sub=%u src_box=%s -> %s",
                  head, dst_x, dst_y, dst_z,
                  static_cast<unsigned long long>(src), sf, sw, sh, src_sub,
                  box_text(m_memory, src_box).c_str(), result);
    } else {
        diag_xfer("%s box=%s pitch=%u/%u -> %s", head,
                  box_text(m_memory, dst_box).c_str(), row_pitch, depth_pitch,
                  result);
    }
}

void D3D11Manager::diag_log_staging_copy(const char* op, GuestAddr dst,
                                         u32 dst_sub, u32 dst_x, u32 dst_y,
                                         u32 dst_z, GuestAddr src, u32 src_sub,
                                         GuestAddr src_box) {
    if (m_diag_transfer_log == nullptr) return;
    char what[160];
    if (const auto b = m_buffers.find(dst); b != m_buffers.end()) {
        if (b->second.desc.usage != D3D11_USAGE_STAGING) return;
        b->second.diag_copy_frame = m_diag_frame;
        std::snprintf(what, sizeof(what), "dst=0x%llx buffer bytes=%u",
                      static_cast<unsigned long long>(dst),
                      b->second.desc.byte_width);
    } else if (const auto t = m_textures.find(dst); t != m_textures.end()) {
        const Texture2D& tex = t->second;
        const u32 usage = tex.volume ? tex.volume_desc.usage : tex.desc.usage;
        if (usage != D3D11_USAGE_STAGING) return;
        t->second.diag_copy_frame = m_diag_frame;
        std::snprintf(what, sizeof(what), "dst=0x%llx texture fmt=%u %ux%u",
                      static_cast<unsigned long long>(dst),
                      tex.volume ? tex.volume_desc.format : tex.desc.format,
                      tex.volume ? tex.volume_desc.width : tex.desc.width,
                      tex.volume ? tex.volume_desc.height : tex.desc.height);
    } else {
        return;
    }
    char source[160] = "?";
    if (const auto b = m_buffers.find(src); b != m_buffers.end()) {
        std::snprintf(source, sizeof(source),
                      "src=0x%llx buffer bytes=%u usage=%u bind=0x%x",
                      static_cast<unsigned long long>(src),
                      b->second.desc.byte_width, b->second.desc.usage,
                      b->second.desc.bind_flags);
    } else if (const auto t = m_textures.find(src); t != m_textures.end()) {
        const Texture2D& tex = t->second;
        std::snprintf(source, sizeof(source),
                      "src=0x%llx texture fmt=%u %ux%u bind=0x%x",
                      static_cast<unsigned long long>(src),
                      tex.volume ? tex.volume_desc.format : tex.desc.format,
                      tex.volume ? tex.volume_desc.width : tex.desc.width,
                      tex.volume ? tex.volume_desc.height : tex.desc.height,
                      tex.volume ? tex.volume_desc.bind_flags
                                 : tex.desc.bind_flags);
    } else {
        std::snprintf(source, sizeof(source), "src=0x%llx unknown",
                      static_cast<unsigned long long>(src));
    }
    diag_xfer("copy-to-staging op=%s %s sub=%u at=(%u,%u,%u) %s src_sub=%u "
              "src_box=%s",
              op, what, dst_sub, dst_x, dst_y, dst_z, source, src_sub,
              box_text(m_memory, src_box).c_str());
}

void D3D11Manager::diag_log_event_query(GuestAddr query, bool end, u32 flags,
                                        u32 data_size, u32 hr) {
    if (m_diag_transfer_log == nullptr) return;
    const auto found = m_queries.find(query);
    if (found == m_queries.end() ||
        found->second.query_type != D3D11_QUERY_EVENT) {
        return;
    }
    Query& q = found->second;
    if (end) {
        q.diag_end_frame = m_diag_frame;
        q.diag_polls = 0;
        diag_xfer("event-end query=0x%llx",
                  static_cast<unsigned long long>(query));
        return;
    }
    ++q.diag_polls;
    diag_xfer("event-getdata query=0x%llx flags=0x%x%s size=%u -> 0x%08x "
              "polls=%u since_end=%llu",
              static_cast<unsigned long long>(query), flags,
              (flags & D3D11_ASYNC_GETDATA_DONOTFLUSH) != 0 ? " DONOTFLUSH"
                                                            : "",
              data_size, hr, q.diag_polls,
              static_cast<unsigned long long>(m_diag_frame - q.diag_end_frame));
}

void D3D11Manager::diag_log_staging_map(GuestAddr resource, u32 subresource,
                                        u32 map_type, u32 map_flags, u32 hr,
                                        GuestAddr data, u32 row_pitch) {
    if (m_diag_transfer_log == nullptr) return;
    const char* kind = nullptr;
    usize bytes = 0;
    u64 copy_frame = ~u64{0};
    if (const auto b = m_buffers.find(resource); b != m_buffers.end()) {
        if (b->second.desc.usage != D3D11_USAGE_STAGING) return;
        kind = "buffer";
        bytes = b->second.desc.byte_width;
        copy_frame = b->second.diag_copy_frame;
    } else if (const auto t = m_textures.find(resource);
               t != m_textures.end()) {
        const u32 usage = t->second.volume ? t->second.volume_desc.usage
                                           : t->second.desc.usage;
        if (usage != D3D11_USAGE_STAGING) return;
        kind = "texture";
        bytes = row_pitch;
        copy_frame = t->second.diag_copy_frame;
    } else {
        return;
    }
    std::string since = "none";
    if (copy_frame != ~u64{0}) since = std::to_string(m_diag_frame - copy_frame);
    std::string first;
    if (hr == d3d11::S_OK && data != 0 && map_type != D3D11_MAP_WRITE_DISCARD) {
        u8 head[64] = {};
        const usize n = std::min<usize>(sizeof(head), bytes != 0 ? bytes : 64);
        if (m_memory.read_bytes(data, reinterpret_cast<Byte*>(head), n)
                .is_ok()) {
            first = " first" + std::to_string(n) + ":";
            char word_text[12];
            for (usize i = 0; i + 4 <= n; i += 4) {
                u32 word = 0;
                std::memcpy(&word, head + i, 4);
                std::snprintf(word_text, sizeof(word_text), " %08x", word);
                first += word_text;
            }
        }
    }
    diag_xfer("map-staging %s=0x%llx sub=%u type=%s flags=0x%x%s -> 0x%08x "
              "since_copy=%s pitch=%u%s",
              kind, static_cast<unsigned long long>(resource), subresource,
              map_type_name(map_type), map_flags,
              (map_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) != 0 ? " DO_NOT_WAIT"
                                                            : "",
              hr, since.c_str(), row_pitch, first.c_str());
}

void D3D11Manager::diag_log_clear_uav(GuestAddr view, bool is_float,
                                      const std::array<u32, 4>& values,
                                      bool cleared) {
    if (m_diag_transfer_log == nullptr) return;
    const auto uav = m_unordered_access_views.find(view);
    if (uav == m_unordered_access_views.end() || uav->second.buffer == 0) {
        return;
    }
    diag_xfer("clear-uav kind=%s view=0x%llx buffer=0x%llx fmt=%u first=%u "
              "n=%u values=(0x%x,0x%x,0x%x,0x%x) -> %s",
              is_float ? "Float" : "Uint",
              static_cast<unsigned long long>(view),
              static_cast<unsigned long long>(uav->second.buffer),
              uav->second.format, uav->second.first_element,
              uav->second.num_elements, values[0], values[1], values[2],
              values[3], cleared ? "cleared" : "ignored");
}

void D3D11Manager::diag_log_om_uavs(u32 rtvs, GuestAddr dsv, u32 uav_start,
                                    u32 uav_count, GuestAddr uavs,
                                    GuestAddr initial_counts) {
    if (m_diag_transfer_log == nullptr) return;
    constexpr u32 kKeep = 0xFFFFFFFFu;  // D3D11_KEEP_*
    std::string text;
    char part[200];
    if (uav_count == kKeep) {
        text = " uavs=keep";
    } else {
        std::snprintf(part, sizeof(part), " start=%u uavs=%u", uav_start,
                      uav_count);
        text = part;
        const usize pointer = m_is_64bit ? 8u : 4u;
        for (u32 i = 0; i < uav_count && i < 8u && uavs != 0; ++i) {
            GuestAddr view = 0;
            if (m_is_64bit) {
                view = m_memory.read_u64(uavs + i * pointer).value_or(0);
            } else {
                view = m_memory.read_u32(uavs + i * pointer).value_or(0);
            }
            std::string initial = "none";
            if (initial_counts != 0) {
                const u32 c =
                    m_memory.read_u32(initial_counts + i * 4u).value_or(0);
                initial = c == kKeep ? "keep" : std::to_string(c);
            }
            const auto uav = m_unordered_access_views.find(view);
            if (view != 0 && uav != m_unordered_access_views.end() &&
                uav->second.buffer != 0) {
                std::snprintf(part, sizeof(part),
                              " u%u=0x%llx buffer=0x%llx first=%u n=%u "
                              "flags=0x%x initial=%s",
                              uav_start + i,
                              static_cast<unsigned long long>(view),
                              static_cast<unsigned long long>(
                                  uav->second.buffer),
                              uav->second.first_element,
                              uav->second.num_elements, uav->second.buffer_flags,
                              initial.c_str());
            } else if (view != 0 && uav != m_unordered_access_views.end()) {
                std::snprintf(part, sizeof(part),
                              " u%u=0x%llx texture=0x%llx initial=%s",
                              uav_start + i,
                              static_cast<unsigned long long>(view),
                              static_cast<unsigned long long>(
                                  uav->second.texture),
                              initial.c_str());
            } else {
                std::snprintf(part, sizeof(part), " u%u=0x%llx%s",
                              uav_start + i,
                              static_cast<unsigned long long>(view),
                              view != 0 ? "(unknown)" : "");
            }
            text += part;
        }
    }
    diag_xfer("om-uav rtvs=%s dsv=0x%llx%s",
              rtvs == kKeep ? "keep" : std::to_string(rtvs).c_str(),
              static_cast<unsigned long long>(dsv), text.c_str());
}

}  // namespace vitreo::winapi
