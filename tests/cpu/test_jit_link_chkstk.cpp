// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — linked __chkstk calls and the GS cookie slot

#include "jit_exec_test_fixture.hpp"

TEST_F(JitExecTest, LinkedChkStkCallUpdatesEspForGsCookieSlot) {
    // MSVC __chkstk is a CALL that returns with ESP reduced by EAX. The
    // caller then writes its /GS cookie at [esp+size-8]. An RSB hop that
    // keeps the pre-call ESP pin stores that cookie in the wrong slot and
    // the matching check __fastfail(2)s — Battle.net Qt5Gui hits this on
    // the 0x2034 image-convert frame.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    auto compile_publish = [&](GuestAddr address, const u8* code,
                               usize length) {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        ASSERT_NE(cached, nullptr);
        publish_compiled(*cached, *compiled);
    };

    // Qt5Gui-shaped short-path __chkstk (no page-probe loop).
    const u8 chkstk[] = {
        0x51, 0x8D, 0x4C, 0x24, 0x04, 0x2B, 0xC8, 0x1B, 0xC0,
        0xF7, 0xD0, 0x23, 0xC8, 0x8B, 0xC1, 0x59, 0x94, 0x8B,
        0x00, 0x89, 0x04, 0x24, 0xC3,
    };
    compile_publish(0x2000, chkstk, sizeof(chkstk));

    // Return site: write a sentinel where MSVC plants the cookie.
    const u8 ret_site[] = {
        0xC7, 0x84, 0x24, 0x2C, 0x20, 0x00, 0x00,
        0x44, 0x33, 0x22, 0x11, // mov dword [esp+0x202c], 0x11223344
    };
    compile_publish(0x100A, ret_site, sizeof(ret_site));

    const u8 caller[] = {
        0xB8, 0x34, 0x20, 0x00, 0x00, // mov eax, 0x2034
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x2000
    };
    auto caller_block = decode_at(0x1000, caller, sizeof(caller));
    auto* caller_compiled = m_jit->compile(caller_block);
    ASSERT_NE(caller_compiled, nullptr);

    constexpr u32 kEsp0 = 0x10000;
    constexpr u32 kNeed = 0x2034;
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(ECX, 0xA5A5A5A5);
    m_cpu->gpr.set32(ESP, kEsp0);
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *caller_compiled, *m_cpu),
              JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(ESP), kEsp0 - kNeed);
    EXPECT_EQ(m_cpu->rip, 0x100Au + sizeof(ret_site));
    const auto cookie = m_memory->read_u32(kEsp0 - kNeed + 0x202c);
    ASSERT_TRUE(cookie.is_ok());
    EXPECT_EQ(cookie.value(), 0x11223344u);
}

