// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — ARM64 immediate/field encoding range checks

#include "jit_exec_test_fixture.hpp"

// ============================================================================
// ARM64 12-bit immediate fields refuse what they cannot hold
//
// ADD/SUB (immediate) carry imm12 (optionally LSL #12) and LDR/STR (unsigned
// offset) a scaled imm12. The encoders used to mask the value, which encodes a
// DIFFERENT instruction rather than failing: `RET imm16` with imm16 >= 4088
// emitted ADD RSP, RSP, #((8 + imm16) mod 4096). Expected encodings are
// llvm-mc's (`llvm-mc -triple=aarch64 -show-encoding`), not Vitreo's.
// ============================================================================

static_assert(arm64::add_x_imm(0, 1, 4095) == 0x913FFC20u);        // add x0, x1, #4095
static_assert(arm64::add_x_imm_lsl12(0, 1, 1) == 0x91400420u);     // add x0, x1, #1, lsl #12
static_assert(arm64::sub_x_imm(12, 13, 4095) == 0xD13FFDACu);      // sub x12, x13, #4095
static_assert(arm64::sub_x_imm_lsl12(14, 15, 1) == 0xD14005EEu);   // sub x14, x15, #1, lsl #12
static_assert(arm64::subs_x_imm(4, 5, 4095) == 0xF13FFCA4u);       // subs x4, x5, #4095
static_assert(arm64::cmp_x_imm(9, 4095) == 0xF13FFD3Fu);           // cmp x9, #4095
static_assert(arm64::add_w_imm(10, 11, 4095) == 0x113FFD6Au);      // add w10, w11, #4095
static_assert(arm64::sub_w_imm(2, 3, 4095) == 0x513FFC62u);        // sub w2, w3, #4095
static_assert(arm64::adds_w_imm(7, 8, 4095) == 0x313FFD07u);       // adds w7, w8, #4095
static_assert(arm64::cmp_w_imm(6, 4095) == 0x713FFCDFu);           // cmp w6, #4095
static_assert(arm64::ldr_x_uoff(0, 19, 4095) == 0xF97FFE60u);      // ldr x0, [x19, #32760]
static_assert(arm64::str_w_uoff(4, 19, 4095) == 0xB93FFE64u);      // str w4, [x19, #16380]
static_assert(arm64::ldrh_uoff(3, 19, 4095) == 0x797FFE63u);       // ldrh w3, [x19, #8190]
static_assert(arm64::ldrb_uoff(1, 19, 4095) == 0x397FFE61u);       // ldrb w1, [x19, #4095]
static_assert(arm64::str_q_uoff(2, 19, 4095) == 0x3DBFFE62u);      // str q2, [x19, #65520]


// arm64::add_x_uimm24: the release of RET imm16 and the ESP-frame fold. At
// most two ADD (immediate) words, none for Xd = Xd + 0 (llvm-mc encodings).
static_assert(arm64::add_x_uimm24(1, 1, 0).count == 0);
static_assert(arm64::add_x_uimm24(0, 1, 0).count == 1 &&
              arm64::add_x_uimm24(0, 1, 0).word[0] == 0x91000020u);   // add x0, x1, #0
static_assert(arm64::add_x_uimm24(1, 1, 4095).count == 1 &&
              arm64::add_x_uimm24(1, 1, 4095).word[0] == 0x913FFC21u);  // add x1, x1, #4095
static_assert(arm64::add_x_uimm24(1, 1, 4096).count == 1 &&
              arm64::add_x_uimm24(1, 1, 4096).word[0] == 0x91400421u);  // add x1, x1, #1, lsl #12
static_assert(arm64::add_x_uimm24(1, 1, 0x10007).count == 2 &&
              arm64::add_x_uimm24(1, 1, 0x10007).word[0] == 0x91404021u &&  // add x1, x1, #16, lsl #12
              arm64::add_x_uimm24(1, 1, 0x10007).word[1] == 0x91001C21u);   // add x1, x1, #7
static_assert(arm64::add_x_uimm24(1, 1, 0xFFFFFF).count == 2 &&
              arm64::add_x_uimm24(1, 1, 0xFFFFFF).word[0] == 0x917FFC21u && // add x1, x1, #4095, lsl #12
              arm64::add_x_uimm24(1, 1, 0xFFFFFF).word[1] == 0x913FFC21u);  // add x1, x1, #4095

// Death tests re-exec the binary under the threadsafe style (the JIT arena
// and test helpers start threads, which the default fork style is unsafe
// with). The fixture scopes that style to these tests only.
class Arm64Imm12Encoding : public ::testing::Test {
protected:
    void SetUp() override {
        m_saved_style = GTEST_FLAG_GET(death_test_style);
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }
    void TearDown() override {
        GTEST_FLAG_SET(death_test_style, m_saved_style);
    }

private:
    std::string m_saved_style;
};

