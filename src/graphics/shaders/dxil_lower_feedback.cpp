// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_lower_feedback.cpp
/// @brief DXIL WriteSamplerFeedback / Bias / Level / Grad (opcodes 174-177,
/// shader model 6.5) -> SM5.1 arithmetic and atomic_or into the feedback
/// map's opaque storage (sampler_feedback_layout.hpp).
///
/// Sources: DirectX-Specs "Sampler Feedback" (the ops' operands, the
/// FeedbackTexture2D / FeedbackTexture2DArray resources and their tag-2
/// SamplerFeedbackType property, "the writing of feedback does not support
/// offsets", helper-lane writes are discarded) and DXIL.rst's operation
/// table. Operand order, from dxc 1.6 listings of
/// tools/oracle_probe/d3d12_vinst/d3d12_feedback.hlsl:
///   174 (feedbackTex, sampledTex, sampler, c0, c1, c2, c3, clamp)
///   175 (..., c3, bias, clamp)
///   176 (..., c3, lod)
///   177 (..., c3, ddx0, ddx1, ddx2, ddy0, ddy1, ddy2, clamp)
///
/// A write records the mips and texels the paired Sample* call would read,
/// as D3D11.3 7.18 computes them for the sampler bound with it:
///  - LOD: 176's level; otherwise log2 of the longer texel-space gradient
///    (174/175: coarse derivatives of the coordinates; 177: the given
///    gradients) or, for an ANISOTROPIC filter, of the major axis divided by
///    the probe count min(ceil(major / minor), MaxAnisotropy); plus 175's
///    bias and the sampler's MipLODBias, clamped to [MinLOD, MaxLOD], then to
///    the map's mip range, and quantized to 8 fraction bits (D3D11.3 7.18.11's
///    fixed-point LOD) so a computed integer LOD stays one.
///  - Mips: POINT mip filter round(LOD + 0.5) down (7.18.10); LINEAR
///    floor(LOD) and ceil(LOD).
///  - Texels: the min (LOD > 0) or mag filter's footprint at each mip --
///    the 2x2 bilinear quad of u * size - 0.5 or the point texel -- through
///    the sampler's address modes (a BORDER texel outside the mip reads no
///    texel and records nothing); an anisotropic filter repeats it for each
///    probe along the major axis.
///  - Array slice: round_ne(c2); outside the array nothing is recorded (as
///    the RTX 4080 SUPER does; WARP clamps the slice).
/// The clamp operand is not applied: both measured implementations
/// (winoracle-gpu, the NVIDIA one, and WARP; d3d12_a41_feedback_probe
/// "grad min clamp 3", "ps clamp 4 min", "ps bias 0.5 clamp 3 min") record
/// the unclamped mips.
///
/// The sampler state comes from its sampler_state_buffer record through
/// VX_SAMPLER_STATE (dxbc_vitreo_ext.hpp); the map's dimensions, region size
/// and per-mip layout from the header at the start of its storage. The
/// feedback UAV is declared dcl_uav_raw (the frontend binds the storage as a
/// raw buffer view). Each recorded (mip, region) is one bit set with
/// atomic_or, for both MIN_MIP and MIP_REGION_USED maps.

#include "dxbc_vitreo_ext.hpp"
#include "dxbc_to_spirv.hpp"
#include "dxil_lower_ext.hpp"
#include "sampler_feedback_layout.hpp"

#include <cstring>

