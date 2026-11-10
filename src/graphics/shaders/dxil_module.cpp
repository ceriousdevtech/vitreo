// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxil_module.cpp
/// @brief LLVM 3.7 module reader for DXIL (record layouts: LLVM release 3.7
/// LLVMBitCodes.h and BitcodeReader.cpp).

#include "dxil_module.hpp"

#include <algorithm>
#include <map>

namespace vitreo::graphics {

namespace {

// Block IDs (bitc::BlockIDs; FIRST_APPLICATION_BLOCKID is 8).
constexpr u32 kModuleBlock = 8;
constexpr u32 kParamattrBlock = 9;
constexpr u32 kParamattrGroupBlock = 10;
constexpr u32 kConstantsBlock = 11;
constexpr u32 kFunctionBlock = 12;
constexpr u32 kValueSymtabBlock = 14;
constexpr u32 kMetadataBlock = 15;
constexpr u32 kTypeBlockNew = 17;

i64 decode_signed(u64 v) {
    if ((v & 1) == 0) return static_cast<i64>(v >> 1);
    if (v != 1) return -static_cast<i64>(v >> 1);
    return INT64_MIN;
}

std::string chars(const std::vector<u64>& ops, usize from = 0) {
    std::string s;
    for (usize i = from; i < ops.size(); ++i) s.push_back(static_cast<char>(ops[i]));
    return s;
}

class ModuleReader {
public:
    explicit ModuleReader(DxilModule& m) : m_(m) {}

    bool fail(const std::string& why) {
        if (m_.error.empty()) m_.error = why;
        return false;
    }

    bool read_module(const BitcodeBlock& block);

private:
    DxilModule& m_;
    std::vector<const BitcodeBlock*> function_blocks_;
    std::vector<u32> functions_with_bodies_;
    std::vector<std::vector<std::pair<std::string, std::string>>> groups_;
    std::map<u32, usize> group_index_;
    u32 i1_type_{~0u};

    // ---- types ----
    u32 find_or_add_pointer(u32 element, u32 space) {
        for (u32 i = 0; i < m_.types.size(); ++i) {
            const auto& t = m_.types[i];
            if (t.kind == DxilTypeKind::Pointer && t.element == element &&
                t.address_space == space) {
                return i;
            }
        }
        DxilType t;
        t.kind = DxilTypeKind::Pointer;
        t.element = element;
        t.address_space = space;
        m_.types.push_back(t);
        return static_cast<u32>(m_.types.size() - 1);
    }
    u32 find_or_add_struct(const std::vector<u32>& members) {
        for (u32 i = 0; i < m_.types.size(); ++i) {
            const auto& t = m_.types[i];
            if (t.kind == DxilTypeKind::Struct && t.name.empty() && t.members == members) {
                return i;
            }
        }
        DxilType t;
        t.kind = DxilTypeKind::Struct;
        t.members = members;
        m_.types.push_back(t);
        return static_cast<u32>(m_.types.size() - 1);
    }
    u32 find_i1() {
        if (i1_type_ != ~0u) return i1_type_;
        for (u32 i = 0; i < m_.types.size(); ++i) {
            if (m_.types[i].kind == DxilTypeKind::Integer && m_.types[i].width == 1) {
                return i1_type_ = i;
            }
        }
        DxilType t;
        t.kind = DxilTypeKind::Integer;
        t.width = 1;
        m_.types.push_back(t);
        return i1_type_ = static_cast<u32>(m_.types.size() - 1);
    }
    bool valid_type(u64 id) const { return id < m_.types.size(); }

    bool read_types(const BitcodeBlock& block) {
        std::string pending_name;
        for (const auto& r : block.records) {
            DxilType t;
            switch (r.code) {
            case 1: continue;  // NUMENTRY
            case 2: t.kind = DxilTypeKind::Void; break;
            case 3: t.kind = DxilTypeKind::Float; break;
            case 4: t.kind = DxilTypeKind::Double; break;
            case 5: t.kind = DxilTypeKind::Label; break;
            case 6:
                t.kind = DxilTypeKind::Opaque;
                t.name = pending_name;
                pending_name.clear();
                break;
            case 7:
                if (r.ops.empty()) return fail("INTEGER type without a width");
                t.kind = DxilTypeKind::Integer;
                t.width = static_cast<u32>(r.ops[0]);
                break;
            case 8:
                if (r.ops.empty()) return fail("POINTER type without a pointee");
                t.kind = DxilTypeKind::Pointer;
                t.element = static_cast<u32>(r.ops[0]);
                t.address_space = r.ops.size() > 1 ? static_cast<u32>(r.ops[1]) : 0;
                break;
            case 10: t.kind = DxilTypeKind::Half; break;
            case 11:
            case 12:
                if (r.ops.size() < 2) return fail("ARRAY/VECTOR type too short");
                t.kind = r.code == 11 ? DxilTypeKind::Array : DxilTypeKind::Vector;
                t.count = r.ops[0];
                t.element = static_cast<u32>(r.ops[1]);
                break;
            case 16: t.kind = DxilTypeKind::Metadata; break;
            case 18:
            case 20:
                if (r.ops.empty()) return fail("STRUCT type without ispacked");
                t.kind = DxilTypeKind::Struct;
                t.packed = r.ops[0] != 0;
                for (usize i = 1; i < r.ops.size(); ++i) {
                    t.members.push_back(static_cast<u32>(r.ops[i]));
                }
                if (r.code == 20) {
                    t.name = pending_name;
                    pending_name.clear();
                }
                break;
            case 19:  // STRUCT_NAME: names the next STRUCT_NAMED / OPAQUE
                pending_name = chars(r.ops);
                continue;
            case 21:
                if (r.ops.size() < 2) return fail("FUNCTION type too short");
                t.kind = DxilTypeKind::Function;
                t.vararg = r.ops[0] != 0;
                for (usize i = 1; i < r.ops.size(); ++i) {
                    t.members.push_back(static_cast<u32>(r.ops[i]));
                }
                break;
            default:
                return fail("unsupported type code " + std::to_string(r.code));
            }
            m_.types.push_back(std::move(t));
        }
        for (const auto& t : m_.types) {
            if (t.element != ~0u && !valid_type(t.element)) return fail("type references an undefined type");
            for (u32 member : t.members) {
                if (!valid_type(member)) return fail("type references an undefined type");
            }
        }
        return true;
    }