TEST_F(JitExecTest, LinkedChkStkProbeCallUpdatesEspForGsCookieSlot) {
    // Qt5Gui __chkstk takes the page-probe loop when the reservation
    // crosses a page: `cmp ecx, eax; jb probe` then
    // `sub eax, 0x1000; test [eax], eax; jmp cmp` (Intel SDM Vol. 2 TEST:
    // AND, flags only, no store). A linked hop that re-enters the entry
    // block, or a TEST/[EAX] lowering that clobbers ECX, plants the /GS
    // cookie in the wrong slot and the matching check __fastfail(2)s.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    std::array<CompiledBlock*, 8> compiled_at{};
    std::array<GuestAddr, 8> compiled_addr{};
    usize compiled_n = 0;
    auto compile_publish = [&](GuestAddr address, const u8* code,
                               usize length) {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        ASSERT_NE(cached, nullptr);
        publish_compiled(*cached, *compiled);
        ASSERT_LT(compiled_n, compiled_at.size());
        compiled_at[compiled_n] = compiled;
        compiled_addr[compiled_n] = address;
        ++compiled_n;
    };

    // Full MSVC __chkstk, including the probe loop after RET.
    const u8 chkstk[] = {
        0x51, 0x8D, 0x4C, 0x24, 0x04, 0x2B, 0xC8, 0x1B, 0xC0, 0xF7,
        0xD0, 0x23, 0xC8, 0x8B, 0xC4, 0x25, 0x00, 0xF0, 0xFF, 0xFF,
        0x3B, 0xC8, 0x72, 0x0A, 0x8B, 0xC1, 0x59, 0x94, 0x8B, 0x00,
        0x89, 0x04, 0x24, 0xC3, 0x2D, 0x00, 0x10, 0x00, 0x00, 0x85,
        0x00, 0xEB, 0xE9,
    };
    compile_publish(0x2000, chkstk, sizeof(chkstk));
    // JMP target is the CMP loop header, not the function entry.
    compile_publish(0x2014, chkstk + 0x14, sizeof(chkstk) - 0x14);
    compile_publish(0x2022, chkstk + 0x22, sizeof(chkstk) - 0x22);

    const u8 ret_site[] = {
        0xC7, 0x84, 0x24, 0x2C, 0x20, 0x00, 0x00,
        0x44, 0x33, 0x22, 0x11, // mov dword [esp+0x202c], 0x11223344
    };
    compile_publish(0x100A, ret_site, sizeof(ret_site));

    const u8 caller[] = {
        0xB8, 0x34, 0x20, 0x00, 0x00, // mov eax, 0x2034
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x2000
    };
    compile_publish(0x1000, caller, sizeof(caller));
    CompiledBlock* caller_compiled = nullptr;
    for (usize i = 0; i < compiled_n; ++i) {
        if (compiled_addr[i] == 0x1000) caller_compiled = compiled_at[i];
    }
    ASSERT_NE(caller_compiled, nullptr);

    constexpr u32 kEsp0 = 0x10000;
    constexpr u32 kNeed = 0x2034;
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(ECX, 0xA5A5A5A5);
    m_cpu->gpr.set32(ESP, kEsp0);
    m_cpu->rip = 0x1000;

    auto* compiled = caller_compiled;
    for (int hop = 0; hop < 16; ++hop) {
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
        if (m_cpu->rip == 0x100Au + sizeof(ret_site)) {
            break;
        }
        CompiledBlock* next = nullptr;
        for (usize i = 0; i < compiled_n; ++i) {
            if (compiled_addr[i] == static_cast<GuestAddr>(m_cpu->rip)) {
                next = compiled_at[i];
                break;
            }
        }
        ASSERT_NE(next, nullptr) << "rip=" << std::hex << m_cpu->rip;
        compiled = next;
    }

    EXPECT_EQ(m_cpu->gpr.get32(ESP), kEsp0 - kNeed);
    EXPECT_EQ(m_cpu->gpr.get32(ECX), 0xA5A5A5A5u);
    EXPECT_EQ(m_cpu->rip, 0x100Au + sizeof(ret_site));
    const auto cookie = m_memory->read_u32(kEsp0 - kNeed + 0x202c);
    ASSERT_TRUE(cookie.is_ok());
    EXPECT_EQ(cookie.value(), 0x11223344u);
}