namespace vitreo::graphics::dxil_lower {

using namespace dxbc_vitreo_ext;
namespace fb = sampler_feedback;

bool is_feedback_opcode(u32 opcode) { return opcode >= 174 && opcode <= 177; }

namespace {

constexpr u32 op(Sm4Opcode o) { return static_cast<u32>(o); }
constexpr u32 sw(u32 x, u32 y, u32 z, u32 w) { return x | (y << 2) | (z << 4) | (w << 6); }
constexpr u32 kXYZW = 0xE4;

u32 fbits(float f) {
    u32 u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}
Operand fimm(float f) { return imm(fbits(f)); }

/// D3D12_FILTER (d3d12.h): mip bits 0-1, mag 2-3, min 4-5,
/// D3D12_ANISOTROPIC_FILTERING_BIT 0x40; D3D12_FILTER_TYPE_LINEAR 1.
constexpr u32 kAnisotropicBit = 0x40;
/// D3D12_TEXTURE_ADDRESS_MODE (d3d12.h).
constexpr u32 kWrap = 1, kMirror = 2, kBorder = 4, kMirrorOnce = 5;

/// One temp register.
struct R {
    u32 r;
    [[nodiscard]] Operand d(u32 mask) const { return dst_temp(r, mask); }
    [[nodiscard]] Operand s(u32 swizzle = kXYZW, u32 modifier = 0) const { return src_temp(r, swizzle, modifier); }
    [[nodiscard]] Operand c(u32 comp, u32 modifier = 0) const { return src_temp_scalar(r, comp, modifier); }
};

class FeedbackLowering {
public:
    FeedbackLowering(LowerHost& host, u32 opcode, const DxilInstruction& inst)
        : h_(host), e_(host.body()), opcode_(opcode), inst_(inst) {}

    bool run();

private:
    [[nodiscard]] u32 arg(usize k) const { return inst_.operands[1 + k]; }
    R reg() { return R{h_.fresh_vector()}; }
    void i(Sm4Opcode o, std::initializer_list<const Operand*> ops) { e_.inst(op(o), ops); }
    /// Moves DXIL value @p id (0 when undef) into component @p comp of @p r.
    void move_value(R r, u32 comp, u32 id) {
        const Operand d = r.d(1u << comp);
        if (h_.undef(id)) {
            const Operand z = imm(0);
            i(Sm4Opcode::MOV, {&d, &z});
            return;
        }
        const Operand s = h_.src(id);
        i(Sm4Opcode::MOV, {&d, &s});
    }

    /// D3D11.3 7.18.7 address modes, per component: @p c holds integer
    /// texel coordinates, @p mode D3D12_TEXTURE_ADDRESS_MODE, @p size_f and
    /// @p size the mip's size (float, uint). On return @p c is the texel
    /// read (in range) and @p size ~0 where one is read, 0 where a BORDER
    /// coordinate outside the mip reads the border colour instead.
    void address(R c, R mode, R size_f, R size);

