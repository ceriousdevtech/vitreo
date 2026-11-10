// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxil_reader.cpp
/// @brief The DXIL container and LLVM 3.7 bitcode reader against dxc.
///
/// Every blob is dxc output from the Windows SDK on winoracle
/// (tools/oracle_probe/d3d12_sm6/gen_d3d12_sm6.py). Each check compares
/// what the reader decoded with dxc's own /Fc disassembly of the same blob,
/// checked in beside the header (tests/oracle_data/d3d12_sm6/*.dxil.txt):
/// the entry function's name, its basic-block count, and per block the
/// instruction sequence -- mnemonic, comparison predicate, and for each
/// dx.op call the opcode -- line for line.

#include <gtest/gtest.h>

#include "graphics/shaders/dxil_container.hpp"
#include "graphics/shaders/dxil_module.hpp"

#include "../oracle_data/d3d12_sm6/d3d12_sm6_shaders.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace vitreo::graphics {
namespace {

std::string read_listing(const std::string& name) {
    std::ifstream in(std::string(VITREO_SOURCE_DIR) + "/tests/oracle_data/d3d12_sm6/" + name +
                     ".dxil.txt");
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

/// One instruction as the listing names it: "add", "icmp eq", "call 59".
using Sequence = std::vector<std::string>;

struct Listing {
    /// Every defined function, in listing order, with its blocks.
    std::vector<std::pair<std::string, std::vector<Sequence>>> functions;
    std::set<std::string> named_metadata;
};

Listing parse_listing(const std::string& text) {
    Listing out;
    std::istringstream in(text);
    std::string line;
    bool in_body = false;
    bool in_switch = false;
    while (std::getline(in, line)) {
        if (!in_body) {
            if (line.rfind("define ", 0) == 0) {
                const auto at = line.find('@');
                const auto paren = line.find('(', at);
                std::string name = line.substr(at + 1, paren - at - 1);
                if (!name.empty() && name.front() == '"') {
                    // A quoted LLVM name: \XX is a hex-escaped byte.
                    std::string raw;
                    for (usize i = 1; i + 1 < name.size(); ++i) {
                        if (name[i] == '\\' && i + 2 < name.size()) {
                            raw.push_back(static_cast<char>(std::stoi(name.substr(i + 1, 2), nullptr, 16)));
                            i += 2;
                        } else {
                            raw.push_back(name[i]);
                        }
                    }
                    name = raw;
                }
                out.functions.emplace_back(name, std::vector<Sequence>(1));
                in_body = true;
            } else if (line.rfind("!", 0) == 0 && line.find(" = !{") != std::string::npos &&
                       !std::isdigit(static_cast<unsigned char>(line[1]))) {
                out.named_metadata.insert(line.substr(1, line.find(' ') - 1));
            }
            continue;
        }
        if (line == "}") {
            in_body = false;
            continue;
        }
        if (in_switch) {
            if (line.find(']') != std::string::npos) in_switch = false;
            continue;
        }
        if (line.rfind("; <label>:", 0) == 0) {
            out.functions.back().second.emplace_back();
            continue;
        }
        std::string s = line;
        const auto first = s.find_first_not_of(' ');
        if (first == std::string::npos || s[first] == ';') continue;
        s = s.substr(first);
        if (s[0] == '%') s = s.substr(s.find(" = ") + 3);
        std::istringstream words(s);
        std::string op;
        words >> op;
        if (op == "tail") words >> op;
        std::string item = op;
        if (op == "icmp" || op == "fcmp") {
            std::string pred;
            words >> pred;
            while (pred == "fast" || pred == "nnan" || pred == "ninf" || pred == "nsz" ||
                   pred == "arcp") {
                words >> pred;
            }
            item += " " + pred;
        } else if (op == "call") {
            const auto at = s.find("@dx.op.");
            if (at != std::string::npos) {
                const auto num = s.find("(i32 ", at);
                item += " " + s.substr(num + 5, s.find_first_of(",)", num + 5) - num - 5);
            } else {
                item += " " + s.substr(s.find('@'), s.find('(', s.find('@')) - s.find('@'));
            }
        } else if (op == "switch") {
            in_switch = s.find(']') == std::string::npos;
        }
        out.functions.back().second.back().push_back(item);
    }
    return out;
}

const char* binop_name(u32 code, bool fp) {
    static const char* const ints[] = {"add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
                                       "shl", "lshr", "ashr", "and", "or", "xor"};
    static const char* const fps[] = {"fadd", "fsub", "fmul", "udiv", "fdiv", "urem", "frem"};
    if (fp && code < 7) return fps[code];
    return code < 13 ? ints[code] : "?";
}

const char* cast_name(u32 code) {
    static const char* const names[] = {"trunc",  "zext",    "sext",     "fptoui", "fptosi",
                                        "uitofp", "sitofp",  "fptrunc",  "fpext",  "ptrtoint",
                                        "inttoptr", "bitcast", "addrspacecast"};
    return code < 13 ? names[code] : "?";
}

const char* predicate_name(u32 p) {
    static const char* const fp[] = {"false", "oeq", "ogt", "oge", "olt", "ole", "one", "ord",
                                     "uno",   "ueq", "ugt", "uge", "ult", "ule", "une", "true"};
    static const char* const ip[] = {"eq", "ne", "ugt", "uge", "ult", "ule", "sgt", "sge",
                                     "slt", "sle"};
    if (p < 16) return fp[p];
    if (p >= 32 && p < 42) return ip[p - 32];
    return "?";
}

bool is_fp(const DxilModule& m, u32 type) {
    if (type >= m.types.size()) return false;
    const auto k = m.types[type].kind;
    return k == DxilTypeKind::Half || k == DxilTypeKind::Float || k == DxilTypeKind::Double;
}

std::string describe(const DxilModule& m, const DxilFunction& fn, const DxilInstruction& inst) {
    switch (inst.op) {
    case DxilOpcode::Binop: return binop_name(inst.sub, is_fp(m, inst.type));
    case DxilOpcode::Cast: return cast_name(inst.sub);
    case DxilOpcode::Gep: return "getelementptr";
    case DxilOpcode::Select: return "select";
    case DxilOpcode::ExtractElement: return "extractelement";
    case DxilOpcode::InsertElement: return "insertelement";
    case DxilOpcode::ShuffleVector: return "shufflevector";
    case DxilOpcode::Cmp:
        return std::string(inst.sub < 16 ? "fcmp " : "icmp ") + predicate_name(inst.sub);
    case DxilOpcode::Ret: return "ret";
    case DxilOpcode::Br: return "br";
    case DxilOpcode::Switch: return "switch";
    case DxilOpcode::Unreachable: return "unreachable";
    case DxilOpcode::Phi: return "phi";
    case DxilOpcode::Alloca: return "alloca";
    case DxilOpcode::Load: return "load";
    case DxilOpcode::Store: return "store";
    case DxilOpcode::ExtractValue: return "extractvalue";
    case DxilOpcode::InsertValue: return "insertvalue";
    case DxilOpcode::AtomicRmw: return "atomicrmw";
    case DxilOpcode::CmpXchg: return "cmpxchg";
    case DxilOpcode::Fence: return "fence";
    case DxilOpcode::Call: {
        const DxilFunction* callee = m.function_of(inst.operands[0]);
        if (callee == nullptr) return "call ?";
        if (callee->name.rfind("dx.op.", 0) == 0) {
            u64 opcode = 0;
            if (inst.operands.size() < 2 || !m.constant_int(&fn, inst.operands[1], opcode)) {
                return "call ?";
            }
            return "call " + std::to_string(opcode);
        }
        return "call @" + callee->name;
    }
    }
    return "?";
}

class DxilReader : public ::testing::TestWithParam<Sm6Blob> {};

TEST_P(DxilReader, MatchesDxcListing) {
    const Sm6Blob& blob = GetParam();
    const DxilContainer container = parse_dxil_container(blob.code, blob.size);
    ASSERT_TRUE(container.ok) << container.error;
    ASSERT_NE(container.bitcode, nullptr);
    const DxilModule module = read_dxil_module(container.bitcode, container.bitcode_size);
    ASSERT_TRUE(module.ok) << module.error;
    EXPECT_EQ(module.triple, "dxil-ms-dx");

    const Listing listing = parse_listing(read_listing(blob.name));
    ASSERT_FALSE(listing.functions.empty());
    for (const auto& name : listing.named_metadata) {
        EXPECT_NE(module.named(name.c_str()), nullptr) << name;
    }
    for (const auto& [name, blocks] : listing.functions) {
        const DxilFunction* fn = nullptr;
        for (const auto& f : module.functions) {
            if (f.name == name) fn = &f;
        }
        ASSERT_NE(fn, nullptr) << name;
        ASSERT_FALSE(fn->is_declaration);
        ASSERT_EQ(fn->blocks.size(), blocks.size()) << name;
        for (usize b = 0; b < fn->blocks.size(); ++b) {
            Sequence got;
            for (u32 index : fn->blocks[b].instructions) {
                got.push_back(describe(module, *fn, fn->instructions[index]));
            }
            EXPECT_EQ(got, blocks[b]) << blob.name << " " << name << " block " << b;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Corpus, DxilReader, ::testing::ValuesIn(kSm6Blobs),
                         [](const ::testing::TestParamInfo<Sm6Blob>& info) {
                             return std::string(info.param.name);
                         });

TEST(DxilContainer, ProgramHeaderNamesStageAndModel) {
    const DxilContainer cs = parse_dxil_container(sm6_cs_alu, sizeof(sm6_cs_alu));
    ASSERT_TRUE(cs.ok);
    EXPECT_EQ(cs.program_kind(), 5u);  // compute
    EXPECT_EQ(cs.major(), 6u);
    EXPECT_EQ(cs.minor(), 0u);
    const DxilContainer ps = parse_dxil_container(sm6_ps_tex, sizeof(sm6_ps_tex));
    ASSERT_TRUE(ps.ok);
    EXPECT_EQ(ps.program_kind(), 0u);  // pixel
    const DxilContainer dyn = parse_dxil_container(sm6_cs_dyn, sizeof(sm6_cs_dyn));
    ASSERT_TRUE(dyn.ok);
    EXPECT_EQ(dyn.minor(), 6u);
    EXPECT_TRUE(dxil_container_present(sm6_cs_alu, sizeof(sm6_cs_alu)));
}

TEST(DxilContainer, RejectsTruncatedAndCorruptInput) {
    EXPECT_FALSE(parse_dxil_container(sm6_cs_alu, 16).ok);
    std::vector<u8> bad(sm6_cs_alu, sm6_cs_alu + sizeof(sm6_cs_alu));
    bad[24] = 0xff;  // ContainerSizeInBytes past the data
    bad[25] = 0xff;
    EXPECT_FALSE(parse_dxil_container(bad.data(), bad.size()).ok);
    const DxilContainer ok = parse_dxil_container(sm6_cs_alu, sizeof(sm6_cs_alu));
    ASSERT_TRUE(ok.ok);
    // Bitcode cut short: the reader fails instead of reading past it.
    EXPECT_FALSE(read_dxil_module(ok.bitcode, ok.bitcode_size / 2).ok);
}

}  // namespace
}  // namespace vitreo::graphics
