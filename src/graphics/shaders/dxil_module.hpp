// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxil_module.hpp
/// @brief The LLVM 3.7 IR module of a DXIL program, read from its bitcode.
///
/// Record layouts are LLVM 3.7's (include/llvm/Bitcode/LLVMBitCodes.h for
/// the codes; lib/Bitcode/Reader/BitcodeReader.cpp of release 3.7 for the
/// operand order of each record). Values are numbered as the bitcode
/// numbers them: module values (globals and functions in record order, then
/// the module constants) first, and inside a function body its arguments,
/// its constants and its instruction results after those. Operand IDs in a
/// function body are relative to the next value number
/// (MODULE_CODE_VERSION 1); the reader stores absolute IDs.

#include "dxil_bitcode.hpp"
#include "shader_types.hpp"

#include <string>
#include <vector>

namespace vitreo::graphics {

enum class DxilTypeKind : u8 {
    Void, Half, Float, Double, Label, Metadata, Opaque,
    Integer, Pointer, Array, Vector, Struct, Function,
};

struct DxilType {
    DxilTypeKind kind{DxilTypeKind::Void};
    u32 width{0};        ///< Integer bit width
    u64 count{0};        ///< Array / vector length
    u32 element{~0u};    ///< Pointer pointee, array / vector element
    u32 address_space{0};
    /// Struct members; for a function the return type, then the parameters.
    std::vector<u32> members;
    bool packed{false};
    bool vararg{false};
    std::string name;    ///< Named struct
};

enum class DxilValueKind : u8 {
    Global, Function, Constant, Argument, Instruction,
};

/// CST_CODE_* constant forms the module keeps.
enum class DxilConstantKind : u8 {
    Null, Undef, Integer, Float, Aggregate, Data, String,
    CastExpr, BinopExpr, GepExpr, CmpExpr, SelectExpr, Other,
};

struct DxilConstant {
    DxilConstantKind kind{DxilConstantKind::Undef};
    /// Integer: the sign-extended value (wide integers: the low word);
    /// Float: the raw bits (half, float or double); Cast/Binop: the opcode.
    u64 bits{0};
    /// Aggregate / expression operands (value IDs); Data / String elements.
    std::vector<u64> elements;
    u32 extra{0};  ///< Cast: the operand type; Gep: inbounds; Cmp: predicate
};

struct DxilValue {
    DxilValueKind kind{DxilValueKind::Constant};
    u32 type{~0u};
    /// Global: index into globals; Function: into functions; Constant: into
    /// constants (module or function); Argument: argument number;
    /// Instruction: index into the function's instructions.
    u32 index{0};
};

struct DxilGlobal {
    std::string name;
    u32 value_type{~0u};   ///< The variable's type (not the pointer)
    u32 address_space{0};
    bool is_constant{false};
    u32 initializer{~0u};  ///< Value ID, or ~0u
    u32 linkage{0};
    u32 alignment{0};
};

/// LLVM 3.7 instruction forms (FUNC_CODE_INST_*).
enum class DxilOpcode : u8 {
    Binop, Cast, Gep, Select, ExtractElement, InsertElement, ShuffleVector,
    Cmp, Ret, Br, Switch, Unreachable, Phi, Alloca, Load, Store,
    ExtractValue, InsertValue, Call, AtomicRmw, CmpXchg, Fence,
};

/// BINOP codes (bitc::BinaryOpcodes); a floating-point operand makes
/// Add/Sub/Mul/SDiv/SRem the F* forms.
enum class DxilBinop : u8 {
    Add = 0, Sub = 1, Mul = 2, UDiv = 3, SDiv = 4, URem = 5, SRem = 6,
    Shl = 7, LShr = 8, AShr = 9, And = 10, Or = 11, Xor = 12,
};

/// CAST codes (bitc::CastOpcodes).
enum class DxilCast : u8 {
    Trunc = 0, ZExt = 1, SExt = 2, FPToUI = 3, FPToSI = 4, UIToFP = 5,
    SIToFP = 6, FPTrunc = 7, FPExt = 8, PtrToInt = 9, IntToPtr = 10,
    BitCast = 11, AddrSpaceCast = 12,
};

/// CmpInst::Predicate (LLVM 3.7 include/llvm/IR/InstrTypes.h).
enum class DxilPredicate : u8 {
    FFalse = 0, FOeq = 1, FOgt = 2, FOge = 3, FOlt = 4, FOle = 5, FOne = 6,
    FOrd = 7, FUno = 8, FUeq = 9, FUgt = 10, FUge = 11, FUlt = 12, FUle = 13,
    FUne = 14, FTrue = 15,
    IEq = 32, INe = 33, IUgt = 34, IUge = 35, IUlt = 36, IUle = 37,
    ISgt = 38, ISge = 39, ISlt = 40, ISle = 41,
};

/// AtomicRMW operations (bitc::RMWOperations).
enum class DxilRmw : u8 {
    Xchg = 0, Add = 1, Sub = 2, And = 3, Nand = 4, Or = 5, Xor = 6,
    Max = 7, Min = 8, UMax = 9, UMin = 10,
};

struct DxilInstruction {
    DxilOpcode op{DxilOpcode::Unreachable};
    u32 type{~0u};       ///< Result type (the void type for none)
    u32 result{~0u};     ///< Value ID of the result, ~0u for none
    /// Binop: DxilBinop; Cast: DxilCast; Cmp: DxilPredicate; AtomicRmw:
    /// DxilRmw.
    u32 sub{0};
    /// Binop: nuw/nsw/exact or fast-math bits; Cmp: fast-math bits;
    /// Gep: inbounds; Load/Store: volatile.
    u32 flags{0};
    /// Value IDs: Binop/Cmp {lhs, rhs}; Cast {x}; Gep {base, idx...};
    /// Select {cond, true, false}; Load {ptr}; Store {ptr, value};
    /// Call {callee, args...}; Ret {value?}; Br {cond?}; Switch {cond};
    /// Phi {incoming...}; Alloca {size}; ExtractValue {agg};
    /// InsertValue {agg, value}; AtomicRmw {ptr, value};
    /// CmpXchg {ptr, cmp, new}; Extract/InsertElement {vec, (elt,) idx}.
    std::vector<u32> operands;
    /// ExtractValue / InsertValue literal indices.
    std::vector<u32> indices;
    /// Br {true, false} or {target}; Switch {default, case targets...};
    /// Phi {incoming block per operand}.
    std::vector<u32> blocks;
    /// Switch case values, parallel to blocks[1..].
    std::vector<u64> cases;
    u32 alignment{0};
    u32 alloca_type{~0u};  ///< Alloca: the allocated type
    u32 ordering{0};       ///< Atomics: AtomicOrderingCodes
};

struct DxilBasicBlock {
    std::vector<u32> instructions;  ///< Indices into DxilFunction::instructions
    std::string name;
};

struct DxilFunction {
    std::string name;
    u32 type{~0u};        ///< The function type
    u32 value_id{~0u};
    bool is_declaration{true};
    u32 linkage{0};
    u32 attributes{0};    ///< PARAMATTR list index (1-based), 0 for none
    std::vector<DxilBasicBlock> blocks;
    std::vector<DxilInstruction> instructions;
    /// Function-local values: arguments, constants and results; local
    /// value k is value ID first_local + k.
    u32 first_local{0};
    std::vector<DxilValue> locals;
    std::vector<DxilConstant> constants;
    /// The fp32-denorm-mode function attribute ("any", "preserve", "ftz"),
    /// empty when absent (DXIL.rst, Functions).
    std::string fp32_denorm_mode;
    /// The waveops-include-helper-lanes function attribute (DXIL.rst,
    /// Functions): wave operations count helper lanes as active.
    bool waveops_include_helper_lanes{false};
};

struct DxilMetadata {
    enum class Kind : u8 { String, Value, Node, Other } kind{Kind::Other};
    std::string string;
    u32 type{~0u};
    u32 value{~0u};  ///< Value: module value ID
    /// Node: metadata indices, or -1 for a null operand.
    std::vector<i64> operands;
};

struct DxilNamedMetadata {
    std::string name;
    std::vector<u32> nodes;
};

struct DxilModule {
    bool ok{false};
    std::string error;
    std::string triple;
    std::string datalayout;
    std::vector<DxilType> types;
    std::vector<DxilValue> values;  ///< Module-level values
    std::vector<DxilGlobal> globals;
    std::vector<DxilFunction> functions;
    std::vector<DxilConstant> constants;
    std::vector<DxilMetadata> metadata;
    std::vector<DxilNamedMetadata> named_metadata;
    /// Attribute groups: string attribute key/value pairs per PARAMATTR
    /// list (index 0 unused).
    std::vector<std::vector<std::pair<std::string, std::string>>> attribute_lists;