TEST_F(Arm64Imm12Encoding, OutOfRangeImmediateStopsEmission) {
    // One representative per encoder: the first value the field cannot hold.
    // Each must abort naming itself rather than return a masked word.
    struct Case {
        const char* name;
        u32 (*encode)(u64);
    };
    const Case cases[] = {
        {"add_x_imm", [](u64 v) { return arm64::add_x_imm(0, 1, v); }},
        {"add_x_imm_lsl12", [](u64 v) { return arm64::add_x_imm_lsl12(0, 1, v); }},
        {"sub_x_imm", [](u64 v) { return arm64::sub_x_imm(0, 1, v); }},
        {"sub_x_imm_lsl12", [](u64 v) { return arm64::sub_x_imm_lsl12(0, 1, v); }},
        {"adds_x_imm", [](u64 v) { return arm64::adds_x_imm(0, 1, v); }},
        {"subs_x_imm", [](u64 v) { return arm64::subs_x_imm(0, 1, v); }},
        {"subs_x_imm", [](u64 v) { return arm64::cmp_x_imm(1, v); }},
        {"add_w_imm", [](u64 v) { return arm64::add_w_imm(0, 1, v); }},
        {"sub_w_imm", [](u64 v) { return arm64::sub_w_imm(0, 1, v); }},
        {"adds_w_imm", [](u64 v) { return arm64::adds_w_imm(0, 1, v); }},
        {"subs_w_imm", [](u64 v) { return arm64::subs_w_imm(0, 1, v); }},
        {"cmp_w_imm", [](u64 v) { return arm64::cmp_w_imm(1, v); }},
        {"ldr_w_uoff", [](u64 v) { return arm64::ldr_w_uoff(0, 1, v); }},
        {"str_w_uoff", [](u64 v) { return arm64::str_w_uoff(0, 1, v); }},
        {"ldrsw_uoff", [](u64 v) { return arm64::ldrsw_uoff(0, 1, v); }},
        {"ldr_x_uoff", [](u64 v) { return arm64::ldr_x_uoff(0, 1, v); }},
        {"str_x_uoff", [](u64 v) { return arm64::str_x_uoff(0, 1, v); }},
        {"ldrh_uoff", [](u64 v) { return arm64::ldrh_uoff(0, 1, v); }},
        {"strh_uoff", [](u64 v) { return arm64::strh_uoff(0, 1, v); }},
        {"ldrb_uoff", [](u64 v) { return arm64::ldrb_uoff(0, 1, v); }},
        {"strb_uoff", [](u64 v) { return arm64::strb_uoff(0, 1, v); }},
        {"ldr_s_uoff", [](u64 v) { return arm64::ldr_s_uoff(0, 1, v); }},
        {"str_s_uoff", [](u64 v) { return arm64::str_s_uoff(0, 1, v); }},
        {"ldr_d_uoff", [](u64 v) { return arm64::ldr_d_uoff(0, 1, v); }},
        {"str_d_uoff", [](u64 v) { return arm64::str_d_uoff(0, 1, v); }},
        {"ldr_q_uoff", [](u64 v) { return arm64::ldr_q_uoff(0, 1, v); }},
        {"str_q_uoff", [](u64 v) { return arm64::str_q_uoff(0, 1, v); }},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        // The top of the field still encodes, unmasked.
        EXPECT_EQ((c.encode(4095) >> 10) & 0xFFFu, 0xFFFu);
        EXPECT_DEATH((void)c.encode(4096), c.name);
    }
    // The check sees the caller's full 64-bit value: 2^32 + 5 would have
    // narrowed to #5 through a u32 parameter.
    EXPECT_DEATH((void)arm64::add_x_imm(0, 1, (u64{1} << 32) + 5), "add_x_imm");
    EXPECT_DEATH((void)arm64::ldr_x_uoff(0, 1, ~u64{0}), "ldr_x_uoff");
}