    LowerHost& h_;
    Emitter& e_;
    u32 opcode_;
    const DxilInstruction& inst_;
};

bool FeedbackLowering::run() {
    const usize want = opcode_ == 174 ? 8 : opcode_ == 175 ? 9 : opcode_ == 176 ? 8 : 14;
    if (inst_.operands.size() < 1 + 1 + want) return h_.malformed("WriteSamplerFeedback operands");
    ResourceView map{}, smp{};
    if (!h_.resource_of(arg(1), map) || !h_.resource_of(arg(3), smp)) return false;
    if (map.cls != ResourceClass::UAV ||
        (map.shape != ResourceShape::FeedbackTexture2D && map.shape != ResourceShape::FeedbackTexture2DArray)) {
        return h_.malformed("WriteSamplerFeedback into a resource that is not a feedback texture");
    }
    if (smp.cls != ResourceClass::Sampler) return h_.malformed("WriteSamplerFeedback without a sampler");
    const bool array = map.shape == ResourceShape::FeedbackTexture2DArray;
    const Operand uav_src = resource(Sm4OperandType::UnorderedAccessView, map.range_id, map.reg, map.nonuniform);
    const Operand uav_dst =
        resource(Sm4OperandType::UnorderedAccessView, map.range_id, map.reg, map.nonuniform, ~0u, 0x1);
    const Operand sampler = resource(Sm4OperandType::Sampler, smp.range_id, smp.reg, smp.nonuniform);

    // Header: (W, H, mips, slices), (log2 region w, log2 region h, words
    // per slice, format).
    const R hd = reg(), hd2 = reg();
    {
        const Operand d = hd.d(0xF), d2 = hd2.d(0xF), a0 = imm(fb::kWidth * 4), a1 = imm(fb::kLog2RegionWidth * 4);
        i(Sm4Opcode::LD_RAW, {&d, &a0, &uav_src});
        i(Sm4Opcode::LD_RAW, {&d2, &a1, &uav_src});
    }
    // Sampler: (MipLODBias, flags, filter state, MinLOD), (MaxLOD, ...).
    const R ss = reg(), ss2 = reg();
    const u32 words[5] = {kSamplerStateBiasWord, kSamplerStateFlagsWord, kSamplerStateFilterStateWord,
                          kSamplerStateFullMinLodWord, kSamplerStateFullMaxLodWord};
    for (u32 k = 0; k < 5; ++k) {
        const Operand d = k < 4 ? ss.d(1u << k) : ss2.d(0x1);
        emit_ext(e_, kOpSamplerState, words[k], {&d, &sampler});
    }
    // Coordinates (u, v, slice).
    const R crd = reg();
    move_value(crd, 0, arg(4));
    move_value(crd, 1, arg(5));
    if (array) {
        move_value(crd, 2, arg(6));
    } else {
        const Operand d = crd.d(0x4), z = imm(0);
        i(Sm4Opcode::MOV, {&d, &z});
    }
    // dims = (W, H, W, H) as floats.
    const R dimf = reg();
    {
        const Operand d = dimf.d(0xF), s = hd.s(sw(0, 1, 0, 1));
        i(Sm4Opcode::UTOF, {&d, &s});
    }

    // lam.x = LOD, lam.y = probe count N (float), lam.zw = scratch.
    // axis.xy = the anisotropic major axis in uv units (0 otherwise).
    const R lam = reg(), axis = reg(), g = reg(), t = reg();
    {
        const Operand d = axis.d(0xF), z = imm(0);
        i(Sm4Opcode::MOV, {&d, &z});
        const Operand n = lam.d(0x2), one = fimm(1.0f);
        i(Sm4Opcode::MOV, {&n, &one});
    }
    if (opcode_ == 176) {
        move_value(lam, 0, arg(8));
    } else {
        // g = (ds/dx, dt/dx, ds/dy, dt/dy) in mip-0 texels.
        if (opcode_ == 177) {
            move_value(g, 0, arg(8));
            move_value(g, 1, arg(9));
            move_value(g, 2, arg(11));
            move_value(g, 3, arg(12));
            const Operand d = g.d(0xF), a = g.s(), b = dimf.s();
            i(Sm4Opcode::MUL, {&d, &a, &b});
        } else {
            const Operand td = t.d(0x3), a = crd.s(), b = dimf.s();
            i(Sm4Opcode::MUL, {&td, &a, &b});
            const Operand gx = g.d(0x3), gy = g.d(0xC), ts = t.s(sw(0, 1, 0, 1));
            i(Sm4Opcode::DERIV_RTX_COARSE, {&gx, &ts});
            i(Sm4Opcode::DERIV_RTY_COARSE, {&gy, &ts});
        }
        // t.x = |gx|^2, t.y = |gy|^2, t.z = major^2, t.w = minor^2.
        {
            const Operand tx = t.d(0x1), ty = t.d(0x2), gxy = g.s(sw(0, 1, 0, 1)), gzw = g.s(sw(2, 3, 2, 3));
            i(Sm4Opcode::DP2, {&tx, &gxy, &gxy});
            i(Sm4Opcode::DP2, {&ty, &gzw, &gzw});
            const Operand tz = t.d(0x4), tw = t.d(0x8), a = t.c(0), b = t.c(1);
            i(Sm4Opcode::MAX, {&tz, &a, &b});
            i(Sm4Opcode::MIN, {&tw, &a, &b});
        }
        // LOD = log2(major^2) / 2.
        {
            const Operand lx = lam.d(0x1), mj = t.c(2), half = fimm(0.5f), lxs = lam.c(0);
            i(Sm4Opcode::LOG, {&lx, &mj});
            i(Sm4Opcode::MUL, {&lx, &lxs, &half});
        }
        // Anisotropic: N = min(ceil(sqrt(major^2 / minor^2)), MaxAnisotropy),
        // LOD -= log2(N), and the major axis (the longer gradient).
        const R an = reg();
        {
            const Operand ax = an.d(0x1), st = ss.c(2), bit = imm(kAnisotropicBit);
            i(Sm4Opcode::AND, {&ax, &st, &bit});
            const Operand ay = an.d(0x2), sh = imm(kSamplerStateMaxAnisoShift), m5 = imm(0x1F);
            i(Sm4Opcode::USHR, {&ay, &st, &sh});
            const Operand ays = an.c(1);
            i(Sm4Opcode::AND, {&ay, &ays, &m5});
            const Operand one = imm(1);
            i(Sm4Opcode::UMAX, {&ay, &ays, &one});
            i(Sm4Opcode::UTOF, {&ay, &ays});
            const Operand axs = an.c(0);
            e_.inst(op(Sm4Opcode::IF) | kTestNonZero, {&axs});
            {
                const Operand az = an.d(0x4), mj = t.c(2), mn = t.c(3), azs = an.c(2);
                i(Sm4Opcode::DIV, {&az, &mj, &mn});
                i(Sm4Opcode::SQRT, {&az, &azs});
                i(Sm4Opcode::ROUND_PI, {&az, &azs});
                i(Sm4Opcode::MIN, {&az, &azs, &ays});
                const Operand ln = lam.d(0x2);
                i(Sm4Opcode::MOV, {&ln, &azs});
                const Operand aw = an.d(0x8), aws = an.c(3), lx = lam.d(0x1), lxs = lam.c(0);
                i(Sm4Opcode::LOG, {&aw, &azs});
                const Operand neg = an.c(3, 1);
                i(Sm4Opcode::ADD, {&lx, &lxs, &neg});
                (void)aws;
                // Major axis: gx when |gx| >= |gy|, in uv units.
                const Operand ge = an.d(0x1), a = t.c(0), b = t.c(1);
                i(Sm4Opcode::GE, {&ge, &a, &b});
                const Operand axd = axis.d(0x3), gxy = g.s(sw(0, 1, 0, 1)), gzw = g.s(sw(2, 3, 2, 3)), sel = an.c(0);
                i(Sm4Opcode::MOVC, {&axd, &sel, &gxy, &gzw});
                const Operand axs2 = axis.s(), dm = dimf.s();
                i(Sm4Opcode::DIV, {&axd, &axs2, &dm});
            }
            i(Sm4Opcode::ENDIF, {});
        }
        if (opcode_ == 175) {
            const R b = reg();
            move_value(b, 0, arg(8));
            const Operand lx = lam.d(0x1), lxs = lam.c(0), bs = b.c(0);
            i(Sm4Opcode::ADD, {&lx, &lxs, &bs});
        }
    }
    // + MipLODBias (without the point-mip tie nudge the frontend may have
    // added for Metal's own rounding), clamp to [MinLOD, MaxLOD].
    {
        const Operand lx = lam.d(0x1), lxs = lam.c(0), bias = ss.c(0);
        i(Sm4Opcode::ADD, {&lx, &lxs, &bias});
        const Operand tz = t.d(0x4), fl = ss.c(1), nud = imm(kSamplerStateBiasNudged);
        i(Sm4Opcode::AND, {&tz, &fl, &nud});
        const Operand tzs = t.c(2), nv = fimm(-kPointMipTieNudge), zero = fimm(0.0f);
        i(Sm4Opcode::MOVC, {&tz, &tzs, &nv, &zero});
        i(Sm4Opcode::ADD, {&lx, &lxs, &tzs});
        const Operand mn = ss.c(3), mx = ss2.c(0);
        i(Sm4Opcode::MAX, {&lx, &lxs, &mn});
        i(Sm4Opcode::MIN, {&lx, &lxs, &mx});
    }
    // sel.x = minify (LOD > 0), sel.y = linear footprint, sel.z = linear mip,
    // sel.w = slice in range.
    const R sel = reg();
    {
        const Operand sx = sel.d(0x1), zero = fimm(0.0f), lxs = lam.c(0);
        i(Sm4Opcode::LT, {&sx, &zero, &lxs});
        // filter >> (minify ? 4 : 2) & 3 == 1, or anisotropic.
        const Operand sy = sel.d(0x2), sxs = sel.c(0), four = imm(4), two = imm(2), sys = sel.c(1);
        i(Sm4Opcode::MOVC, {&sy, &sxs, &four, &two});
        const Operand st = ss.c(2);
        i(Sm4Opcode::USHR, {&sy, &st, &sys});
        const Operand three = imm(3), onei = imm(1);
        i(Sm4Opcode::AND, {&sy, &sys, &three});
        i(Sm4Opcode::IEQ, {&sy, &sys, &onei});
        const Operand sz = sel.d(0x4), szs = sel.c(2), bit = imm(kAnisotropicBit);
        i(Sm4Opcode::AND, {&sz, &st, &bit});
        const Operand zi = imm(0);
        i(Sm4Opcode::INE, {&sz, &szs, &zi});
        i(Sm4Opcode::OR, {&sy, &sys, &szs});
        // Mip filter LINEAR: bits 0-1 == 1.
        i(Sm4Opcode::AND, {&sz, &st, &three});
        i(Sm4Opcode::IEQ, {&sz, &szs, &onei});
    }
    // Slice: round_ne(c2) as an int, valid when below the slice count.
    const R mp = reg();  // x m0, y m1, z slice base word, w N (uint)
    {
        const Operand t0 = t.d(0x1), c2 = crd.c(2), t0s = t.c(0);
        i(Sm4Opcode::ROUND_NE, {&t0, &c2});
        i(Sm4Opcode::FTOI, {&t0, &t0s});
        const Operand sw_ = sel.d(0x8), slices = hd.c(3);
        i(Sm4Opcode::ULT, {&sw_, &t0s, &slices});
        const Operand mz = mp.d(0x4), wps = hd2.c(2), base = imm(fb::kHeaderWords);
        i(Sm4Opcode::UMAD, {&mz, &t0s, &wps, &base});
        const Operand mw = mp.d(0x8), n = lam.c(1);
        i(Sm4Opcode::FTOU, {&mw, &n});
    }
    // LOD clamped to the mip range and quantized; m0, m1.
    {
        const Operand tz = t.d(0x4), mips = hd.c(2), tzs = t.c(2);
        i(Sm4Opcode::UTOF, {&tz, &mips});
        const Operand m1 = fimm(-1.0f);
        i(Sm4Opcode::ADD, {&tz, &tzs, &m1});
        const Operand lz = lam.d(0x4), lxs = lam.c(0), zero = fimm(0.0f), lzs = lam.c(2);
        i(Sm4Opcode::MAX, {&lz, &lxs, &zero});
        i(Sm4Opcode::MIN, {&lz, &lzs, &tzs});
        const Operand k256 = fimm(256.0f), inv = fimm(1.0f / 256.0f);
        i(Sm4Opcode::MUL, {&lz, &lzs, &k256});
        i(Sm4Opcode::ROUND_NE, {&lz, &lzs});
        i(Sm4Opcode::MUL, {&lz, &lzs, &inv});
        // point: floor(LOD + 0.5); linear: floor(LOD), ceil(LOD).
        const Operand tx = t.d(0x1), ty = t.d(0x2), tw = t.d(0x8), half = fimm(0.5f);
        const Operand txs = t.c(0), tys = t.c(1), tws = t.c(3);
        i(Sm4Opcode::ADD, {&tw, &lzs, &half});
        i(Sm4Opcode::ROUND_NI, {&tw, &tws});
        i(Sm4Opcode::ROUND_NI, {&tx, &lzs});
        i(Sm4Opcode::ROUND_PI, {&ty, &lzs});
        const Operand lin = sel.c(2);
        i(Sm4Opcode::MOVC, {&tx, &lin, &txs, &tws});
        i(Sm4Opcode::MOVC, {&ty, &lin, &tys, &tws});
        const Operand mxy = mp.d(0x3), txy = t.s(sw(0, 1, 0, 1));
        i(Sm4Opcode::FTOU, {&mxy, &txy});
    }

    // for (k = 0; k < 2N; ++k): mip = k < N ? m0 : m1, probe = k mod N.
    const R lp = reg(), rec = reg(), sz = reg(), c = reg(), x = reg(), v = reg();
    {
        const Operand l0 = lp.d(0xF), z = imm(0);
        i(Sm4Opcode::MOV, {&l0, &z});
        const Operand ly = lp.d(0x2), n = mp.c(3);
        i(Sm4Opcode::IADD, {&ly, &n, &n});  // lp.y = 2N
    }
    i(Sm4Opcode::LOOP, {});
    {
        const Operand lz = lp.d(0x4), lx = lp.c(0), ly = lp.c(1), lzs = lp.c(2);
        i(Sm4Opcode::UGE, {&lz, &lx, &ly});
        e_.inst(op(Sm4Opcode::BREAKC) | kTestNonZero, {&lzs});
        // lp.z = second pass, lp.w = probe index.
        const Operand n = mp.c(3);
        i(Sm4Opcode::UGE, {&lz, &lx, &n});
        const Operand lw = lp.d(0x8), lws = lp.c(3);
        i(Sm4Opcode::AND, {&lw, &lzs, &n});
        const Operand negw = lp.c(3);
        i(Sm4Opcode::INEG, {&lw, &negw});
        i(Sm4Opcode::IADD, {&lw, &lx, &lws});
        // rec = mip record of m; sz = (S, S, T, T) as uint.
        const Operand t0 = t.d(0x1), m0 = mp.c(0), m1 = mp.c(1), t0s = t.c(0);
        i(Sm4Opcode::MOVC, {&t0, &lzs, &m1, &m0});
        const Operand t1 = t.d(0x2), k16 = imm(16), base = imm(fb::kMipRecords * 4), t1s = t.c(1);
        i(Sm4Opcode::UMAD, {&t1, &t0s, &k16, &base});
        const Operand rd = rec.d(0xF);
        i(Sm4Opcode::LD_RAW, {&rd, &t1s, &uav_src});
        const Operand szd = sz.d(0xF), wh = hd.s(sw(0, 0, 1, 1)), one = imm(1);
        i(Sm4Opcode::USHR, {&szd, &wh, &t0s});
        const Operand szs = sz.s();
        i(Sm4Opcode::UMAX, {&szd, &szs, &one});
        // Probe position: uv + axis * ((probe + 0.5) / N - 0.5).
        const Operand t2 = t.d(0x4), t2s = t.c(2), half = fimm(0.5f), nh = fimm(-0.5f), nf = lam.c(1);
        i(Sm4Opcode::UTOF, {&t2, &lws});
        i(Sm4Opcode::ADD, {&t2, &t2s, &half});
        i(Sm4Opcode::DIV, {&t2, &t2s, &nf});
        i(Sm4Opcode::ADD, {&t2, &t2s, &nh});
        // c = (u, u, v, v) * size, - 0.5 for a linear footprint.
        const Operand cd = c.d(0xF), cs = c.s(), cuv = crd.s(sw(0, 0, 1, 1)), ax = axis.s(sw(0, 0, 1, 1));
        i(Sm4Opcode::MAD, {&cd, &ax, &t2s, &cuv});
        const Operand xd = x.d(0xF), xs = x.s();
        i(Sm4Opcode::UTOF, {&xd, &szs});  // x = size as float
        i(Sm4Opcode::MUL, {&cd, &cs, &xs});
        const Operand lin = sel.c(1), nhalf = fimm(-0.5f), z = fimm(0.0f);
        const Operand vd = v.d(0xF), vs = v.s();
        i(Sm4Opcode::MOVC, {&vd, &lin, &nhalf, &z});
        i(Sm4Opcode::ADD, {&cd, &cs, &vs});
        i(Sm4Opcode::ROUND_NI, {&cd, &cs});
        i(Sm4Opcode::FTOI, {&cd, &cs});
        // + (0, 1, 0, 1) for linear.
        const Operand step = imm4(0, 1, 0, 1);
        i(Sm4Opcode::AND, {&vd, &lin, &step});
        i(Sm4Opcode::IADD, {&cd, &cs, &vs});
        // Address modes: v = (mU, mU, mV, mV).
        const Operand st = ss.c(2);
        const Operand shifts = imm4(kSamplerStateAddressUShift, kSamplerStateAddressUShift,
                                    kSamplerStateAddressVShift, kSamplerStateAddressVShift);
        i(Sm4Opcode::USHR, {&vd, &st, &shifts});
        const Operand am = imm(kSamplerStateAddressMask);
        i(Sm4Opcode::AND, {&vd, &vs, &am});
        address(c, v, x, sz);
        // Regions: c >> (lrw, lrw, lrh, lrh).
        const Operand lr = hd2.s(sw(0, 0, 1, 1));
        i(Sm4Opcode::USHR, {&cd, &cs, &lr});
        // Bits of the four combinations (x0,y0) (x1,y0) (x0,y1) (x1,y1).
        const Operand X = c.s(sw(0, 1, 0, 1)), Y = c.s(sw(2, 2, 3, 3)), gw = rec.c(1);
        i(Sm4Opcode::UMAD, {&xd, &Y, &gw, &X});  // x = bit index
        const Operand VX = sz.s(sw(0, 1, 0, 1)), VY = sz.s(sw(2, 2, 3, 3));
        i(Sm4Opcode::AND, {&vd, &VX, &VY});  // sz now holds validity (address())
        const Operand slice_ok = sel.c(3);
        i(Sm4Opcode::AND, {&vd, &vs, &slice_ok});
        const Operand ones = imm(1);
        i(Sm4Opcode::ISHL, {&cd, &ones, &xs});
        i(Sm4Opcode::AND, {&cd, &cs, &vs});  // c = bit values
        const Operand five = imm(5), off = rec.c(0), baseword = mp.c(2), two = imm(2);
        i(Sm4Opcode::USHR, {&xd, &xs, &five});
        i(Sm4Opcode::IADD, {&xd, &xs, &off});
        i(Sm4Opcode::IADD, {&xd, &xs, &baseword});
        i(Sm4Opcode::ISHL, {&xd, &xs, &two});  // x = byte addresses
        for (u32 k = 0; k < 4; ++k) {
            const Operand a = x.c(k), b = c.c(k);
            i(Sm4Opcode::ATOMIC_OR, {&uav_dst, &a, &b});
        }
        const Operand lxd = lp.d(0x1);
        i(Sm4Opcode::IADD, {&lxd, &lx, &one});
    }
    i(Sm4Opcode::ENDLOOP, {});
    return !h_.failed();
}

void FeedbackLowering::address(R c, R mode, R size_f, R size) {
    const R a = reg(), b = reg(), q = reg(), res = reg();
    const Operand ad = a.d(0xF), as = a.s(), bd = b.d(0xF), bs = b.s(), qd = q.d(0xF), qs = q.s();
    const Operand rd = res.d(0xF), rs = res.s(), cs = c.s(), cd = c.d(0xF), fs = size_f.s(), us = size.s(),
                  ud = size.d(0xF), ms = mode.s();
    const Operand zero = imm(0), minus1 = imm(0xFFFFFFFFu), fm1 = fimm(-1.0f), two = fimm(2.0f);
    // res = clamp(c, 0, size - 1) (CLAMP; also the range guard of the rest).
    i(Sm4Opcode::IADD, {&qd, &us, &minus1});
    i(Sm4Opcode::IMAX, {&rd, &cs, &zero});
    i(Sm4Opcode::IMIN, {&rd, &rs, &qs});
    // WRAP: c - floor(c / size) * size.
    i(Sm4Opcode::ITOF, {&ad, &cs});
    i(Sm4Opcode::DIV, {&bd, &as, &fs});
    i(Sm4Opcode::ROUND_NI, {&bd, &bs});
    const Operand nb = b.s(kXYZW, 1);
    i(Sm4Opcode::MAD, {&bd, &nb, &fs, &as});
    i(Sm4Opcode::FTOI, {&bd, &bs});
    const Operand mw = imm(kWrap);
    i(Sm4Opcode::IEQ, {&qd, &ms, &mw});
    i(Sm4Opcode::MOVC, {&rd, &qs, &bs, &rs});
    // MIRROR: r = c mod 2size; r >= size ? 2size - 1 - r : r.
    i(Sm4Opcode::MUL, {&qd, &fs, &two});
    i(Sm4Opcode::DIV, {&bd, &as, &qs});
    i(Sm4Opcode::ROUND_NI, {&bd, &bs});
    i(Sm4Opcode::MAD, {&bd, &nb, &qs, &as});  // b = r
    i(Sm4Opcode::ADD, {&qd, &qs, &fm1});
    i(Sm4Opcode::ADD, {&qd, &qs, &nb});        // q = 2size - 1 - r
    i(Sm4Opcode::GE, {&ad, &bs, &fs});
    i(Sm4Opcode::MOVC, {&bd, &as, &qs, &bs});
    i(Sm4Opcode::FTOI, {&bd, &bs});
    const Operand mm = imm(kMirror);
    i(Sm4Opcode::IEQ, {&qd, &ms, &mm});
    i(Sm4Opcode::MOVC, {&rd, &qs, &bs, &rs});
    // MIRROR_ONCE: clamp(c < 0 ? -c - 1 : c).
    i(Sm4Opcode::NOT, {&bd, &cs});  // -c - 1
    i(Sm4Opcode::ILT, {&ad, &cs, &zero});
    i(Sm4Opcode::MOVC, {&bd, &as, &bs, &cs});
    i(Sm4Opcode::IADD, {&ad, &us, &minus1});
    i(Sm4Opcode::IMIN, {&bd, &bs, &as});
    const Operand mo = imm(kMirrorOnce);
    i(Sm4Opcode::IEQ, {&qd, &ms, &mo});
    i(Sm4Opcode::MOVC, {&rd, &qs, &bs, &rs});
    // BORDER: read only when 0 <= c < size.
    i(Sm4Opcode::ULT, {&ad, &cs, &us});
    const Operand mb = imm(kBorder);
    i(Sm4Opcode::INE, {&qd, &ms, &mb});
    i(Sm4Opcode::OR, {&ud, &as, &qs});
    i(Sm4Opcode::MOV, {&cd, &rs});
}

}  // namespace

bool lower_feedback_call(LowerHost& host, u32 opcode, const DxilInstruction& inst) {
    if (host.stage() != ShaderStage::Fragment && (opcode == 174 || opcode == 175)) {
        // Implicit-LOD feedback needs derivatives: pixel shaders only
        // (compute and mesh derivatives are shader model 6.6).
        return host.unimplemented("WriteSamplerFeedback / Bias outside a pixel shader");
    }
    FeedbackLowering l(host, opcode, inst);
    return l.run();
}

}  // namespace vitreo::graphics::dxil_lower