    [[nodiscard]] const DxilNamedMetadata* named(const char* name) const;
    /// The value with module-or-local ID @p id as seen from @p fn.
    [[nodiscard]] const DxilValue* value(const DxilFunction* fn, u32 id) const;
    /// The constant behind @p id, or null when it is not one.
    [[nodiscard]] const DxilConstant* constant(const DxilFunction* fn,
                                               u32 id) const;
    /// The integer constant behind @p id.
    [[nodiscard]] bool constant_int(const DxilFunction* fn, u32 id,
                                    u64& out) const;
    [[nodiscard]] u32 type_of(const DxilFunction* fn, u32 id) const;
    /// The function a callee value ID names, or null.
    [[nodiscard]] const DxilFunction* function_of(u32 id) const;
    /// Metadata helpers: the string, the integer constant value of a
    /// Value node, and the node operand (or null).
    [[nodiscard]] const DxilMetadata* md(i64 index) const;
    [[nodiscard]] bool md_int(i64 index, u64& out) const;
    [[nodiscard]] std::string md_string(i64 index) const;
};

/// Parses an LLVM 3.7 module from @p bitcode. Fails (ok == false, error
/// set) on any record this reader cannot place: an unknown instruction or
/// type code, a value ID out of range, or a malformed record.
[[nodiscard]] DxilModule read_dxil_module(const u8* bitcode, usize size);

/// The textual name of a type, as LLVM prints it ("i32", "float",
/// "%dx.types.Handle", "[4 x float]", "i8 addrspace(3)*" ...), for
/// diagnostics and tests.
[[nodiscard]] std::string dxil_type_name(const DxilModule& module, u32 type);

}  // namespace vitreo::graphics