TEST_F(Arm64Imm12Encoding, Uimm24SplitRefusesTwentyFiveBits) {
    EXPECT_DEATH((void)arm64::add_x_uimm24(1, 1, 0x100'0000u), "add_x_uimm24");
}

// ============================================================================
// Every other ARM64 encoder field refuses what it cannot hold
//
// The imm12 lesson applies to every fixed-width field: B/BL imm26, B.cond,
// CBZ/CBNZ imm19, TBZ/TBNZ imm14, LDP/STP imm7, LDUR/STUR imm9, ADR imm21,
// MOVZ/MOVK/MOVN imm16 and hw, shift amounts and immr/imms, bit numbers, lane
// indices, condition codes and logical (bitmask) immediates. Each value below
// is the LAST one its field holds, and every expected word is llvm-mc's
// (`llvm-mc -triple=aarch64 -mattr=+lse,+sha2,+flagm,+v8.4a -show-encoding`),
// never Vitreo's.
// ============================================================================

// PC-relative: the extremes of each displacement field.
static_assert(arm64::b_imm(-134217728) == 0x16000000u);          // b #-134217728
static_assert(arm64::b_imm(134217724) == 0x15FFFFFFu);           // b #134217724
static_assert(arm64::bl_imm(-134217728) == 0x96000000u);         // bl #-134217728
static_assert(arm64::bl_imm(134217724) == 0x95FFFFFFu);          // bl #134217724
static_assert(arm64::b_cond(arm64::NE, -1048576) == 0x54800001u);  // b.ne #-1048576
static_assert(arm64::b_cond(0xF, 1048572) == 0x547FFFEFu);       // b.nv #1048572
static_assert(arm64::cbz_x(1, 1048572) == 0xB47FFFE1u);          // cbz x1, #1048572
static_assert(arm64::cbnz_w(2, -1048576) == 0x35800002u);        // cbnz w2, #-1048576
static_assert(arm64::cbz_w(3, -4) == 0x34FFFFE3u);               // cbz w3, #-4
static_assert(arm64::cbnz_x(4, 8) == 0xB5000044u);               // cbnz x4, #8
static_assert(arm64::tbz(3, 63, -32768) == 0xB6FC0003u);         // tbz x3, #63, #-32768
static_assert(arm64::tbnz(5, 31, 32764) == 0x37FBFFE5u);         // tbnz w5, #31, #32764
static_assert(arm64::tbz(0, 0, 4) == 0x36000020u);               // tbz w0, #0, #4
static_assert(arm64::adr_x(0, -1048576) == 0x10800000u);         // adr x0, #-1048576
static_assert(arm64::adr_x(0, 1048575) == 0x707FFFE0u);          // adr x0, #1048575
static_assert(arm64::adr_x(7, 0) == 0x10000007u);                // adr x7, #0

// LDP/STP imm7 (the encoders take the scaled index) and LDUR/STUR imm9.
static_assert(arm64::ldp_x_off(1, 2, 3, -64) == 0xA9600861u);    // ldp x1, x2, [x3, #-512]
static_assert(arm64::stp_x_off(1, 2, 3, 63) == 0xA91F8861u);     // stp x1, x2, [x3, #504]
static_assert(arm64::stp_w_off(1, 2, 3, 63) == 0x291F8861u);     // stp w1, w2, [x3, #252]
static_assert(arm64::stp_w_off(1, 2, 3, -64) == 0x29200861u);    // stp w1, w2, [x3, #-256]
static_assert(arm64::ldp_q_off(0, 1, 3, 63) == 0xAD5F8460u);     // ldp q0, q1, [x3, #1008]
static_assert(arm64::stp_q_off(0, 1, 3, -64) == 0xAD200460u);    // stp q0, q1, [x3, #-1024]
static_assert(arm64::ldur_x(1, 2, -256) == 0xF8500041u);         // ldur x1, [x2, #-256]
static_assert(arm64::stur_x(1, 2, 255) == 0xF80FF041u);          // stur x1, [x2, #255]

// MOVZ/MOVK/MOVN: imm16 and the top hw of each width.
static_assert(arm64::movz_w_shift(1, 0xFFFF, 16) == 0x52BFFFE1u);  // mov w1, #-65536
static_assert(arm64::movk_w(1, 0xFFFF, 16) == 0x72BFFFE1u);      // movk w1, #65535, lsl #16
static_assert(arm64::movn_w(1, 0xFFFF, 16) == 0x12BFFFE1u);      // movn w1, #65535, lsl #16
static_assert(arm64::movk_x(1, 0xFFFF, 48) == 0xF2FFFFE1u);      // movk x1, #65535, lsl #48
static_assert(arm64::movz_x(1, 0xFFFF, 48) == 0xD2FFFFE1u);      // movz x1, #65535, lsl #48
static_assert(arm64::movn_x(1, 0x1234, 32) == 0x92C24681u);      // movn x1, #0x1234, lsl #32

// Shifts, rotates and bitfields at the top of their ranges.
static_assert(arm64::lsl_w_imm(0, 1, 31) == 0x53010020u);        // lsl w0, w1, #31
static_assert(arm64::lsl_x_imm(0, 1, 63) == 0xD3410020u);        // lsl x0, x1, #63
static_assert(arm64::lsr_w_imm(0, 1, 31) == 0x531F7C20u);        // lsr w0, w1, #31
static_assert(arm64::lsr_x_imm(0, 1, 63) == 0xD37FFC20u);        // lsr x0, x1, #63
static_assert(arm64::asr_w_imm(0, 1, 31) == 0x131F7C20u);        // asr w0, w1, #31
static_assert(arm64::asr_x_imm(0, 1, 63) == 0x937FFC20u);        // asr x0, x1, #63
static_assert(arm64::ror_w_imm(0, 1, 31) == 0x13817C20u);        // ror w0, w1, #31
static_assert(arm64::ror_x_imm(0, 1, 63) == 0x93C1FC20u);        // ror x0, x1, #63
static_assert(arm64::extr_x(0, 1, 2, 63) == 0x93C2FC20u);        // extr x0, x1, x2, #63
static_assert(arm64::extr_w(0, 1, 2, 31) == 0x13827C20u);        // extr w0, w1, w2, #31
static_assert(arm64::ubfx_x(0, 1, 63, 1) == 0xD37FFC20u);        // ubfx x0, x1, #63, #1
static_assert(arm64::ubfx_x(0, 1, 0, 64) == 0xD340FC20u);        // ubfx x0, x1, #0, #64
static_assert(arm64::ubfx_w(0, 1, 31, 1) == 0x531F7C20u);        // ubfx w0, w1, #31, #1
static_assert(arm64::ubfx_w(0, 1, 0, 32) == 0x53007C20u);        // ubfx w0, w1, #0, #32
static_assert(arm64::bfi_x(0, 1, 63, 1) == 0xB3410020u);         // bfi x0, x1, #63, #1
static_assert(arm64::bfi_x(0, 1, 0, 64) == 0xB340FC20u);         // bfi x0, x1, #0, #64
static_assert(arm64::bfi_w(0, 1, 31, 1) == 0x33010020u);         // bfi w0, w1, #31, #1
static_assert(arm64::bfi_w(0, 1, 0, 32) == 0x33007C20u);         // bfi w0, w1, #0, #32
static_assert(arm64::sbfm_x(0, 1, 63, 63) == 0x937FFC20u);       // sbfm x0, x1, #63, #63
static_assert(arm64::add_x_lsl(0, 1, 2, 63) == 0x8B02FC20u);     // add x0, x1, x2, lsl #63
static_assert(arm64::add_w_lsl(0, 1, 2, 31) == 0x0B027C20u);     // add w0, w1, w2, lsl #31
static_assert(arm64::orr_w_lsl(0, 1, 2, 31) == 0x2A027C20u);     // orr w0, w1, w2, lsl #31
static_assert(arm64::orr_x_lsl(0, 1, 2, 63) == 0xAA02FC20u);     // orr x0, x1, x2, lsl #63
static_assert(arm64::orr_x_lsr(0, 1, 2, 63) == 0xAA42FC20u);     // orr x0, x1, x2, lsr #63
static_assert(arm64::eor_x_lsr(0, 1, 2, 63) == 0xCA42FC20u);     // eor x0, x1, x2, lsr #63
static_assert(arm64::ands_w_lsl(0, 1, 2, 31) == 0x6A027C20u);    // ands w0, w1, w2, lsl #31

// Conditions, RMIF, atomics' size, SHA1 op.
static_assert(arm64::csel_x(0, 1, 2, 0xF) == 0x9A82F020u);       // csel x0, x1, x2, nv
static_assert(arm64::csel_w(0, 1, 2, arm64::AL) == 0x1A82E020u);  // csel w0, w1, w2, al
static_assert(arm64::cset_w(0, arm64::LE) == 0x1A9FC7E0u);       // cset w0, le
static_assert(arm64::cset_w(0, arm64::EQ) == 0x1A9F17E0u);       // cset w0, eq
static_assert(arm64::rmif(1, 63, 15) == 0xBA1F842Fu);            // rmif x1, #63, #15
static_assert(arm64::lse_al(arm64::LseOp::Add, 0, 1, 2, 3) == 0x38E10062u);  // ldaddalb w1, w2, [x3]
static_assert(arm64::casal(3, 1, 2, 3) == 0xC8E1FC62u);          // casal x1, x2, [x3]
static_assert(arm64::sha1_hash(2, 0, 3, 1) == 0x5E012060u);      // sha1m q0, s3, v1.4s

// Vector shift immediates, lanes, EXT, MOVI, widen/narrow sizes.
static_assert(arm64::shl_v_imm(0, 1, 63, arm64::VS_D) == 0x4F7F5420u);   // shl v0.2d, v1.2d, #63
static_assert(arm64::shl_v_imm(0, 1, 7, arm64::VS_B) == 0x4F0F5420u);    // shl v0.16b, v1.16b, #7
static_assert(arm64::ushr_v_imm(0, 1, 8, arm64::VS_B) == 0x6F080420u);   // ushr v0.16b, v1.16b, #8
static_assert(arm64::ushr_v_imm(0, 1, 1, arm64::VS_D) == 0x6F7F0420u);   // ushr v0.2d, v1.2d, #1
static_assert(arm64::sshr_v_imm(0, 1, 64, arm64::VS_D) == 0x4F400420u);  // sshr v0.2d, v1.2d, #64
static_assert(arm64::sshr_v_imm(0, 1, 16, arm64::VS_H) == 0x4F100420u);  // sshr v0.8h, v1.8h, #16
static_assert(arm64::ins_v_elem(0, 15, 1, 15, arm64::VS_B) == 0x6E1F7C20u);  // mov v0.b[15], v1.b[15]
static_assert(arm64::ins_v_elem(0, 1, 1, 1, arm64::VS_D) == 0x6E184420u);    // mov v0.d[1], v1.d[1]
static_assert(arm64::ins_v_gpr(0, 15, 1, arm64::VS_B) == 0x4E1F1C20u);   // mov v0.b[15], w1
static_assert(arm64::ins_v_gpr(0, 1, 1, arm64::VS_D) == 0x4E181C20u);    // mov v0.d[1], x1
static_assert(arm64::ins_v_gpr(0, 3, 1, arm64::VS_S) == 0x4E1C1C20u);    // mov v0.s[3], w1
static_assert(arm64::umov_w_b(0, 1, 15) == 0x0E1F3C20u);         // umov w0, v1.b[15]
static_assert(arm64::umov_w_h(0, 1, 7) == 0x0E1E3C20u);          // umov w0, v1.h[7]
static_assert(arm64::umov_w_s(0, 1, 3) == 0x0E1C3C20u);          // mov w0, v1.s[3]
static_assert(arm64::umov_x_d(0, 1, 1) == 0x4E183C20u);          // mov x0, v1.d[1]
static_assert(arm64::dup_v_elem_s(0, 1, 3) == 0x4E1C0420u);      // dup v0.4s, v1.s[3]
static_assert(arm64::ext_v16b(0, 1, 2, 15) == 0x6E027820u);      // ext v0.16b, v1.16b, v2.16b, #15
static_assert(arm64::movi_v16b(0, 0xFF) == 0x4F07E7E0u);         // movi v0.16b, #255
static_assert(arm64::sxtl_v(0, 1, arm64::VS_D) == 0x0F20A420u);  // sxtl v0.2d, v1.2s
static_assert(arm64::uxtl_v(0, 1, arm64::VS_H) == 0x2F08A420u);  // uxtl v0.8h, v1.8b
static_assert(arm64::sqxtn_v(0, 1, arm64::VS_D) == 0x0EA14820u);   // sqxtn v0.2s, v1.2d
static_assert(arm64::sqxtun2_v(0, 1, arm64::VS_H) == 0x6E212820u); // sqxtun2 v0.16b, v1.8h
static_assert(arm64::xtn_v(0, 1, arm64::VS_S) == 0x0EA12820u);   // xtn v0.2s, v1.2d
static_assert(arm64::add_v(0, 1, 2, arm64::VS_D) == 0x4EE28420u);  // add v0.2d, v1.2d, v2.2d
static_assert(arm64::dup_v_gpr(0, 1, arm64::VS_D) == 0x4E080C20u);  // dup v0.2d, x1

// Logical (bitmask) immediates: the masks the JIT uses, and the shapes that
// exercise each part of N:immr:imms (element size, rotation, run length).
static_assert(arm64::and_w_imm(0, 1, 0xFF) == 0x12001C20u);       // and w0, w1, #0xff
static_assert(arm64::and_w_imm(0, 0, 0x1) == 0x12000000u);        // and w0, w0, #0x1
static_assert(arm64::and_w_imm(0, 0, 0x1F) == 0x12001000u);       // and w0, w0, #0x1f
static_assert(arm64::and_w_imm(0, 0, 0xFFFF) == 0x12003C00u);     // and w0, w0, #0xffff
static_assert(arm64::and_w_imm(0, 0, 0x55555555) == 0x1200F000u); // and w0, w0, #0x55555555
static_assert(arm64::and_w_imm(0, 0, 0xFFFFFFFE) == 0x121F7800u); // and w0, w0, #0xfffffffe
static_assert(arm64::and_w_imm(0, 0, 0x80000001) == 0x12010400u); // and w0, w0, #0x80000001
static_assert(arm64::and_w_imm(0, 0, 0xF0F0F0F0) == 0x1204CC00u); // and w0, w0, #0xf0f0f0f0
static_assert(arm64::and_w_imm(0, 0, 0x7FFFFFFF) == 0x12007800u); // and w0, w0, #0x7fffffff
// The first value past each field, in a constant expression: an encoder that
// refuses calls imm_field_overflow (not constexpr), so the call is not a
// constant expression and `encodes` is false. The masking encoders this
// replaces returned a word for every one of these.
template <auto F>
constexpr bool encodes = requires { typename std::integral_constant<u32, F()>; };

// Displacements: one past each end, and a byte offset that is not a word.
static_assert(!encodes<[] { return arm64::b_imm(134217728); }>);
static_assert(!encodes<[] { return arm64::b_imm(-134217732); }>);
static_assert(!encodes<[] { return arm64::b_imm(2); }>);
static_assert(!encodes<[] { return arm64::bl_imm(134217728); }>);
static_assert(!encodes<[] { return arm64::bl_imm(-134217732); }>);
static_assert(!encodes<[] { return arm64::b_cond(arm64::NE, 1048576); }>);
static_assert(!encodes<[] { return arm64::b_cond(arm64::NE, -1048580); }>);
static_assert(!encodes<[] { return arm64::b_cond(16, 0); }>);
static_assert(!encodes<[] { return arm64::cbz_w(0, 1048576); }>);
static_assert(!encodes<[] { return arm64::cbz_x(0, -1048580); }>);
static_assert(!encodes<[] { return arm64::cbnz_w(0, 1048576); }>);
static_assert(!encodes<[] { return arm64::cbnz_x(0, 1048576); }>);
static_assert(!encodes<[] { return arm64::cbnz_x(0, 6); }>);
static_assert(!encodes<[] { return arm64::tbz(0, 0, 32768); }>);
static_assert(!encodes<[] { return arm64::tbnz(0, 0, -32772); }>);
static_assert(!encodes<[] { return arm64::tbz(0, 64, 0); }>);
static_assert(!encodes<[] { return arm64::tbnz(0, 64, 0); }>);
static_assert(!encodes<[] { return arm64::adr_x(0, 1048576); }>);
static_assert(!encodes<[] { return arm64::adr_x(0, -1048577); }>);
// A 64-bit distance that narrows into range through an i32 parameter.
static_assert(!encodes<[] { return arm64::b_imm((i64{1} << 32) + 8); }>);

// LDP/STP imm7, LDUR/STUR imm9.
static_assert(!encodes<[] { return arm64::ldp_x_off(1, 2, 3, 64); }>);
static_assert(!encodes<[] { return arm64::ldp_x_off(1, 2, 3, -65); }>);
static_assert(!encodes<[] { return arm64::stp_x_off(1, 2, 3, -65); }>);
static_assert(!encodes<[] { return arm64::stp_w_off(1, 2, 3, 64); }>);
static_assert(!encodes<[] { return arm64::ldp_q_off(0, 1, 3, 64); }>);
static_assert(!encodes<[] { return arm64::stp_q_off(0, 1, 3, 64); }>);
static_assert(!encodes<[] { return arm64::ldur_x(1, 2, 256); }>);
static_assert(!encodes<[] { return arm64::stur_x(1, 2, -257); }>);

// MOVZ/MOVK/MOVN imm16 and hw.
static_assert(!encodes<[] { return arm64::movz_w(1, 0x10000); }>);
static_assert(!encodes<[] { return arm64::movz_w_shift(1, 0x10000, 0); }>);
static_assert(!encodes<[] { return arm64::movz_w_shift(1, 1, 32); }>);
static_assert(!encodes<[] { return arm64::movz_w_shift(1, 1, 8); }>);
static_assert(!encodes<[] { return arm64::movn_w(1, 1, 32); }>);
static_assert(!encodes<[] { return arm64::movk_w(1, 1, 32); }>);
static_assert(!encodes<[] { return arm64::movk_w(1, 0x10000, 16); }>);
static_assert(!encodes<[] { return arm64::movz_x(1, 1, 64); }>);
static_assert(!encodes<[] { return arm64::movz_x(1, 1, 24); }>);
static_assert(!encodes<[] { return arm64::movk_x(1, 0x10000, 48); }>);
static_assert(!encodes<[] { return arm64::movn_x(1, 1, 64); }>);

// Shift amounts, rotates and bitfields.
static_assert(!encodes<[] { return arm64::lsl_w_imm(0, 1, 32); }>);
static_assert(!encodes<[] { return arm64::lsl_x_imm(0, 1, 64); }>);
static_assert(!encodes<[] { return arm64::lsr_w_imm(0, 1, 32); }>);
static_assert(!encodes<[] { return arm64::lsr_x_imm(0, 1, 64); }>);
static_assert(!encodes<[] { return arm64::asr_w_imm(0, 1, 32); }>);
static_assert(!encodes<[] { return arm64::asr_x_imm(0, 1, 64); }>);
static_assert(!encodes<[] { return arm64::ror_w_imm(0, 1, 32); }>);
static_assert(!encodes<[] { return arm64::ror_x_imm(0, 1, 64); }>);
static_assert(!encodes<[] { return arm64::extr_x(0, 1, 2, 64); }>);
static_assert(!encodes<[] { return arm64::extr_w(0, 1, 2, 32); }>);
static_assert(!encodes<[] { return arm64::ubfx_x(0, 1, 63, 2); }>);
static_assert(!encodes<[] { return arm64::ubfx_x(0, 1, 0, 0); }>);
static_assert(!encodes<[] { return arm64::ubfx_x(0, 1, 64, 1); }>);
static_assert(!encodes<[] { return arm64::ubfx_w(0, 1, 31, 2); }>);
static_assert(!encodes<[] { return arm64::ubfx_w(0, 1, 32, 1); }>);
static_assert(!encodes<[] { return arm64::bfi_w(0, 1, 31, 2); }>);
static_assert(!encodes<[] { return arm64::bfi_w(0, 1, 0, 0); }>);
static_assert(!encodes<[] { return arm64::bfi_x(0, 1, 63, 2); }>);
static_assert(!encodes<[] { return arm64::sbfm_x(0, 1, 64, 0); }>);
static_assert(!encodes<[] { return arm64::sbfm_x(0, 1, 0, 64); }>);
static_assert(!encodes<[] { return arm64::add_x_lsl(0, 1, 2, 64); }>);
static_assert(!encodes<[] { return arm64::add_w_lsl(0, 1, 2, 32); }>);
static_assert(!encodes<[] { return arm64::orr_w_lsl(0, 1, 2, 32); }>);
static_assert(!encodes<[] { return arm64::orr_x_lsl(0, 1, 2, 64); }>);
static_assert(!encodes<[] { return arm64::orr_x_lsr(0, 1, 2, 64); }>);
static_assert(!encodes<[] { return arm64::eor_x_lsr(0, 1, 2, 64); }>);
static_assert(!encodes<[] { return arm64::ands_w_lsl(0, 1, 2, 32); }>);

// Conditions, RMIF, atomics' size, SHA1 op.
static_assert(!encodes<[] { return arm64::csel_x(0, 1, 2, 16); }>);
static_assert(!encodes<[] { return arm64::csel_w(0, 1, 2, 16); }>);
static_assert(!encodes<[] { return arm64::cset_w(0, arm64::AL); }>);
static_assert(!encodes<[] { return arm64::cset_w(0, 0xF); }>);
static_assert(!encodes<[] { return arm64::rmif(1, 64, 0); }>);
static_assert(!encodes<[] { return arm64::rmif(1, 0, 16); }>);
static_assert(!encodes<[] { return arm64::lse_al(arm64::LseOp::Add, 4, 1, 2, 3); }>);
static_assert(!encodes<[] { return arm64::casal(4, 1, 2, 3); }>);
static_assert(!encodes<[] { return arm64::sha1_hash(3, 0, 3, 1); }>);

// Vector shift immediates, lanes, EXT, MOVI, element sizes.
static_assert(!encodes<[] { return arm64::shl_v_imm(0, 1, 8, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::shl_v_imm(0, 1, 64, arm64::VS_D); }>);
static_assert(!encodes<[] { return arm64::ushr_v_imm(0, 1, 0, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::ushr_v_imm(0, 1, 9, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::sshr_v_imm(0, 1, 65, arm64::VS_D); }>);
static_assert(!encodes<[] { return arm64::sshr_v_imm(0, 1, 0, arm64::VS_D); }>);
static_assert(!encodes<[] { return arm64::ins_v_elem(0, 16, 1, 0, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::ins_v_elem(0, 0, 1, 2, arm64::VS_D); }>);
static_assert(!encodes<[] { return arm64::ins_v_gpr(0, 4, 1, arm64::VS_S); }>);
static_assert(!encodes<[] { return arm64::ins_v_gpr(0, 2, 1, arm64::VS_D); }>);
static_assert(!encodes<[] { return arm64::umov_w_b(0, 1, 16); }>);
static_assert(!encodes<[] { return arm64::umov_w_h(0, 1, 8); }>);
static_assert(!encodes<[] { return arm64::umov_w_s(0, 1, 4); }>);
static_assert(!encodes<[] { return arm64::umov_x_d(0, 1, 2); }>);
static_assert(!encodes<[] { return arm64::dup_v_elem_s(0, 1, 4); }>);
static_assert(!encodes<[] { return arm64::ext_v16b(0, 1, 2, 16); }>);
static_assert(!encodes<[] { return arm64::movi_v16b(0, 0x100); }>);
static_assert(!encodes<[] { return arm64::sxtl_v(0, 1, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::uxtl_v(0, 1, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::sqxtn_v(0, 1, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::sqxtun2_v(0, 1, arm64::VS_B); }>);
static_assert(!encodes<[] { return arm64::xtn_v(0, 1, arm64::VS_D); }>);
static_assert(!encodes<[] { return arm64::add_v(0, 1, 2, static_cast<arm64::VSize>(4)); }>);
static_assert(!encodes<[] { return arm64::fadd_v(0, 1, 2, static_cast<arm64::FSize>(2)); }>);
static_assert(!encodes<[] { return arm64::dup_v_gpr(0, 1, static_cast<arm64::VSize>(4)); }>);
static_assert(!encodes<[] { return arm64::dup_v_elem0(0, 1, static_cast<arm64::VSize>(4)); }>);

// Logical immediates: zero, all-ones, two runs, a bit past the datasize.
static_assert(!encodes<[] { return arm64::and_w_imm(0, 1, 0); }>);
static_assert(!encodes<[] { return arm64::and_w_imm(0, 1, 0xFFFFFFFF); }>);
static_assert(!encodes<[] { return arm64::and_w_imm(0, 1, 0x5); }>);
static_assert(!encodes<[] { return arm64::and_w_imm(0, 1, 0xFF00FF0F); }>);
static_assert(!encodes<[] { return arm64::and_w_imm(0, 1, u64{1} << 32); }>);

// A hot-to-stub test whose stub may sit past TBNZ's reach: TBNZ within
// +-32 KiB, the same-length CBNZ beyond it (llvm-mc words).
static_assert(arm64::tbnz_or_cbnz_x(3, 0, 32764) == 0x3703FFE3u);   // tbnz w3, #0, #32764
static_assert(arm64::tbnz_or_cbnz_x(3, 0, 32768) == 0xB5040003u);   // cbnz x3, #32768
static_assert(arm64::tbnz_or_cbnz_x(3, 0, -32768) == 0x37040003u);  // tbnz w3, #0, #-32768
static_assert(arm64::tbnz_or_cbnz_x(3, 0, -32772) == 0xB5FBFFE3u);  // cbnz x3, #-32772
static_assert(arm64::tbnz_or_cbnz_x(3, 63, 4) == 0xB7F80023u);      // tbnz x3, #63, #4

// The same refusals at run time: each checked-field kind stops emission and
// names the encoder (one representative per helper and reporter).
TEST_F(Arm64Imm12Encoding, EveryFieldKindStopsEmissionOutOfRange) {
    struct Case {
        const char* name;
        u32 (*encode)();
    };
    const Case cases[] = {
        {"b_imm", [] { return arm64::b_imm(134217728); }},                 // imm26
        {"cbz_w", [] { return arm64::cbz_w(0, 2); }},                      // not a word
        {"b_cond", [] { return arm64::b_cond(arm64::EQ, 1048576); }},      // imm19
        {"tbz", [] { return arm64::tbz(0, 0, 32768); }},                   // imm14
        {"adr_x", [] { return arm64::adr_x(0, 1048576); }},                // imm21
        {"ldp_x_off", [] { return arm64::ldp_x_off(1, 2, 3, 64); }},       // imm7
        {"stur_x", [] { return arm64::stur_x(1, 2, -257); }},              // imm9
        // A 64-bit value that a u16 parameter would have narrowed to #5.
        {"movz_w", [] { return arm64::movz_w(1, (u64{1} << 32) + 5); }},   // imm16
        {"movk_x", [] { return arm64::movk_x(1, 1, 24); }},                // hw
        {"lsl_x_imm", [] { return arm64::lsl_x_imm(0, 1, 64); }},          // shift
        {"ubfx_w", [] { return arm64::ubfx_w(0, 1, 0, 0); }},              // width
        {"cset_w", [] { return arm64::cset_w(0, arm64::AL); }},            // cond
        {"umov_w_b", [] { return arm64::umov_w_b(0, 1, 16); }},            // lane
        {"and_w_imm", [] { return arm64::and_w_imm(0, 1, 5); }},           // bitmask
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        EXPECT_DEATH((void)c.encode(), c.name);
    }
}

// A deferred branch patch that cannot reach its target declines its block
// (JitCompiler::cold_branch_disp) rather than stopping the process: the
// displacement helper reports it and returns a placeholder that encodes.
TEST(JitDeferredBranchReach, OutOfFieldDisplacementIsReportedNotMasked) {
    struct Case {
        usize from, to;
        unsigned bits;
        bool exceeded;
        i32 disp;
    };
    const Case cases[] = {
        {100, 100 + 8191, 14, false, 8191 * 4},     // TBZ's last forward word
        {100, 100 + 8192, 14, true, 0},             // one past it
        {8192, 0, 14, false, -8192 * 4},            // TBZ's last backward word
        {8193, 0, 14, true, 0},
        {0, (usize{1} << 18) - 1, 19, false, ((1 << 18) - 1) * 4},
        {0, usize{1} << 18, 19, true, 0},           // 1 MiB: past imm19
        {0, (usize{1} << 25) - 1, 26, false, ((1 << 25) - 1) * 4},
        {0, usize{1} << 25, 26, true, 0},           // 128 MiB: past imm26
    };
    for (const Case& c : cases) {
        bool exceeded = false;
        const i32 disp = deferred_branch_disp(c.from, c.to, c.bits, exceeded);
        EXPECT_EQ(exceeded, c.exceeded) << c.from << " -> " << c.to;
        EXPECT_EQ(disp, c.disp) << c.from << " -> " << c.to;
    }
}