    // ---- attributes ----
    bool read_attribute_groups(const BitcodeBlock& block) {
        for (const auto& r : block.records) {
            if (r.code != 3 || r.ops.size() < 2) continue;  // PARAMATTR_GRP_CODE_ENTRY
            std::vector<std::pair<std::string, std::string>> attrs;
            for (usize i = 2; i < r.ops.size();) {
                const u64 kind = r.ops[i++];
                if (kind == 0) {
                    ++i;
                } else if (kind == 1) {
                    i += 2;
                } else if (kind == 3 || kind == 4) {
                    std::string key, value;
                    while (i < r.ops.size() && r.ops[i] != 0) key.push_back(static_cast<char>(r.ops[i++]));
                    ++i;
                    if (kind == 4) {
                        while (i < r.ops.size() && r.ops[i] != 0) value.push_back(static_cast<char>(r.ops[i++]));
                        ++i;
                    }
                    attrs.emplace_back(std::move(key), std::move(value));
                } else {
                    return fail("unknown attribute encoding");
                }
            }
            group_index_[static_cast<u32>(r.ops[0])] = groups_.size();
            groups_.push_back(std::move(attrs));
        }
        return true;
    }
    bool read_attribute_lists(const BitcodeBlock& block) {
        m_.attribute_lists.assign(1, {});
        for (const auto& r : block.records) {
            if (r.code != 2) continue;  // PARAMATTR_CODE_ENTRY: [grpid...]
            std::vector<std::pair<std::string, std::string>> merged;
            for (u64 g : r.ops) {
                const auto it = group_index_.find(static_cast<u32>(g));
                if (it == group_index_.end()) continue;
                for (const auto& a : groups_[it->second]) merged.push_back(a);
            }
            m_.attribute_lists.push_back(std::move(merged));
        }
        return true;
    }

    // ---- constants ----
    bool read_constants(const BitcodeBlock& block, std::vector<DxilValue>& values,
                        std::vector<DxilConstant>& constants) {
        u32 current = ~0u;
        for (const auto& r : block.records) {
            if (r.code == 1) {  // SETTYPE
                if (r.ops.empty() || !valid_type(r.ops[0])) return fail("SETTYPE names an undefined type");
                current = static_cast<u32>(r.ops[0]);
                continue;
            }
            if (current == ~0u) return fail("constant before SETTYPE");
            DxilConstant c;
            switch (r.code) {
            case 2: c.kind = DxilConstantKind::Null; break;
            case 3: c.kind = DxilConstantKind::Undef; break;
            case 4:
                if (r.ops.empty()) return fail("INTEGER constant without a value");
                c.kind = DxilConstantKind::Integer;
                c.bits = static_cast<u64>(decode_signed(r.ops[0]));
                break;
            case 5:
                if (r.ops.empty()) return fail("WIDE_INTEGER constant without words");
                c.kind = DxilConstantKind::Integer;
                c.bits = static_cast<u64>(decode_signed(r.ops[0]));
                break;
            case 6:
                if (r.ops.empty()) return fail("FLOAT constant without a value");
                c.kind = DxilConstantKind::Float;
                c.bits = r.ops[0];
                break;
            case 7:
                c.kind = DxilConstantKind::Aggregate;
                c.elements = r.ops;
                break;
            case 8:
            case 9:
                c.kind = DxilConstantKind::String;
                c.elements = r.ops;
                if (r.code == 9) c.elements.push_back(0);
                break;
            case 22:
                c.kind = DxilConstantKind::Data;
                c.elements = r.ops;
                break;
            case 10:  // CE_BINOP [opcode, lhs, rhs, flags?]
                if (r.ops.size() < 3) return fail("CE_BINOP too short");
                c.kind = DxilConstantKind::BinopExpr;
                c.bits = r.ops[0];
                c.elements = {r.ops[1], r.ops[2]};
                break;
            case 11:  // CE_CAST [opcode, opty, opval]
                if (r.ops.size() < 3) return fail("CE_CAST too short");
                c.kind = DxilConstantKind::CastExpr;
                c.bits = r.ops[0];
                c.extra = static_cast<u32>(r.ops[1]);
                c.elements = {r.ops[2]};
                break;
            case 12:
            case 20: {  // CE_GEP / CE_INBOUNDS_GEP [pointee?, (ty, val)...]
                c.kind = DxilConstantKind::GepExpr;
                c.extra = r.code == 20 ? 1u : 0u;
                usize i = r.ops.size() % 2;
                for (; i + 1 < r.ops.size(); i += 2) c.elements.push_back(r.ops[i + 1]);
                break;
            }
            case 13:
                c.kind = DxilConstantKind::SelectExpr;
                c.elements = r.ops;
                break;
            case 17:  // CE_CMP [opty, lhs, rhs, pred]
                if (r.ops.size() < 4) return fail("CE_CMP too short");
                c.kind = DxilConstantKind::CmpExpr;
                c.elements = {r.ops[1], r.ops[2]};
                c.extra = static_cast<u32>(r.ops[3]);
                break;
            default:
                c.kind = DxilConstantKind::Other;
                break;
            }
            DxilValue v;
            v.kind = DxilValueKind::Constant;
            v.type = current;
            v.index = static_cast<u32>(constants.size());
            constants.push_back(std::move(c));
            values.push_back(v);
        }
        return true;
    }