TEST_F(JitExecTest, LinkedChkStkThenPushLeaPreservesGsCookie) {
    // After __chkstk the convert frame plants the cookie, then
    // `push ebx; push ebp; push esi; lea ebp, [esp+0x34]`. EBP is the
    // 2048-pixel buffer. If LEA sees the pre-push ESP, the buffer overlaps
    // [esp+0x202c] and a full 0x2000 store __fastfail(2)s.
    struct LinkGuard {
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    BlockCache cache;
    m_jit->set_block_cache(&cache);
    JitCompiler::enable_jit_link();

    auto decode_at = [&](GuestAddr address, const u8* code,
                         usize length) -> BasicBlock {
        EXPECT_TRUE(m_memory->write_bytes(address, code, length));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, length);
        while (reader.remaining() > 0) {
            DecodedInstruction instruction;
            if (decode_instruction(reader, DecodeMode::Mode32, instruction) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(instruction);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };

    std::array<CompiledBlock*, 8> compiled_at{};
    std::array<GuestAddr, 8> compiled_addr{};
    usize compiled_n = 0;
    auto compile_publish = [&](GuestAddr address, const u8* code,
                               usize length) {
        auto block = decode_at(address, code, length);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        cache.insert(std::move(block));
        auto* cached = cache.lookup(address);
        ASSERT_NE(cached, nullptr);
        publish_compiled(*cached, *compiled);
        ASSERT_LT(compiled_n, compiled_at.size());
        compiled_at[compiled_n] = compiled;
        compiled_addr[compiled_n] = address;
        ++compiled_n;
    };

    const u8 chkstk[] = {
        0x51, 0x8D, 0x4C, 0x24, 0x04, 0x2B, 0xC8, 0x1B, 0xC0, 0xF7,
        0xD0, 0x23, 0xC8, 0x8B, 0xC4, 0x25, 0x00, 0xF0, 0xFF, 0xFF,
        0x3B, 0xC8, 0x72, 0x0A, 0x8B, 0xC1, 0x59, 0x94, 0x8B, 0x00,
        0x89, 0x04, 0x24, 0xC3, 0x2D, 0x00, 0x10, 0x00, 0x00, 0x85,
        0x00, 0xEB, 0xE9,
    };
    compile_publish(0x2000, chkstk, sizeof(chkstk));
    compile_publish(0x2014, chkstk + 0x14, sizeof(chkstk) - 0x14);
    compile_publish(0x2022, chkstk + 0x22, sizeof(chkstk) - 0x22);

    const u8 ret_site[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12,             // mov eax, 0x12345678
        0x33, 0xC4,                               // xor eax, esp
        0x89, 0x84, 0x24, 0x2C, 0x20, 0x00, 0x00, // mov [esp+0x202c], eax
        0x53, 0x55, 0x56,                         // push ebx; push ebp; push esi
        0x8D, 0x6C, 0x24, 0x34,                   // lea ebp, [esp+0x34]
        0xC7, 0x45, 0x00, 0xAB, 0xAB, 0xAB, 0xAB, // mov dword [ebp], 0xABABABAB
        0xC7, 0x85, 0xFC, 0x1F, 0x00, 0x00, 0xCD, 0xCD, 0xCD, 0xCD,
        // mov dword [ebp+0x1FFC], 0xCDCDCDCD  (last of 2048 pixels)
    };
    compile_publish(0x100A, ret_site, sizeof(ret_site));

    const u8 caller[] = {
        0xB8, 0x34, 0x20, 0x00, 0x00, // mov eax, 0x2034
        0xE8, 0xF6, 0x0F, 0x00, 0x00, // call 0x2000
    };
    compile_publish(0x1000, caller, sizeof(caller));
    CompiledBlock* caller_compiled = nullptr;
    for (usize i = 0; i < compiled_n; ++i) {
        if (compiled_addr[i] == 0x1000) caller_compiled = compiled_at[i];
    }
    ASSERT_NE(caller_compiled, nullptr);

    constexpr u32 kEsp0 = 0x10000;
    constexpr u32 kNeed = 0x2034;
    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EBX, 0xB1000001);
    m_cpu->gpr.set32(EBP, 0xB2000002);
    m_cpu->gpr.set32(ESI, 0xB3000003);
    m_cpu->gpr.set32(ESP, kEsp0);
    m_cpu->rip = 0x1000;

    auto* compiled = caller_compiled;
    for (int hop = 0; hop < 16; ++hop) {
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
        if (m_cpu->rip == 0x100Au + sizeof(ret_site)) {
            break;
        }
        CompiledBlock* next = nullptr;
        for (usize i = 0; i < compiled_n; ++i) {
            if (compiled_addr[i] == static_cast<GuestAddr>(m_cpu->rip)) {
                next = compiled_at[i];
                break;
            }
        }
        ASSERT_NE(next, nullptr) << "rip=" << std::hex << m_cpu->rip;
        compiled = next;
    }

    const u32 new_esp = kEsp0 - kNeed;
    EXPECT_EQ(m_cpu->gpr.get32(ESP), new_esp - 12u);
    EXPECT_EQ(m_cpu->gpr.get32(EBP), new_esp - 12u + 0x34u);
    const auto cookie = m_memory->read_u32(new_esp + 0x202c);
    ASSERT_TRUE(cookie.is_ok());
    EXPECT_EQ(cookie.value(), 0x12345678u ^ new_esp);
    const auto first = m_memory->read_u32(new_esp - 12u + 0x34u);
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first.value(), 0xABABABABu);
    const auto last = m_memory->read_u32(new_esp - 12u + 0x34u + 0x1FFCu);
    ASSERT_TRUE(last.is_ok());
    EXPECT_EQ(last.value(), 0xCDCDCDCDu);
}