    // ---- metadata ----
    bool read_metadata(const BitcodeBlock& block) {
        for (usize e = 0; e < block.records.size(); ++e) {
            const auto& r = block.records[e];
            DxilMetadata md;
            switch (r.code) {
            case 1:  // STRING
                md.kind = DxilMetadata::Kind::String;
                md.string = r.has_blob ? std::string(r.blob.begin(), r.blob.end()) : chars(r.ops);
                break;
            case 2:  // VALUE [ty, val]
                if (r.ops.size() != 2) return fail("METADATA_VALUE needs two operands");
                md.kind = DxilMetadata::Kind::Value;
                md.type = static_cast<u32>(r.ops[0]);
                md.value = static_cast<u32>(r.ops[1]);
                break;
            case 3:
            case 5:  // NODE / DISTINCT_NODE [n x (md+1)]
                md.kind = DxilMetadata::Kind::Node;
                for (u64 op : r.ops) md.operands.push_back(op == 0 ? -1 : static_cast<i64>(op - 1));
                break;
            case 4: {  // NAME, then NAMED_NODE
                DxilNamedMetadata named;
                named.name = chars(r.ops);
                if (e + 1 >= block.records.size() || block.records[e + 1].code != 10) {
                    return fail("METADATA_NAME not followed by NAMED_NODE");
                }
                ++e;
                for (u64 op : block.records[e].ops) named.nodes.push_back(static_cast<u32>(op));
                m_.named_metadata.push_back(std::move(named));
                continue;
            }
            case 6:   // KIND
            case 10:  // NAMED_NODE without a NAME
            case 11:  // ATTACHMENT
                continue;
            case 8: {  // OLD_NODE [n x (ty, val)]
                md.kind = DxilMetadata::Kind::Other;
                break;
            }
            default:
                // LOCATION and the debug-info records each take a slot.
                md.kind = DxilMetadata::Kind::Other;
                break;
            }
            m_.metadata.push_back(std::move(md));
        }
        return true;
    }

    // ---- symbol tables ----
    void read_module_symtab(const BitcodeBlock& block) {
        for (const auto& r : block.records) {
            if (r.code != 1 || r.ops.empty()) continue;  // VST_ENTRY
            const u64 id = r.ops[0];
            if (id >= m_.values.size()) continue;
            const DxilValue& v = m_.values[id];
            const std::string name = chars(r.ops, 1);
            if (v.kind == DxilValueKind::Global) m_.globals[v.index].name = name;
            if (v.kind == DxilValueKind::Function) m_.functions[v.index].name = name;
        }
    }

    // ---- function bodies ----
    bool read_function(DxilFunction& fn, const BitcodeBlock& block);
};

bool ModuleReader::read_module(const BitcodeBlock& block) {
    // Types, attributes and the module-level value list come first in
    // stream order; function bodies follow once every module value exists.
    const BitcodeBlock* symtab = nullptr;
    for (const auto& entry : block.entries) {
        if (entry.is_block) {
            const BitcodeBlock& b = block.blocks[entry.index];
            switch (b.id) {
            case kTypeBlockNew:
                if (!read_types(b)) return false;
                break;
            case kParamattrGroupBlock:
                if (!read_attribute_groups(b)) return false;
                break;
            case kParamattrBlock:
                if (!read_attribute_lists(b)) return false;
                break;
            case kConstantsBlock:
                if (!read_constants(b, m_.values, m_.constants)) return false;
                break;
            case kMetadataBlock:
                if (!read_metadata(b)) return false;
                break;
            case kValueSymtabBlock:
                symtab = &b;
                break;
            case kFunctionBlock:
                function_blocks_.push_back(&b);
                break;
            default:
                break;  // USELIST, METADATA_KIND, etc.
            }
            continue;
        }
        const BitcodeRecord& r = block.records[entry.index];
        switch (r.code) {
        case 1:  // VERSION
            if (r.ops.empty() || r.ops[0] != 1) {
                return fail("module version is not 1 (relative value IDs)");
            }
            break;
        case 2: m_.triple = chars(r.ops); break;
        case 3: m_.datalayout = chars(r.ops); break;
        case 7: {  // GLOBALVAR [ty, isconst|explicit<<1|as<<2, initid, linkage, align, section, ...]
            if (r.ops.size() < 6 || !valid_type(r.ops[0])) return fail("malformed GLOBALVAR");
            DxilGlobal g;
            u32 type = static_cast<u32>(r.ops[0]);
            g.is_constant = (r.ops[1] & 1) != 0;
            if ((r.ops[1] & 2) != 0) {
                g.address_space = static_cast<u32>(r.ops[1] >> 2);
            } else {
                if (m_.types[type].kind != DxilTypeKind::Pointer) return fail("GLOBALVAR type is not a pointer");
                g.address_space = m_.types[type].address_space;
                type = m_.types[type].element;
            }
            g.value_type = type;
            g.initializer = r.ops[2] == 0 ? ~0u : static_cast<u32>(r.ops[2] - 1);
            g.linkage = static_cast<u32>(r.ops[3]);
            g.alignment = r.ops[4] == 0 ? 0u : (1u << (r.ops[4] - 1));
            DxilValue v;
            v.kind = DxilValueKind::Global;
            v.type = find_or_add_pointer(type, g.address_space);
            v.index = static_cast<u32>(m_.globals.size());
            m_.globals.push_back(std::move(g));
            m_.values.push_back(v);
            break;
        }
        case 8: {  // FUNCTION [type, cc, isproto, linkage, paramattr, align, section, visibility, ...]
            if (r.ops.size() < 8 || !valid_type(r.ops[0])) return fail("malformed FUNCTION");
            u32 type = static_cast<u32>(r.ops[0]);
            if (m_.types[type].kind == DxilTypeKind::Pointer) type = m_.types[type].element;
            if (m_.types[type].kind != DxilTypeKind::Function) return fail("FUNCTION type is not a function type");
            DxilFunction f;
            f.type = type;
            f.is_declaration = r.ops[2] != 0;
            f.linkage = static_cast<u32>(r.ops[3]);
            f.attributes = static_cast<u32>(r.ops[4]);
            f.value_id = static_cast<u32>(m_.values.size());
            DxilValue v;
            v.kind = DxilValueKind::Function;
            v.type = find_or_add_pointer(type, 0);
            v.index = static_cast<u32>(m_.functions.size());
            if (!f.is_declaration) functions_with_bodies_.push_back(v.index);
            m_.functions.push_back(std::move(f));
            m_.values.push_back(v);
            break;
        }
        default:
            break;
        }
    }
    if (symtab != nullptr) read_module_symtab(*symtab);
    for (auto& f : m_.functions) {
        if (f.attributes == 0 || f.attributes >= m_.attribute_lists.size()) continue;
        for (const auto& [key, value] : m_.attribute_lists[f.attributes]) {
            if (key == "fp32-denorm-mode") f.fp32_denorm_mode = value;
            if (key == "waveops-include-helper-lanes") f.waveops_include_helper_lanes = true;
        }
    }
    if (function_blocks_.size() != functions_with_bodies_.size()) {
        return fail("function body count does not match the function definitions");
    }
    for (usize i = 0; i < function_blocks_.size(); ++i) {
        if (!read_function(m_.functions[functions_with_bodies_[i]], *function_blocks_[i])) {
            return false;
        }
    }
    return true;
}

bool ModuleReader::read_function(DxilFunction& fn, const BitcodeBlock& block) {
    const u32 module_values = static_cast<u32>(m_.values.size());
    fn.first_local = module_values;
    const DxilType& fty = m_.types[fn.type];
    for (usize p = 1; p < fty.members.size(); ++p) {
        DxilValue v;
        v.kind = DxilValueKind::Argument;
        v.type = fty.members[p];
        v.index = static_cast<u32>(p - 1);
        fn.locals.push_back(v);
    }
    const auto next_value = [&]() { return module_values + static_cast<u32>(fn.locals.size()); };
    const auto type_of = [&](u32 id) -> u32 {
        if (id < module_values) return m_.values[id].type;
        const u32 local = id - module_values;
        return local < fn.locals.size() ? fn.locals[local].type : ~0u;
    };
    // Forward references: their types are recorded at the reference.
    std::map<u32, u32> forward_types;
    u32 current_block = 0;
    bool declared = false;

    // [relative value, (type if forward)] pairs.
    const auto value_type_pair = [&](const BitcodeRecord& r, usize& slot, u32& out) -> bool {
        if (slot >= r.ops.size()) return false;
        const u32 next = next_value();
        out = static_cast<u32>(next - static_cast<u32>(r.ops[slot++]));
        if (out >= next) {
            if (slot >= r.ops.size()) return false;
            forward_types[out] = static_cast<u32>(r.ops[slot++]);
        }
        return true;
    };
    const auto relative = [&](const BitcodeRecord& r, usize slot, u32& out) -> bool {
        if (slot >= r.ops.size()) return false;
        out = static_cast<u32>(next_value() - static_cast<u32>(r.ops[slot]));
        return true;
    };

    for (const auto& entry : block.entries) {
        if (entry.is_block) {
            const BitcodeBlock& b = block.blocks[entry.index];
            if (b.id == kConstantsBlock) {
                const usize before = fn.locals.size();
                if (!read_constants(b, fn.locals, fn.constants)) return false;
                (void)before;
            } else if (b.id == kValueSymtabBlock) {
                for (const auto& r : b.records) {
                    if (r.ops.empty()) continue;
                    if (r.code == 2 && r.ops[0] < fn.blocks.size()) {  // BBENTRY
                        fn.blocks[r.ops[0]].name = chars(r.ops, 1);
                    }
                }
            }
            // Function-local metadata, attachments and use lists carry
            // nothing the translation reads.
            continue;
        }
        const BitcodeRecord& r = block.records[entry.index];
        if (r.code == 1) {  // DECLAREBLOCKS
            if (r.ops.empty() || r.ops[0] == 0) return fail("DECLAREBLOCKS without blocks");
            fn.blocks.resize(r.ops[0]);
            declared = true;
            continue;
        }
        if (r.code == 33 || r.code == 35) continue;  // DEBUG_LOC(_AGAIN)
        if (!declared) return fail("instruction before DECLAREBLOCKS");
        if (current_block >= fn.blocks.size()) return fail("instruction after the last block");
        DxilInstruction inst;
        u32 result_type = ~0u;
        bool terminator = false;
        usize slot = 0;
        const auto bad = [&](const char* what) {
            return fail(std::string("malformed ") + what + " record in " + fn.name);
        };
        switch (r.code) {
        case 2: {  // BINOP [opval, ty?, opval, opcode, flags?]
            inst.op = DxilOpcode::Binop;
            u32 lhs = 0, rhs = 0;
            if (!value_type_pair(r, slot, lhs) || !relative(r, slot, rhs)) return bad("BINOP");
            ++slot;
            if (slot >= r.ops.size()) return bad("BINOP");
            inst.sub = static_cast<u32>(r.ops[slot++]);
            if (slot < r.ops.size()) inst.flags = static_cast<u32>(r.ops[slot]);
            inst.operands = {lhs, rhs};
            result_type = type_of(lhs) != ~0u ? type_of(lhs) : forward_types[lhs];
            break;
        }
        case 3: {  // CAST [opval, ty?, destty, castopc]
            inst.op = DxilOpcode::Cast;
            u32 x = 0;
            if (!value_type_pair(r, slot, x) || slot + 2 != r.ops.size()) return bad("CAST");
            if (!valid_type(r.ops[slot])) return bad("CAST");
            result_type = static_cast<u32>(r.ops[slot]);
            inst.sub = static_cast<u32>(r.ops[slot + 1]);
            inst.operands = {x};
            break;
        }
        case 43: {  // GEP [inbounds, ty, (opval, ty?)...]
            inst.op = DxilOpcode::Gep;
            if (r.ops.size() < 3) return bad("GEP");
            inst.flags = static_cast<u32>(r.ops[0]);
            u32 source = static_cast<u32>(r.ops[1]);
            slot = 2;
            while (slot < r.ops.size()) {
                u32 v = 0;
                if (!value_type_pair(r, slot, v)) return bad("GEP");
                inst.operands.push_back(v);
            }
            if (!valid_type(source) || inst.operands.empty()) return bad("GEP");
            // Result: a pointer, in the base's address space, to the type
            // the indices after the first select.
            u32 t = source;
            for (usize k = 2; k < inst.operands.size(); ++k) {
                const DxilType& cur = m_.types[t];
                if (cur.kind == DxilTypeKind::Struct) {
                    u64 field = 0;
                    if (!m_.constant_int(&fn, inst.operands[k], field) || field >= cur.members.size()) {
                        return bad("GEP struct index");
                    }
                    t = cur.members[field];
                } else if (cur.kind == DxilTypeKind::Array || cur.kind == DxilTypeKind::Vector) {
                    t = cur.element;
                } else {
                    return bad("GEP index into a scalar");
                }
            }
            const u32 base_type = type_of(inst.operands[0]);
            const u32 space = base_type != ~0u ? m_.types[base_type].address_space : 0;
            result_type = find_or_add_pointer(t, space);
            inst.alloca_type = source;
            break;
        }
        case 5: {  // SELECT [opval, ty?, opval, cond]
            inst.op = DxilOpcode::Select;
            u32 t = 0, f = 0, c = 0;
            if (!value_type_pair(r, slot, t) || !relative(r, slot, f)) return bad("SELECT");
            ++slot;
            if (!relative(r, slot, c)) return bad("SELECT");
            inst.operands = {c, t, f};
            result_type = type_of(t);
            break;
        }
        case 29: {  // VSELECT [opval, ty?, opval, pred, predty?]
            inst.op = DxilOpcode::Select;
            u32 t = 0, f = 0, c = 0;
            if (!value_type_pair(r, slot, t) || !relative(r, slot, f)) return bad("VSELECT");
            ++slot;
            if (!value_type_pair(r, slot, c)) return bad("VSELECT");
            inst.operands = {c, t, f};
            result_type = type_of(t);
            break;
        }
        case 6: {  // EXTRACTELT [opval, ty?, opval, ty?]
            inst.op = DxilOpcode::ExtractElement;
            u32 v = 0, i = 0;
            if (!value_type_pair(r, slot, v) || !value_type_pair(r, slot, i)) return bad("EXTRACTELT");
            inst.operands = {v, i};
            const u32 vt = type_of(v);
            if (vt == ~0u) return bad("EXTRACTELT");
            result_type = m_.types[vt].element;
            break;
        }
        case 7: {  // INSERTELT [opval, ty?, elt, idx, ty?]
            inst.op = DxilOpcode::InsertElement;
            u32 v = 0, e = 0, i = 0;
            if (!value_type_pair(r, slot, v) || !relative(r, slot, e)) return bad("INSERTELT");
            ++slot;
            if (!value_type_pair(r, slot, i)) return bad("INSERTELT");
            inst.operands = {v, e, i};
            result_type = type_of(v);
            break;
        }
        case 8: {  // SHUFFLEVEC [opval, ty?, opval, mask, ty?]
            inst.op = DxilOpcode::ShuffleVector;
            u32 a = 0, b = 0, mask = 0;
            if (!value_type_pair(r, slot, a) || !relative(r, slot, b)) return bad("SHUFFLEVEC");
            ++slot;
            if (!value_type_pair(r, slot, mask)) return bad("SHUFFLEVEC");
            inst.operands = {a, b, mask};
            const u32 mt = type_of(mask);
            const u32 at = type_of(a);
            if (mt == ~0u || at == ~0u) return bad("SHUFFLEVEC");
            DxilType vt;
            vt.kind = DxilTypeKind::Vector;
            vt.count = m_.types[mt].count;
            vt.element = m_.types[at].element;
            result_type = ~0u;
            for (u32 k = 0; k < m_.types.size(); ++k) {
                if (m_.types[k].kind == DxilTypeKind::Vector && m_.types[k].count == vt.count &&
                    m_.types[k].element == vt.element) {
                    result_type = k;
                }
            }
            if (result_type == ~0u) {
                m_.types.push_back(vt);
                result_type = static_cast<u32>(m_.types.size() - 1);
            }
            break;
        }
        case 9:
        case 28: {  // CMP / CMP2 [opval, ty?, opval, pred, fmf?]
            inst.op = DxilOpcode::Cmp;
            u32 a = 0, b = 0;
            if (!value_type_pair(r, slot, a) || !relative(r, slot, b)) return bad("CMP");
            ++slot;
            if (slot >= r.ops.size()) return bad("CMP");
            inst.sub = static_cast<u32>(r.ops[slot++]);
            if (slot < r.ops.size()) inst.flags = static_cast<u32>(r.ops[slot]);
            inst.operands = {a, b};
            const u32 at = type_of(a) != ~0u ? type_of(a) : forward_types[a];
            if (at != ~0u && m_.types[at].kind == DxilTypeKind::Vector) {
                DxilType vt;
                vt.kind = DxilTypeKind::Vector;
                vt.count = m_.types[at].count;
                vt.element = find_i1();
                m_.types.push_back(vt);
                result_type = static_cast<u32>(m_.types.size() - 1);
            } else {
                result_type = find_i1();
            }
            break;
        }
        case 10: {  // RET [opval, ty?]
            inst.op = DxilOpcode::Ret;
            terminator = true;
            if (!r.ops.empty()) {
                u32 v = 0;
                if (!value_type_pair(r, slot, v)) return bad("RET");
                inst.operands = {v};
            }
            break;
        }
        case 11: {  // BR [bb] or [bb, bb, cond]
            inst.op = DxilOpcode::Br;
            terminator = true;
            if (r.ops.size() == 1) {
                inst.blocks = {static_cast<u32>(r.ops[0])};
            } else if (r.ops.size() == 3) {
                u32 c = 0;
                if (!relative(r, 2, c)) return bad("BR");
                inst.blocks = {static_cast<u32>(r.ops[0]), static_cast<u32>(r.ops[1])};
                inst.operands = {c};
            } else {
                return bad("BR");
            }
            for (u32 b : inst.blocks) if (b >= fn.blocks.size()) return bad("BR target");
            break;
        }
        case 12: {  // SWITCH [opty, cond, default, (value id, bb)...]
            inst.op = DxilOpcode::Switch;
            terminator = true;
            if (r.ops.size() < 3 || (r.ops[0] >> 16) == 0x4B5u) {
                return fail("unsupported SWITCH encoding in " + fn.name);
            }
            if ((r.ops.size() & 1) == 0) return bad("SWITCH");
            u32 c = 0;
            if (!relative(r, 1, c)) return bad("SWITCH");
            inst.operands = {c};
            inst.blocks.push_back(static_cast<u32>(r.ops[2]));
            for (usize k = 3; k + 1 < r.ops.size(); k += 2) {
                u64 value = 0;
                // Case values are absolute value IDs of constants.
                if (!m_.constant_int(&fn, static_cast<u32>(r.ops[k]), value)) return bad("SWITCH case");
                inst.cases.push_back(value);
                inst.blocks.push_back(static_cast<u32>(r.ops[k + 1]));
            }
            for (u32 b : inst.blocks) if (b >= fn.blocks.size()) return bad("SWITCH target");
            break;
        }
        case 15:  // UNREACHABLE
            inst.op = DxilOpcode::Unreachable;
            terminator = true;
            break;
        case 16: {  // PHI [ty, (signed rel value, bb)...]
            inst.op = DxilOpcode::Phi;
            if (r.ops.empty() || ((r.ops.size() - 1) & 1) != 0 || !valid_type(r.ops[0])) return bad("PHI");
            result_type = static_cast<u32>(r.ops[0]);
            for (usize k = 1; k + 1 < r.ops.size(); k += 2) {
                const i64 rel = decode_signed(r.ops[k]);
                inst.operands.push_back(static_cast<u32>(static_cast<i64>(next_value()) - rel));
                inst.blocks.push_back(static_cast<u32>(r.ops[k + 1]));
                if (inst.blocks.back() >= fn.blocks.size()) return bad("PHI block");
            }
            break;
        }
        case 19: {  // ALLOCA [instty, opty, op, align]
            inst.op = DxilOpcode::Alloca;
            if (r.ops.size() != 4 || !valid_type(r.ops[0])) return bad("ALLOCA");
            u32 type = static_cast<u32>(r.ops[0]);
            const u64 align = r.ops[3];
            if ((align & (u64{1} << 6)) == 0) {
                if (m_.types[type].kind != DxilTypeKind::Pointer) return bad("ALLOCA");
                type = m_.types[type].element;
            }
            inst.alloca_type = type;
            inst.operands = {static_cast<u32>(r.ops[2])};  // absolute
            const u64 a = align & ~((u64{1} << 5) | (u64{1} << 6));
            inst.alignment = a == 0 ? 0u : (1u << (a - 1));
            result_type = find_or_add_pointer(type, 0);
            break;
        }
        case 20: {  // LOAD [op, ty?, retty?, align, vol]
            inst.op = DxilOpcode::Load;
            u32 p = 0;
            if (!value_type_pair(r, slot, p)) return bad("LOAD");
            if (slot + 3 == r.ops.size()) {
                result_type = static_cast<u32>(r.ops[slot++]);
            } else if (slot + 2 == r.ops.size()) {
                const u32 pt = type_of(p);
                if (pt == ~0u) return bad("LOAD");
                result_type = m_.types[pt].element;
            } else {
                return bad("LOAD");
            }
            inst.alignment = r.ops[slot] == 0 ? 0u : (1u << (r.ops[slot] - 1));
            inst.flags = static_cast<u32>(r.ops[slot + 1]);
            inst.operands = {p};
            break;
        }
        case 44: {  // STORE [ptr, ty?, val, ty?, align, vol]
            inst.op = DxilOpcode::Store;
            u32 p = 0, v = 0;
            if (!value_type_pair(r, slot, p) || !value_type_pair(r, slot, v) || slot + 2 != r.ops.size()) {
                return bad("STORE");
            }
            inst.alignment = r.ops[slot] == 0 ? 0u : (1u << (r.ops[slot] - 1));
            inst.flags = static_cast<u32>(r.ops[slot + 1]);
            inst.operands = {p, v};
            break;
        }
        case 26: {  // EXTRACTVAL [opval, ty?, idx...]
            inst.op = DxilOpcode::ExtractValue;
            u32 agg = 0;
            if (!value_type_pair(r, slot, agg) || slot >= r.ops.size()) return bad("EXTRACTVAL");
            inst.operands = {agg};
            u32 t = type_of(agg);
            for (; slot < r.ops.size(); ++slot) {
                inst.indices.push_back(static_cast<u32>(r.ops[slot]));
                if (t == ~0u) return bad("EXTRACTVAL");
                const DxilType& cur = m_.types[t];
                if (cur.kind == DxilTypeKind::Struct) {
                    if (r.ops[slot] >= cur.members.size()) return bad("EXTRACTVAL");
                    t = cur.members[r.ops[slot]];
                } else if (cur.kind == DxilTypeKind::Array) {
                    t = cur.element;
                } else {
                    return bad("EXTRACTVAL");
                }
            }
            result_type = t;
            break;
        }
        case 27: {  // INSERTVAL [opval, ty?, opval, ty?, idx...]
            inst.op = DxilOpcode::InsertValue;
            u32 agg = 0, v = 0;
            if (!value_type_pair(r, slot, agg) || !value_type_pair(r, slot, v)) return bad("INSERTVAL");
            inst.operands = {agg, v};
            for (; slot < r.ops.size(); ++slot) inst.indices.push_back(static_cast<u32>(r.ops[slot]));
            result_type = type_of(agg);
            break;
        }
        case 34: {  // CALL [paramattrs, cc, fnty?, fnid, args...]
            inst.op = DxilOpcode::Call;
            if (r.ops.size() < 3) return bad("CALL");
            inst.flags = static_cast<u32>(r.ops[0]);
            const u64 cc = r.ops[1];
            slot = 2;
            u32 ftype = ~0u;
            if (((cc >> 15) & 1) != 0) ftype = static_cast<u32>(r.ops[slot++]);
            u32 callee = 0;
            if (!value_type_pair(r, slot, callee)) return bad("CALL");
            if (ftype == ~0u) {
                const u32 ct = type_of(callee);
                if (ct == ~0u) return bad("CALL");
                ftype = m_.types[ct].element;
            }
            if (!valid_type(ftype) || m_.types[ftype].kind != DxilTypeKind::Function) return bad("CALL");
            const DxilType& ft = m_.types[ftype];
            inst.operands.push_back(callee);
            for (usize p = 1; p < ft.members.size(); ++p) {
                u32 arg = 0;
                if (!relative(r, slot++, arg)) return bad("CALL argument");
                inst.operands.push_back(arg);
            }
            if (slot != r.ops.size()) return fail("vararg call in " + fn.name);
            result_type = ft.members[0];
            break;
        }
        case 36:  // FENCE [ordering, synchscope]
            inst.op = DxilOpcode::Fence;
            if (!r.ops.empty()) inst.ordering = static_cast<u32>(r.ops[0]);
            break;
        case 38: {  // ATOMICRMW [ptr, ty?, val, op, vol, ordering, synchscope]
            inst.op = DxilOpcode::AtomicRmw;
            u32 p = 0, v = 0;
            if (!value_type_pair(r, slot, p) || !relative(r, slot, v)) return bad("ATOMICRMW");
            ++slot;
            if (slot + 4 != r.ops.size()) return bad("ATOMICRMW");
            inst.sub = static_cast<u32>(r.ops[slot]);
            inst.flags = static_cast<u32>(r.ops[slot + 1]);
            inst.ordering = static_cast<u32>(r.ops[slot + 2]);
            inst.operands = {p, v};
            const u32 pt = type_of(p);
            if (pt == ~0u) return bad("ATOMICRMW");
            result_type = m_.types[pt].element;
            break;
        }
        case 46: {  // CMPXCHG [ptr, ty?, cmp, ty?, new, vol, success, synchscope, failure?, weak?]
            inst.op = DxilOpcode::CmpXchg;
            u32 p = 0, c = 0, n = 0;
            if (!value_type_pair(r, slot, p) || !value_type_pair(r, slot, c) || !relative(r, slot, n)) {
                return bad("CMPXCHG");
            }
            ++slot;
            if (slot + 3 > r.ops.size()) return bad("CMPXCHG");
            inst.flags = static_cast<u32>(r.ops[slot]);
            inst.ordering = static_cast<u32>(r.ops[slot + 1]);
            inst.operands = {p, c, n};
            const u32 ct = type_of(c);
            if (ct == ~0u) return bad("CMPXCHG");
            result_type = find_or_add_struct({ct, find_i1()});
            break;
        }
        default:
            return fail("unsupported instruction record " + std::to_string(r.code) + " in " + fn.name);
        }
        // Every operand except forward references and absolute IDs must be
        // defined.
        inst.type = result_type;
        const bool has_value =
            result_type != ~0u && result_type < m_.types.size() &&
            m_.types[result_type].kind != DxilTypeKind::Void;
        if (has_value) {
            inst.result = next_value();
            DxilValue v;
            v.kind = DxilValueKind::Instruction;
            v.type = result_type;
            v.index = static_cast<u32>(fn.instructions.size());
            fn.locals.push_back(v);
        }
        fn.blocks[current_block].instructions.push_back(static_cast<u32>(fn.instructions.size()));
        fn.instructions.push_back(std::move(inst));
        if (terminator) ++current_block;
    }
    if (current_block != fn.blocks.size()) return fail("function " + fn.name + " ends inside a block");
    // Every operand names a defined value.
    const u32 total = next_value();
    for (const auto& inst : fn.instructions) {
        for (usize k = 0; k < inst.operands.size(); ++k) {
            if (inst.op == DxilOpcode::Alloca) continue;
            if (inst.operands[k] >= total) return fail("operand names an undefined value in " + fn.name);
        }
    }
    return true;
}

}  // namespace

const DxilNamedMetadata* DxilModule::named(const char* name) const {
    for (const auto& n : named_metadata) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

const DxilValue* DxilModule::value(const DxilFunction* fn, u32 id) const {
    if (id < values.size()) return &values[id];
    if (fn == nullptr || id < fn->first_local) return nullptr;
    const u32 local = id - fn->first_local;
    return local < fn->locals.size() ? &fn->locals[local] : nullptr;
}

const DxilConstant* DxilModule::constant(const DxilFunction* fn, u32 id) const {
    const DxilValue* v = value(fn, id);
    if (v == nullptr || v->kind != DxilValueKind::Constant) return nullptr;
    if (id < values.size()) return v->index < constants.size() ? &constants[v->index] : nullptr;
    return fn != nullptr && v->index < fn->constants.size() ? &fn->constants[v->index] : nullptr;
}

bool DxilModule::constant_int(const DxilFunction* fn, u32 id, u64& out) const {
    const DxilConstant* c = constant(fn, id);
    if (c == nullptr) return false;
    if (c->kind == DxilConstantKind::Integer) {
        out = c->bits;
        return true;
    }
    if (c->kind == DxilConstantKind::Null) {
        out = 0;
        return true;
    }
    return false;
}

u32 DxilModule::type_of(const DxilFunction* fn, u32 id) const {
    const DxilValue* v = value(fn, id);
    return v != nullptr ? v->type : ~0u;
}

const DxilFunction* DxilModule::function_of(u32 id) const {
    if (id >= values.size() || values[id].kind != DxilValueKind::Function) return nullptr;
    return &functions[values[id].index];
}

const DxilMetadata* DxilModule::md(i64 index) const {
    if (index < 0 || static_cast<u64>(index) >= metadata.size()) return nullptr;
    return &metadata[static_cast<usize>(index)];
}

bool DxilModule::md_int(i64 index, u64& out) const {
    const DxilMetadata* m = md(index);
    if (m == nullptr || m->kind != DxilMetadata::Kind::Value) return false;
    return constant_int(nullptr, m->value, out);
}

std::string DxilModule::md_string(i64 index) const {
    const DxilMetadata* m = md(index);
    return m != nullptr && m->kind == DxilMetadata::Kind::String ? m->string : std::string();
}

DxilModule read_dxil_module(const u8* bitcode, usize size) {
    DxilModule module;
    const BitcodeStream stream = read_bitcode(bitcode, size);
    if (!stream.ok) {
        module.error = stream.error;
        return module;
    }
    const BitcodeBlock* top = nullptr;
    for (const auto& b : stream.blocks) {
        if (b.id == kModuleBlock) top = &b;
    }
    if (top == nullptr) {
        module.error = "bitcode has no MODULE_BLOCK";
        return module;
    }
    ModuleReader reader(module);
    module.ok = reader.read_module(*top);
    return module;
}

std::string dxil_type_name(const DxilModule& module, u32 type) {
    if (type >= module.types.size()) return "<bad type>";
    const DxilType& t = module.types[type];
    switch (t.kind) {
    case DxilTypeKind::Void: return "void";
    case DxilTypeKind::Half: return "half";
    case DxilTypeKind::Float: return "float";
    case DxilTypeKind::Double: return "double";
    case DxilTypeKind::Label: return "label";
    case DxilTypeKind::Metadata: return "metadata";
    case DxilTypeKind::Opaque: return "%" + t.name;
    case DxilTypeKind::Integer: return "i" + std::to_string(t.width);
    case DxilTypeKind::Pointer: {
        std::string s = dxil_type_name(module, t.element);
        if (t.address_space != 0) s += " addrspace(" + std::to_string(t.address_space) + ")";
        return s + "*";
    }
    case DxilTypeKind::Array:
        return "[" + std::to_string(t.count) + " x " + dxil_type_name(module, t.element) + "]";
    case DxilTypeKind::Vector:
        return "<" + std::to_string(t.count) + " x " + dxil_type_name(module, t.element) + ">";
    case DxilTypeKind::Struct: {
        if (!t.name.empty()) return "%" + t.name;
        std::string s = "{ ";
        for (usize i = 0; i < t.members.size(); ++i) {
            if (i != 0) s += ", ";
            s += dxil_type_name(module, t.members[i]);
        }
        return s + " }";
    }
    case DxilTypeKind::Function: {
        std::string s = dxil_type_name(module, t.members.empty() ? ~0u : t.members[0]) + " (";
        for (usize i = 1; i < t.members.size(); ++i) {
            if (i != 1) s += ", ";
            s += dxil_type_name(module, t.members[i]);
        }
        return s + ")";
    }
    }
    return "?";
}

}  // namespace vitreo::graphics
