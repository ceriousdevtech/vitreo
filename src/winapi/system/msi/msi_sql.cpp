// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_sql.cpp
/// @brief Windows Installer SQL (msi_sql.hpp): lexer, parser, binder, executor.

#include "winapi/system/msi/msi_sql.hpp"
#include "winapi/system/msi/msi_core.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace vitreo::winapi::msi::sql {

namespace {

// ---- errors (MsiGetLastErrorRecord field 1; msi_database_sql_probe) ---------------
constexpr u32 kErrTableExists = 2204;
constexpr u32 kErrMissingParams = 2208;
constexpr u32 kErrBadValue = 2210;
constexpr u32 kErrUnknownTable = 2228;
constexpr u32 kErrUnloadable = 2229;
constexpr u32 kErrSelfJoin = 2230;
constexpr u32 kErrTypeMismatch = 2232;
constexpr u32 kErrUnknownColumn = 2235;
constexpr u32 kErrBadToken = 2236;
constexpr u32 kErrSyntax = 2237;
constexpr u32 kErrNoFrom = 2238;
constexpr u32 kErrValueCount = 2239;
constexpr u32 kErrDuplicateColumn = 2242;
constexpr u32 kErrParamType = 2258;
constexpr u32 kErrUpdateFailed = 2259;

constexpr u32 kErrorBadQuery = 1615;
constexpr u32 kErrorFunctionFailed = 1627;
constexpr usize kMaxComparisons = 32;  // Learn "SQL Syntax"; measured 32 ok, 33 fails

// ---- lexer -------------------------------------------------------------------------

enum class T : u8 { Name, QName, Str, Int, Sym, Param, End };

struct Tok {
    T t = T::End;
    std::string text;
    i64 v = 0;
    bool unterminated = false;
    bool invalid = false;  ///< a character that starts no token ('!')
};

bool is_delim(char c) {
    switch (c) {
    case ' ': case '(': case ')': case ',': case '=': case '<': case '>':
    case '*': case '?': case '.': case '`': case '\'': case '!':
        return true;
    default:
        return false;
    }
}

std::vector<Tok> lex(std::string_view s) {
    std::vector<Tok> out;
    usize i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ') { ++i; continue; }
        Tok t;
        if (c == '`' || c == '\'') {
            const usize start = ++i;
            while (i < s.size() && s[i] != c) ++i;
            t.t = c == '`' ? T::QName : T::Str;
            t.text = std::string(s.substr(start, i - start));
            if (i < s.size()) ++i;
            else t.unterminated = true;
        } else if (c == '?') {
            t.t = T::Param; t.text = "?"; ++i;
        } else if (c == '!') {
            t.t = T::Sym; t.text = "!"; t.invalid = true; ++i;
        } else if (is_delim(c)) {
            t.t = T::Sym; t.text = std::string(1, c); ++i;
        } else {
            const usize start = i;
            while (i < s.size() && !is_delim(s[i])) ++i;
            t.text = std::string(s.substr(start, i - start));
            const bool neg = t.text.size() > 1 && t.text[0] == '-';
            bool digits = !t.text.empty() && !(t.text.size() == 1 && neg);
            for (usize k = neg ? 1 : 0; k < t.text.size() && digits; ++k)
                digits = t.text[k] >= '0' && t.text[k] <= '9';
            if (digits) {
                // 32-bit wrapping (measured: 4294967297 == 1, 2147483648 wraps negative).
                u32 v = 0;
                for (usize k = neg ? 1 : 0; k < t.text.size(); ++k) v = v * 10u + static_cast<u32>(t.text[k] - '0');
                if (neg) v = 0u - v;
                t.t = T::Int; t.v = static_cast<i32>(v);
            } else {
                t.t = T::Name;
            }
        }
        out.push_back(std::move(t));
    }
    out.push_back(Tok{});
    return out;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// ---- AST --------------------------------------------------------------------------

struct ColRef {
    std::string table;   ///< qualifier, "" when unqualified
    std::string column;
    i32 t = -1;
    i32 c = -1;
};

struct Operand {
    enum class K : u8 { Column, Int, Str, Null, Param } k = K::Null;
    ColRef col;
    i32 i = 0;
    std::string s;
    u32 param = 0;  ///< 1-based
};

enum class Op : u8 { Eq, Ne, Lt, Gt, Le, Ge, IsNull, IsNotNull };

struct Expr {
    enum class K : u8 { And, Or, Cmp } k = K::Cmp;
    std::unique_ptr<Expr> l, r;
    Op op = Op::Eq;
    Operand a, b;
};

// ---- data access for evaluation ------------------------------------------------------

struct Source {
    std::string name;
    Table* table = nullptr;
    std::unique_ptr<Table> owned;  ///< a materialized virtual table
};

using Combo = std::vector<usize>;  ///< one row index per source

class Parser;

/// Shared parse/bind/evaluate machinery of the concrete statements.
class Base : public Statement {
public:
    Error* err = nullptr;
    std::string sql;
    u32 param_count = 0;
    std::vector<Source> sources;

    std::vector<std::string>& m_tables_ref() { return m_tables; }
    void set_kind(Kind k) { m_kind = k; }

    bool fail(u32 code, std::string a, std::string b, u32 fields = 4) {
        if (err) *err = Error{code, std::move(a), std::move(b), fields};
        return false;
    }
    bool syntax() { return fail(kErrSyntax, sql, {}); }

    /// Resolve the FROM tables against @p pkg (prepare: unknown -> 2228).
    bool open_sources(Package& pkg, bool at_prepare) {
        sources.clear();
        for (const std::string& name : m_tables) {
            Source s;
            s.name = name;
            if (is_virtual_table(name)) {
                s.owned = std::make_unique<Table>(virtual_table(pkg, name));
                s.table = s.owned.get();
            } else {
                s.table = pkg.find_table(name);
            }
            if (s.table == nullptr && at_prepare) {
                return fail(pkg.unloadable(name) ? kErrUnloadable : kErrUnknownTable, name, sql);
            }
            sources.push_back(std::move(s));
        }
        return true;
    }

    bool bind(ColRef& c) {
        c.t = c.c = -1;
        if (!c.table.empty()) {
            for (usize t = 0; t < sources.size(); ++t) {
                if (sources[t].name == c.table) { c.t = static_cast<i32>(t); break; }
            }
            if (c.t < 0) return fail(kErrUnknownTable, c.table, sql);
            const Table* tb = sources[static_cast<usize>(c.t)].table;
            auto idx = tb ? tb->exact_column(c.column) : std::nullopt;
            if (!idx) return fail(kErrUnknownColumn, c.column, sql);
            c.c = static_cast<i32>(*idx);
            return true;
        }
        for (usize t = 0; t < sources.size(); ++t) {
            const Table* tb = sources[t].table;
            auto idx = tb ? tb->exact_column(c.column) : std::nullopt;
            if (!idx) continue;
            if (c.t >= 0) return fail(kErrUnknownColumn, c.column, sql);  // ambiguous
            c.t = static_cast<i32>(t);
            c.c = static_cast<i32>(*idx);
        }
        if (c.t < 0) return fail(kErrUnknownColumn, c.column, sql);
        return true;
    }

    const ColumnDef& def(const ColRef& c) const {
        return sources[static_cast<usize>(c.t)].table->columns[static_cast<usize>(c.c)];
    }

    /// Bind and type-check a WHERE tree (prepare).
    bool check(Expr* e) {
        if (e == nullptr) return true;
        if (e->k != Expr::K::Cmp) return check(e->l.get()) && check(e->r.get());
        if (!bind(e->a.col)) return false;
        const ColumnDef& ca = def(e->a.col);
        if (e->op == Op::IsNull || e->op == Op::IsNotNull) return true;
        const bool ordered = e->op != Op::Eq && e->op != Op::Ne;
        if (e->b.k == Operand::K::Column) {
            if (!bind(e->b.col)) return false;
            const ColumnDef& cb = def(e->b.col);
            if (e->b.col.t == e->a.col.t && e->b.col.c == e->a.col.c) return true;
            if (ca.is_stream() || cb.is_stream() || ca.is_string != cb.is_string) return syntax();
            if (ca.is_string && ordered) return syntax();
            return true;
        }
        if (ca.is_stream()) {
            return e->b.k == Operand::K::Param ? true : fail(kErrTypeMismatch, operand_text(e->b), sql);
        }
        if (ca.is_string && ordered) return syntax();
        if (e->b.k == Operand::K::Int && ca.is_string) return fail(kErrTypeMismatch, operand_text(e->b), sql);
        if (e->b.k == Operand::K::Str && !ca.is_string) return fail(kErrTypeMismatch, operand_text(e->b), sql);
        if (e->b.k == Operand::K::Null && ordered) return syntax();
        return true;
    }

    static std::string operand_text(const Operand& o) {
        if (o.k == Operand::K::Int) return std::to_string(o.i);
        return o.s;
    }

    /// Re-resolve every reference at execute (the schema may have changed).
    bool rebind(Expr* e) {
        if (e == nullptr) return true;
        if (e->k != Expr::K::Cmp) return rebind(e->l.get()) && rebind(e->r.get());
        if (!bind(e->a.col)) return false;
        if (e->b.k == Operand::K::Column && !bind(e->b.col)) return false;
        return true;
    }

    /// A parameter value; *type_error for a string given for an integer column.
    std::optional<Value> param_value(const Record* params, u32 index) {
        if (params == nullptr || index > params->count()) return std::nullopt;
        const Record::Field& f = params->fields[index];
        Value v;
        switch (f.kind) {
        case Record::Kind::Null: v.kind = Value::Kind::Null; break;
        case Record::Kind::Integer: v.kind = Value::Kind::Int; v.i = f.integer; break;
        case Record::Kind::String: {
            v.kind = Value::Kind::Str;
            for (const char16_t u : f.text) {
                // UTF-16 -> UTF-8
                const u32 cp = u;
                if (cp < 0x80) v.s.push_back(static_cast<char>(cp));
                else if (cp < 0x800) { v.s.push_back(static_cast<char>(0xC0 | (cp >> 6))); v.s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else { v.s.push_back(static_cast<char>(0xE0 | (cp >> 12))); v.s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); v.s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
            }
            if (v.s.empty()) v.kind = Value::Kind::Null;
            break;
        }
        case Record::Kind::Stream: v.kind = Value::Kind::Stream; break;
        }
        return v;
    }

    const std::string& cell(const ColRef& c, const Combo& combo) const {
        return sources[static_cast<usize>(c.t)].table->rows[combo[static_cast<usize>(c.t)]][static_cast<usize>(c.c)];
    }

    /// Evaluate a comparison. Integer null fails everything but <> (and = with
    /// a null right side); string null is "".
    bool eval(const Expr* e, const Combo& combo, const Record* params, u32& error) {
        if (e == nullptr) return true;
        if (e->k == Expr::K::And) return eval(e->l.get(), combo, params, error) && eval(e->r.get(), combo, params, error);
        if (e->k == Expr::K::Or) return eval(e->l.get(), combo, params, error) || eval(e->r.get(), combo, params, error);
        const ColumnDef& ca = def(e->a.col);
        const std::string& left = cell(e->a.col, combo);
        if (e->op == Op::IsNull) return left.empty();
        if (e->op == Op::IsNotNull) return !left.empty();
        std::string right;
        bool right_null = false;
        switch (e->b.k) {
        case Operand::K::Column: right = cell(e->b.col, combo); right_null = right.empty(); break;
        case Operand::K::Int: right = std::to_string(e->b.i); break;
        case Operand::K::Str: right = e->b.s; right_null = right.empty(); break;
        case Operand::K::Null: right_null = true; break;
        case Operand::K::Param: {
            auto v = param_value(params, e->b.param);
            if (!v) { error = kErrMissingParams; return false; }
            if (v->kind == Value::Kind::Str && ca.is_integer()) { error = kErrParamType; return false; }
            if (v->kind == Value::Kind::Int) right = std::to_string(v->i);
            else if (v->kind == Value::Kind::Str) right = v->s;
            right_null = v->kind == Value::Kind::Null;
            break;
        }
        }
        if (ca.is_string || ca.is_stream()) {
            const bool eq = left == right;
            return e->op == Op::Eq ? eq : (e->op == Op::Ne ? !eq : false);
        }
        const bool left_null = left.empty();
        if (left_null || right_null) {
            if (e->op == Op::Eq) return left_null && right_null;
            if (e->op == Op::Ne) return left_null != right_null;
            return false;
        }
        const i64 x = cell_int(left).value_or(0);
        const i64 y = cell_int(right).value_or(0);
        switch (e->op) {
        case Op::Eq: return x == y;
        case Op::Ne: return x != y;
        case Op::Lt: return x < y;
        case Op::Gt: return x > y;
        case Op::Le: return x <= y;
        case Op::Ge: return x >= y;
        default: return false;
        }
    }

    /// Top-level AND conjuncts of the WHERE tree.
    static void conjuncts(const Expr* e, std::vector<const Expr*>& out) {
        if (e == nullptr) return;
        if (e->k == Expr::K::And) { conjuncts(e->l.get(), out); conjuncts(e->r.get(), out); return; }
        out.push_back(e);
    }

    /// Every combination of source rows that satisfies @p where, in the order
    /// Windows produces them.
    u32 matches(Package& pkg, const Expr* where, const Record* params, std::vector<Combo>& out) {
        (void)pkg;
        out.clear();
        const usize n = sources.size();
        for (const Source& s : sources) {
            if (s.table == nullptr) return 0;  // a table dropped since prepare: no rows
        }
        std::vector<const Expr*> cj;
        conjuncts(where, cj);
        // Join order: a table whose join column is its leading key is
        // iterated inside the other (msi_database_sql_probe join cases).
        std::vector<bool> inner(n, false);
        auto leading_key = [&](const ColRef& c) {
            const Table* t = sources[static_cast<usize>(c.t)].table;
            return c.c == 0 && !t->columns.empty() && t->columns[0].key();
        };
        for (const Expr* e : cj) {
            if (e->k != Expr::K::Cmp || e->op != Op::Eq || e->b.k != Operand::K::Column) continue;
            if (e->a.col.t == e->b.col.t) continue;
            const bool ka = leading_key(e->a.col), kb = leading_key(e->b.col);
            if (ka && !kb) inner[static_cast<usize>(e->a.col.t)] = true;
            if (kb && !ka) inner[static_cast<usize>(e->b.col.t)] = true;
        }
        std::vector<usize> order;
        for (usize t = 0; t < n; ++t) if (!inner[t]) order.push_back(t);
        for (usize t = 0; t < n; ++t) if (inner[t]) order.push_back(t);
        if (order.empty()) return 0;

        // Hash lookups for equality joins onto an already-placed table.
        struct Lookup { ColRef self; ColRef other; std::unordered_multimap<std::string, usize> index; };
        std::vector<std::optional<Lookup>> lookups(n);
        std::vector<bool> placed(n, false);
        placed[order[0]] = true;
        for (usize p = 1; p < order.size(); ++p) {
            const usize t = order[p];
            for (const Expr* e : cj) {
                if (e->k != Expr::K::Cmp || e->op != Op::Eq || e->b.k != Operand::K::Column) continue;
                const ColRef* self = nullptr; const ColRef* other = nullptr;
                if (static_cast<usize>(e->a.col.t) == t && placed[static_cast<usize>(e->b.col.t)] && e->b.col.t != e->a.col.t) { self = &e->a.col; other = &e->b.col; }
                else if (static_cast<usize>(e->b.col.t) == t && placed[static_cast<usize>(e->a.col.t)] && e->b.col.t != e->a.col.t) { self = &e->b.col; other = &e->a.col; }
                if (!self) continue;
                Lookup l{*self, *other, {}};
                const Table* tb = sources[t].table;
                for (usize r = 0; r < tb->rows.size(); ++r) l.index.emplace(tb->rows[r][static_cast<usize>(self->c)], r);
                lookups[t] = std::move(l);
                break;
            }
            placed[t] = true;
        }

        Combo combo(n, 0);
        u32 error = 0;
        std::function<void(usize)> walk = [&](usize p) {
            if (error) return;
            if (p == order.size()) {
                if (eval(where, combo, params, error) && !error) out.push_back(combo);
                return;
            }
            const usize t = order[p];
            const Table* tb = sources[t].table;
            if (lookups[t]) {
                const std::string& key = cell(lookups[t]->other, combo);
                auto [lo, hi] = lookups[t]->index.equal_range(key);
                std::vector<usize> rows;
                for (auto it = lo; it != hi; ++it) rows.push_back(it->second);
                std::sort(rows.begin(), rows.end());
                for (const usize r : rows) { combo[t] = r; walk(p + 1); if (error) return; }
                return;
            }
            for (usize r = 0; r < tb->rows.size(); ++r) { combo[t] = r; walk(p + 1); if (error) return; }
        };
        walk(0);
        return error;
    }

    u32 exec_error(u32 code, Error& e, const std::string& a = {}, u32 fields = 4) {
        e = Error{code, a, {}, fields};
        return kErrorFunctionFailed;
    }
};

// ---- concrete statements ----------------------------------------------------------------

struct SelectItem {
    enum class K : u8 { Column, Star, Int, Str, Param } k = K::Column;
    ColRef col;
    i32 i = 0;
    std::string s;
    u32 param = 0;
};

class SelectStmt final : public Base {
public:
    bool distinct = false;
    std::vector<SelectItem> items;
    std::unique_ptr<Expr> where;
    std::vector<ColRef> order;

    struct Out { SelectItem item; };
    std::vector<SelectItem> expanded;

    bool bind_all(Package& pkg, bool at_prepare) {
        if (!open_sources(pkg, at_prepare)) return false;
        if (!at_prepare) {
            for (const Source& s : sources) if (!s.table) return true;
        }
        expanded.clear();
        m_columns.clear();
        for (SelectItem it : items) {
            if (it.k == SelectItem::K::Star) {
                for (usize t = 0; t < sources.size(); ++t) {
                    for (usize c = 0; c < sources[t].table->columns.size(); ++c) {
                        SelectItem x;
                        x.k = SelectItem::K::Column;
                        x.col = ColRef{sources[t].name, sources[t].table->columns[c].name, static_cast<i32>(t), static_cast<i32>(c)};
                        expanded.push_back(x);
                    }
                }
                continue;
            }
            if (it.k == SelectItem::K::Column && !bind(it.col)) return false;
            expanded.push_back(it);
        }
        for (const SelectItem& it : expanded) {
            OutputColumn oc;
            if (it.k == SelectItem::K::Column) {
                const ColumnDef& d = def(it.col);
                oc.name = d.name;
                oc.definition = d.definition();
                oc.table = it.col.t;
                oc.column = it.col.c;
            } else if (it.k == SelectItem::K::Str) {
                oc.definition = "f" + std::to_string(it.s.size());  // measured: 'x' -> f1
            } else if (it.k == SelectItem::K::Int) {
                oc.definition = "h4";  // measured: 1 -> h4
            } else {
                oc.definition = "f0";
            }
            m_columns.push_back(std::move(oc));
        }
        if (at_prepare ? !check(where.get()) : !rebind(where.get())) return false;
        for (ColRef& c : order) if (!bind(c)) return false;
        return true;
    }

    u32 execute(Package& pkg, const Record* params, bool, Error& e) override {
        err = &e;
        m_results.clear();
        if (!bind_all(pkg, false)) return kErrorFunctionFailed;
        for (const Source& s : sources) if (!s.table) return 0;
        // Parameter checks happen before any row is read.
        for (const SelectItem& it : expanded) {
            if (it.k == SelectItem::K::Param && (!params || it.param > params->count()))
                return exec_error(kErrMissingParams, e);
        }
        if (param_count && (!params || params->count() < param_count)) return exec_error(kErrMissingParams, e);
        std::vector<Combo> combos;
        if (u32 code = matches(pkg, where.get(), params, combos)) {
            return code == kErrParamType ? exec_error(code, e, std::to_string(1), 3) : exec_error(code, e);
        }
        if (!order.empty()) {
            std::stable_sort(combos.begin(), combos.end(), [&](const Combo& a, const Combo& b) {
                for (const ColRef& c : order) {
                    const i64 ka = pkg.order_key(def(c), cell(c, a));
                    const i64 kb = pkg.order_key(def(c), cell(c, b));
                    if (ka != kb) return ka < kb;
                }
                return false;
            });
        }
        std::set<std::vector<std::string>> seen;
        for (const Combo& combo : combos) {
            ResultRow row;
            row.rows = combo;
            std::vector<std::string> sig;
            for (const SelectItem& it : expanded) {
                Value v;
                switch (it.k) {
                case SelectItem::K::Column: v = cell_value(def(it.col), cell(it.col, combo)); break;
                case SelectItem::K::Int: v.kind = Value::Kind::Int; v.i = it.i; break;
                case SelectItem::K::Str: v.kind = it.s.empty() ? Value::Kind::Null : Value::Kind::Str; v.s = it.s; break;
                case SelectItem::K::Param: v = *param_value(params, it.param); break;
                default: break;
                }
                if (distinct) sig.push_back(std::to_string(static_cast<int>(v.kind)) + ":" + std::to_string(v.i) + ":" + v.s);
                row.values.push_back(std::move(v));
            }
            if (distinct && !seen.insert(sig).second) continue;
            m_results.push_back(std::move(row));
        }
        return 0;
    }
};

struct Assign { ColRef col; Operand value; };

/// A value from a VALUES / SET operand.
std::optional<Value> operand_value(Base& b, const Operand& o, const Record* params, u32& error) {
    Value v;
    switch (o.k) {
    case Operand::K::Int: v.kind = Value::Kind::Int; v.i = o.i; break;
    case Operand::K::Str: v.kind = o.s.empty() ? Value::Kind::Null : Value::Kind::Str; v.s = o.s; break;
    case Operand::K::Null: break;
    case Operand::K::Param: {
        auto p = b.param_value(params, o.param);
        if (!p) { error = kErrMissingParams; return std::nullopt; }
        v = *p;
        break;
    }
    default: break;
    }
    return v;
}

class InsertStmt final : public Base {
public:
    std::vector<std::string> cols;
    std::vector<Operand> values;
    bool temporary = false;

    bool bind_cols(Package& pkg, bool at_prepare, std::vector<usize>& idx) {
        if (!open_sources(pkg, at_prepare)) return false;
        idx.clear();
        if (!sources[0].table) return true;
        for (const std::string& c : cols) {
            auto i = sources[0].table->exact_column(c);
            if (!i) return fail(kErrUnknownColumn, c, sql);
            idx.push_back(*i);
        }
        return true;
    }

    u32 execute(Package& pkg, const Record* params, bool read_only, Error& e) override {
        err = &e;
        std::vector<usize> idx;
        if (!bind_cols(pkg, false, idx)) return kErrorFunctionFailed;
        Table* t = sources[0].table;
        if (t == nullptr) return exec_error(kErrUnknownTable, e, m_tables[0]);
        if (param_count && (!params || params->count() < param_count)) return exec_error(kErrMissingParams, e);
        const std::string& name = m_tables[0];
        std::vector<std::string> cells(t->columns.size());
        std::map<usize, std::vector<u8>> streams;
        for (usize k = 0; k < idx.size(); ++k) {
            u32 error = 0;
            auto v = operand_value(*this, values[k], params, error);
            if (!v) return exec_error(error, e);
            const ColumnDef& col = t->columns[idx[k]];
            if (v->kind == Value::Kind::Stream) {
                const Record::Field& f = params->fields[values[k].param];
                if (!col.is_stream()) return exec_error(kErrUpdateFailed, e);
                streams[idx[k]] = f.stream;
                continue;
            }
            if (v->kind == Value::Kind::Null && !col.nullable())
                return exec_error(kErrBadValue, e, std::to_string(k), 3);
            bool range = false;
            if (!value_to_cell(col, *v, cells[idx[k]], &range))
                return range ? exec_error(kErrBadValue, e, std::to_string(k), 3) : exec_error(kErrUpdateFailed, e);
        }
        if (name == "_Streams" || name == "_Storages") {
            if (read_only) return exec_error(kErrUpdateFailed, e);
            const std::string& key = cells[0];
            if (key.empty() || streams.empty()) return exec_error(kErrUpdateFailed, e);
            if (name == "_Streams") {
                if (pkg.stream(key)) return exec_error(kErrUpdateFailed, e);
                pkg.set_stream(key, streams.begin()->second);
            } else if (!pkg.set_storage_image(key, streams.begin()->second)) {
                return exec_error(kErrUpdateFailed, e);
            }
            return 0;
        }
        if (is_virtual_table(name)) return exec_error(kErrUpdateFailed, e);
        if (read_only && !temporary && !t->temporary()) return exec_error(kErrUpdateFailed, e);
        DbError de;
        if (pkg.insert_row(*t, std::move(cells), temporary || t->temporary(), streams.empty() ? nullptr : &streams,
                           nullptr, &de))
            return exec_error(kErrUpdateFailed, e);
        return 0;
    }
};

class UpdateStmt final : public Base {
public:
    std::vector<Assign> sets;
    std::unique_ptr<Expr> where;

    bool bind_all(Package& pkg, bool at_prepare) {
        if (!open_sources(pkg, at_prepare)) return false;
        for (const Source& s : sources) if (!s.table) return true;
        for (Assign& a : sets) if (!bind(a.col)) return false;
        return at_prepare ? check(where.get()) : rebind(where.get());
    }

    u32 execute(Package& pkg, const Record* params, bool read_only, Error& e) override {
        err = &e;
        if (!bind_all(pkg, false)) return kErrorFunctionFailed;
        for (const Source& s : sources) if (!s.table) return 0;
        if (param_count && (!params || params->count() < param_count)) return exec_error(kErrMissingParams, e);
        std::vector<Value> vals;
        for (const Assign& a : sets) {
            u32 error = 0;
            auto v = operand_value(*this, a.value, params, error);
            if (!v) return exec_error(error, e);
            vals.push_back(*v);
            if (def(a.col).key()) return exec_error(kErrUpdateFailed, e);
        }
        std::vector<Combo> combos;
        if (u32 code = matches(pkg, where.get(), params, combos)) return exec_error(code, e);
        for (const Combo& combo : combos) {
            for (usize k = 0; k < sets.size(); ++k) {
                const ColRef& c = sets[k].col;
                Source& src = sources[static_cast<usize>(c.t)];
                const usize row = combo[static_cast<usize>(c.t)];
                const ColumnDef& col = def(c);
                if (src.owned) {
                    if (src.name == "_Streams" && col.is_stream() && vals[k].kind == Value::Kind::Stream && !read_only) {
                        pkg.set_stream(src.table->rows[row][0], params->fields[sets[k].value.param].stream);
                        continue;
                    }
                    return exec_error(kErrUpdateFailed, e);
                }
                const bool temp_row = row < src.table->row_temporary.size() && src.table->row_temporary[row];
                if (read_only && !temp_row && !col.temporary()) return exec_error(kErrUpdateFailed, e);
                std::string cell;
                const std::vector<u8>* stream = nullptr;
                if (vals[k].kind == Value::Kind::Stream) {
                    stream = &params->fields[sets[k].value.param].stream;
                } else if (!value_to_cell(col, vals[k], cell)) {
                    return exec_error(kErrUpdateFailed, e);
                }
                if (pkg.update_cell(*src.table, row, static_cast<usize>(c.c), cell, stream))
                    return exec_error(kErrUpdateFailed, e);
            }
        }
        return 0;
    }
};

class DeleteStmt final : public Base {
public:
    std::unique_ptr<Expr> where;

    u32 execute(Package& pkg, const Record* params, bool read_only, Error& e) override {
        err = &e;
        if (!open_sources(pkg, false)) return kErrorFunctionFailed;
        if (!sources[0].table) return 0;
        if (!rebind(where.get())) return kErrorFunctionFailed;
        if (param_count && (!params || params->count() < param_count)) return exec_error(kErrMissingParams, e);
        std::vector<Combo> combos;
        if (u32 code = matches(pkg, where.get(), params, combos)) return exec_error(code, e);
        Source& src = sources[0];
        if (src.owned) {
            // _Streams / _Storages rows are real objects; the catalog tables
            // are not deletable row by row (no effect).
            if (read_only && !combos.empty() && (src.name == "_Streams" || src.name == "_Storages"))
                return exec_error(kErrUpdateFailed, e);
            for (const Combo& c : combos) {
                const std::string& key = src.table->rows[c[0]][0];
                if (src.name == "_Streams") pkg.remove_stream(key);
                else if (src.name == "_Storages") pkg.remove_storage(key);
            }
            return 0;
        }
        std::vector<usize> rows;
        for (const Combo& c : combos) rows.push_back(c[0]);
        if (read_only) {
            for (const usize r : rows)
                if (r >= src.table->row_temporary.size() || !src.table->row_temporary[r]) return exec_error(kErrUpdateFailed, e);
        }
        std::sort(rows.begin(), rows.end());
        for (usize k = rows.size(); k-- > 0;) (void)pkg.delete_row(*src.table, rows[k]);
        return 0;
    }
};

class CreateStmt final : public Base {
public:
    std::vector<ColumnDef> cols;
    bool hold = false;

    u32 execute(Package& pkg, const Record*, bool, Error& e) override {
        DbError de;
        if (u32 rc = pkg.create_table(m_tables[0], cols, hold, &de)) {
            e = Error{de.code, de.arg1, sql};
            return rc == kErrorBadQuery ? kErrorFunctionFailed : rc;
        }
        return 0;
    }
};

class DropStmt final : public Base {
public:
    u32 execute(Package& pkg, const Record*, bool, Error& e) override {
        DbError de;
        if (pkg.drop_table(m_tables[0], &de)) {
            e = Error{de.code, de.arg1, {}};
            return kErrorFunctionFailed;
        }
        return 0;
    }
};

class AlterStmt final : public Base {
public:
    ColumnDef col;
    bool hold = false;

    u32 execute(Package& pkg, const Record*, bool, Error& e) override {
        DbError de;
        u32 rc = 0;
        if (m_kind == Kind::AlterAdd) rc = pkg.add_column(m_tables[0], col, hold, &de);
        else if (m_kind == Kind::AlterHold) rc = pkg.hold_table(m_tables[0], &de);
        else rc = pkg.free_table(m_tables[0], &de);
        if (rc) {
            e = Error{de.code, de.arg1, de.arg2.empty() ? sql : de.arg2};
            return kErrorFunctionFailed;
        }
        return 0;
    }
};

// ---- parser --------------------------------------------------------------------------------

class Parser {
public:
    Parser(std::string_view sql, Error& err) : m_sql(sql), m_err(err), m_toks(lex(sql)) {}

    std::unique_ptr<Statement> parse(Package& pkg) {
        for (const Tok& t : m_toks) {
            if (t.invalid) { fail(kErrBadToken, t.text, std::string(m_sql)); return nullptr; }
        }
        if (kw("SELECT")) return select(pkg);
        if (kw("INSERT")) return insert(pkg);
        if (kw("UPDATE")) return update(pkg);
        if (kw("DELETE")) return remove(pkg);
        if (kw("CREATE")) return create(pkg);
        if (kw("DROP")) return drop(pkg);
        if (kw("ALTER")) return alter(pkg);
        syntax();
        return nullptr;
    }

private:
    const Tok& peek(usize k = 0) const { return m_toks[std::min(m_pos + k, m_toks.size() - 1)]; }
    bool is_kw(const Tok& t, const char* k) const { return t.t == T::Name && ieq(t.text, k); }
    bool kw(const char* k) { if (is_kw(peek(), k)) { ++m_pos; return true; } return false; }
    bool sym(const char* s) { if (peek().t == T::Sym && peek().text == s) { ++m_pos; return true; } return false; }
    bool at_end() const { return peek().t == T::End; }
    void fail(u32 code, std::string a, std::string b, u32 fields = 4) { m_err = Error{code, std::move(a), std::move(b), fields}; }
    void syntax() { fail(kErrSyntax, std::string(m_sql), {}); }

    static bool is_keyword(const std::string& s) {
        static const char* kws[] = {"SELECT", "DISTINCT", "FROM", "WHERE", "ORDER", "BY", "AND", "OR", "IS", "NOT",
                                    "NULL", "DELETE", "UPDATE", "SET", "INSERT", "INTO", "VALUES", "TEMPORARY",
                                    "CREATE", "TABLE", "CHAR", "CHARACTER", "LONGCHAR", "SHORT", "INT", "INTEGER",
                                    "LONG", "OBJECT", "LOCALIZABLE", "PRIMARY", "KEY", "HOLD", "FREE", "DROP",
                                    "ALTER", "ADD"};
        for (const char* k : kws) if (ieq(s, k)) return true;
        return false;
    }

    /// A table or column name (quoted, or bare and not a keyword).
    std::optional<std::string> name() {
        const Tok& t = peek();
        if (t.t == T::QName && !t.unterminated) { ++m_pos; return t.text; }
        if (t.t == T::Name && !is_keyword(t.text)) { ++m_pos; return t.text; }
        return std::nullopt;
    }

    std::optional<ColRef> colref() {
        auto a = name();
        if (!a) return std::nullopt;
        ColRef c;
        if (peek().t == T::Sym && peek().text == ".") {
            ++m_pos;
            auto b = name();
            if (!b) return std::nullopt;
            c.table = *a;
            c.column = *b;
        } else {
            c.column = *a;
        }
        return c;
    }

    bool table_list(Base& s) {
        for (;;) {
            auto n = name();
            if (!n) { syntax(); return false; }
            for (const std::string& t : s.m_tables_ref()) {
                if (t == *n) { fail(kErrSelfJoin, *n, std::string(m_sql)); return false; }
            }
            s.m_tables_ref().push_back(*n);
            if (!sym(",")) return true;
        }
    }

    std::optional<Op> comparison_op() {
        if (sym("=")) return Op::Eq;
        if (sym("<")) {
            if (sym(">")) return Op::Ne;
            if (sym("=")) return Op::Le;
            return Op::Lt;
        }
        if (sym(">")) {
            if (sym("=")) return Op::Ge;
            return Op::Gt;
        }
        return std::nullopt;
    }

    std::optional<Operand> value_operand(Base& s, bool allow_column) {
        const Tok& t = peek();
        Operand o;
        if (t.t == T::Int) { ++m_pos; o.k = Operand::K::Int; o.i = static_cast<i32>(t.v); return o; }
        if (t.t == T::Str) {
            ++m_pos;
            if (t.unterminated) { fail(kErrTypeMismatch, {}, std::string(m_sql)); m_failed = true; return std::nullopt; }
            o.k = Operand::K::Str; o.s = t.text; return o;
        }
        if (t.t == T::Param) { ++m_pos; o.k = Operand::K::Param; o.param = ++s.param_count; return o; }
        if (is_kw(t, "NULL")) { ++m_pos; o.k = Operand::K::Null; return o; }
        if (allow_column) {
            if (auto c = colref()) { o.k = Operand::K::Column; o.col = *c; return o; }
        }
        return std::nullopt;
    }

    std::unique_ptr<Expr> where_or(Base& s) {
        auto l = where_and(s);
        if (!l) return nullptr;
        while (kw("OR")) {
            auto r = where_and(s);
            if (!r) return nullptr;
            auto e = std::make_unique<Expr>();
            e->k = Expr::K::Or; e->l = std::move(l); e->r = std::move(r);
            l = std::move(e);
        }
        return l;
    }

    std::unique_ptr<Expr> where_and(Base& s) {
        auto l = where_factor(s);
        if (!l) return nullptr;
        while (kw("AND")) {
            auto r = where_factor(s);
            if (!r) return nullptr;
            auto e = std::make_unique<Expr>();
            e->k = Expr::K::And; e->l = std::move(l); e->r = std::move(r);
            l = std::move(e);
        }
        return l;
    }

    std::unique_ptr<Expr> where_factor(Base& s) {
        if (sym("(")) {
            auto e = where_or(s);
            if (!e) return nullptr;
            if (!sym(")")) { syntax(); m_failed = true; return nullptr; }
            return e;
        }
        auto c = colref();
        if (!c) {
            // A constant (or NOT) where a column must stand.
            const Tok& t = peek();
            if (t.t == T::End) syntax();
            else fail(kErrTypeMismatch, t.t == T::Int ? std::to_string(t.v) : t.text, std::string(m_sql));
            m_failed = true;
            return nullptr;
        }
        auto e = std::make_unique<Expr>();
        e->a.k = Operand::K::Column;
        e->a.col = *c;
        if (++m_comparisons > kMaxComparisons) { syntax(); m_failed = true; return nullptr; }
        if (kw("IS")) {
            const bool negate = kw("NOT");
            if (!kw("NULL")) { syntax(); m_failed = true; return nullptr; }
            e->op = negate ? Op::IsNotNull : Op::IsNull;
            return e;
        }
        auto op = comparison_op();
        if (!op) { syntax(); m_failed = true; return nullptr; }
        e->op = *op;
        auto b = value_operand(s, true);
        if (!b) { if (!m_failed) syntax(); m_failed = true; return nullptr; }
        e->b = *b;
        return e;
    }

    std::unique_ptr<Expr> where_clause(Base& s, bool& ok) {
        ok = true;
        if (!kw("WHERE")) return nullptr;
        auto e = where_or(s);
        if (!e) { if (!m_failed) syntax(); ok = false; }
        return e;
    }

    template <typename S>
    static std::unique_ptr<Statement> finish(std::unique_ptr<S> s, bool bound) {
        if (!bound) return nullptr;
        return s;
    }

    std::unique_ptr<Statement> select(Package& pkg) {
        auto s = std::make_unique<SelectStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        s->set_kind(Kind::Select);
        s->distinct = kw("DISTINCT");
        for (;;) {
            SelectItem it;
            const Tok& t = peek();
            if (t.t == T::Sym && t.text == "*") { ++m_pos; it.k = SelectItem::K::Star; }
            else if (t.t == T::Int) { ++m_pos; it.k = SelectItem::K::Int; it.i = static_cast<i32>(t.v); }
            else if (t.t == T::Str && !t.unterminated) { ++m_pos; it.k = SelectItem::K::Str; it.s = t.text; }
            else if (t.t == T::Param) { ++m_pos; it.k = SelectItem::K::Param; it.param = ++s->param_count; }
            else if (auto c = colref()) { it.col = *c; }
            else break;
            s->items.push_back(std::move(it));
            if (!sym(",")) break;
        }
        if (!kw("FROM")) {
            if (s->items.empty() || !at_end()) {
                // A list that runs into something other than FROM.
                bool has_from = false;
                for (usize k = m_pos; k < m_toks.size(); ++k) if (is_kw(m_toks[k], "FROM")) has_from = true;
                if (has_from) { syntax(); return nullptr; }
            }
            fail(kErrNoFrom, std::string(m_sql), {});
            return nullptr;
        }
        if (s->items.empty()) { syntax(); return nullptr; }
        if (!table_list(*s)) return nullptr;
        bool ok = true;
        s->where = where_clause(*s, ok);
        if (!ok) return nullptr;
        if (kw("ORDER")) {
            if (!kw("BY")) { syntax(); return nullptr; }
            for (;;) {
                auto c = colref();
                if (!c) { syntax(); return nullptr; }
                s->order.push_back(*c);
                if (!sym(",")) break;
            }
        }
        if (!at_end()) { syntax(); return nullptr; }
        const bool bound = s->bind_all(pkg, true);
        return finish(std::move(s), bound);
    }

    std::unique_ptr<Statement> insert(Package& pkg) {
        auto s = std::make_unique<InsertStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        s->set_kind(Kind::Insert);
        if (!kw("INTO")) { syntax(); return nullptr; }
        auto t = name();
        if (!t) { syntax(); return nullptr; }
        s->m_tables_ref().push_back(*t);
        if (!sym("(")) { syntax(); return nullptr; }
        for (;;) {
            auto c = name();
            if (!c) { syntax(); return nullptr; }
            s->cols.push_back(*c);
            if (sym(")")) break;
            if (!sym(",")) { syntax(); return nullptr; }
        }
        if (!kw("VALUES") || !sym("(")) { syntax(); return nullptr; }
        for (;;) {
            auto v = value_operand(*s, false);
            if (!v) { if (!m_failed) syntax(); return nullptr; }
            s->values.push_back(*v);
            if (sym(")")) break;
            if (!sym(",")) { syntax(); return nullptr; }
        }
        s->temporary = kw("TEMPORARY");
        if (!at_end()) { syntax(); return nullptr; }
        if (s->values.size() != s->cols.size()) { fail(kErrValueCount, std::string(m_sql), {}); return nullptr; }
        std::vector<usize> idx;
        const bool bound = s->bind_cols(pkg, true, idx);
        return finish(std::move(s), bound);
    }

    std::unique_ptr<Statement> update(Package& pkg) {
        auto s = std::make_unique<UpdateStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        s->set_kind(Kind::Update);
        if (!table_list(*s)) return nullptr;
        if (!kw("SET")) { syntax(); return nullptr; }
        for (;;) {
            auto c = colref();
            if (!c || !sym("=")) { syntax(); return nullptr; }
            auto v = value_operand(*s, false);
            if (!v) { if (!m_failed) syntax(); return nullptr; }
            s->sets.push_back(Assign{*c, *v});
            if (!sym(",")) break;
        }
        bool ok = true;
        s->where = where_clause(*s, ok);
        if (!ok) return nullptr;
        if (!at_end()) { syntax(); return nullptr; }
        const bool bound = s->bind_all(pkg, true);
        return finish(std::move(s), bound);
    }

    std::unique_ptr<Statement> remove(Package& pkg) {
        auto s = std::make_unique<DeleteStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        s->set_kind(Kind::Delete);
        if (!kw("FROM")) { syntax(); return nullptr; }
        auto t = name();
        if (!t) { syntax(); return nullptr; }
        s->m_tables_ref().push_back(*t);
        bool ok = true;
        s->where = where_clause(*s, ok);
        if (!ok) return nullptr;
        if (!at_end()) { syntax(); return nullptr; }
        const bool bound = s->open_sources(pkg, true) && s->check(s->where.get());
        return finish(std::move(s), bound);
    }

    /// One column definition: name type [NOT NULL] [TEMPORARY] [LOCALIZABLE].
    std::optional<ColumnDef> coldef() {
        auto n = name();
        if (!n) return std::nullopt;
        char kind = 'i';
        u32 size = 2;
        if (kw("CHAR") || kw("CHARACTER")) {
            kind = 's';
            size = 0;
            if (sym("(")) {
                if (peek().t != T::Int || peek().v < 0 || peek().v > 255) return std::nullopt;
                size = static_cast<u32>(peek().v);
                ++m_pos;
                if (!sym(")")) return std::nullopt;
            }
        } else if (kw("LONGCHAR")) {
            kind = 's'; size = 0;
        } else if (kw("SHORT") || kw("INT") || kw("INTEGER")) {
            kind = 'i'; size = 2;
        } else if (kw("LONG")) {
            kind = 'i'; size = 4;
        } else if (kw("OBJECT")) {
            kind = 'v'; size = 0;
        } else {
            return std::nullopt;
        }
        bool not_null = false, temporary = false, localizable = false;
        for (;;) {
            if (is_kw(peek(), "NOT") && is_kw(peek(1), "NULL")) { m_pos += 2; not_null = true; continue; }
            if (kw("TEMPORARY")) { temporary = true; continue; }
            if (kw("LOCALIZABLE")) { localizable = true; continue; }
            break;
        }
        return make_column(*n, kind, size, !not_null, false, localizable && kind == 's', temporary);
    }

    std::unique_ptr<Statement> create(Package& pkg) {
        auto s = std::make_unique<CreateStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        s->set_kind(Kind::CreateTable);
        if (!kw("TABLE")) { syntax(); return nullptr; }
        auto t = name();
        if (!t) { syntax(); return nullptr; }
        s->m_tables_ref().push_back(*t);
        if (!sym("(")) { syntax(); return nullptr; }
        std::vector<std::string> keys;
        for (;;) {
            auto c = coldef();
            if (!c) { syntax(); return nullptr; }
            for (const ColumnDef& d : s->cols) {
                if (d.name == c->name) { fail(kErrDuplicateColumn, c->name, std::string(m_sql)); return nullptr; }
            }
            s->cols.push_back(std::move(*c));
            if (sym(",")) continue;
            if (kw("PRIMARY")) {
                if (!kw("KEY")) { syntax(); return nullptr; }
                for (;;) {
                    auto k = name();
                    if (!k) { syntax(); return nullptr; }
                    keys.push_back(*k);
                    if (!sym(",")) break;
                }
                if (!sym(")")) { syntax(); return nullptr; }
                break;
            }
            syntax();
            return nullptr;
        }
        s->hold = kw("HOLD");
        if (!at_end() || keys.empty()) { syntax(); return nullptr; }
        // Key columns lead, in PRIMARY KEY order (measured: (`A`, `B`) PRIMARY
        // KEY `B` reports columns B, A).
        std::vector<ColumnDef> ordered;
        for (const std::string& k : keys) {
            auto it = std::find_if(s->cols.begin(), s->cols.end(), [&](const ColumnDef& d) { return d.name == k; });
            if (it == s->cols.end()) { fail(kErrUnknownColumn, k, std::string(m_sql)); return nullptr; }
            it->type |= coltype::kKey;
            ordered.push_back(*it);
            s->cols.erase(it);
        }
        for (ColumnDef& d : s->cols) ordered.push_back(std::move(d));
        s->cols = std::move(ordered);
        if (pkg.find_table(*t) != nullptr || is_virtual_table(*t)) {
            fail(kErrTableExists, *t, std::string(m_sql));
            return nullptr;
        }
        return s;
    }

    std::unique_ptr<Statement> drop(Package& pkg) {
        auto s = std::make_unique<DropStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        s->set_kind(Kind::DropTable);
        if (!kw("TABLE")) { syntax(); return nullptr; }
        auto t = name();
        if (!t || !at_end()) { syntax(); return nullptr; }
        s->m_tables_ref().push_back(*t);
        if (!is_virtual_table(*t) && pkg.find_table(*t) == nullptr) {
            fail(kErrUnknownTable, *t, std::string(m_sql));
            return nullptr;
        }
        return s;
    }

    std::unique_ptr<Statement> alter(Package& pkg) {
        auto s = std::make_unique<AlterStmt>();
        s->err = &m_err;
        s->sql = std::string(m_sql);
        if (!kw("TABLE")) { syntax(); return nullptr; }
        auto t = name();
        if (!t) { syntax(); return nullptr; }
        s->m_tables_ref().push_back(*t);
        if (kw("ADD")) {
            s->set_kind(Kind::AlterAdd);
            auto c = coldef();
            if (!c) { syntax(); return nullptr; }
            s->col = *c;
            s->hold = kw("HOLD");
        } else if (kw("HOLD")) {
            s->set_kind(Kind::AlterHold);
        } else if (kw("FREE")) {
            s->set_kind(Kind::AlterFree);
        } else {
            syntax();
            return nullptr;
        }
        if (!at_end()) { syntax(); return nullptr; }
        Table* tb = pkg.find_table(*t);
        if (tb == nullptr) { fail(kErrUnknownTable, *t, std::string(m_sql)); return nullptr; }
        if (s->kind() == Kind::AlterAdd && tb->exact_column(s->col.name)) {
            fail(kErrDuplicateColumn, s->col.name, std::string(m_sql));
            return nullptr;
        }
        return s;
    }

    std::string_view m_sql;
    Error& m_err;
    std::vector<Tok> m_toks;
    usize m_pos = 0;
    usize m_comparisons = 0;
    bool m_failed = false;
};

}  // namespace

// ---- public helpers ------------------------------------------------------------------------

std::unique_ptr<Statement> Statement::prepare(Package& pkg, std::string_view sql, Error& err) {
    std::lock_guard lock(pkg.mutex());
    err = Error{};
    Parser p(sql, err);
    return p.parse(pkg);
}

bool is_virtual_table(std::string_view name) {
    return name == "_Tables" || name == "_Columns" || name == "_Streams" || name == "_Storages";
}

Table virtual_table(Package& pkg, std::string_view name) {
    Table t;
    t.name = std::string(name);
    auto cfb_less = [](const std::string& a, const std::string& b) {
        const std::u16string ea = Package::encode_stream_name(a, false), eb = Package::encode_stream_name(b, false);
        if (ea.size() != eb.size()) return ea.size() < eb.size();
        for (usize i = 0; i < ea.size(); ++i) {
            const char16_t x = (ea[i] >= u'a' && ea[i] <= u'z') ? ea[i] - 32 : ea[i];
            const char16_t y = (eb[i] >= u'a' && eb[i] <= u'z') ? eb[i] - 32 : eb[i];
            if (x != y) return x < y;
        }
        return false;
    };
    if (name == "_Tables") {
        // Measured: Name s64 and an unnamed temporary object column.
        t.columns.push_back(make_column("Name", 's', 64, false, true, false, false));
        t.columns.push_back(make_column("", 'v', 0, true, false, false, true));
        for (const std::string& n : pkg.table_names()) {
            const Table* tb = pkg.table(n);
            if (tb && !tb->temporary()) t.rows.push_back({n, ""});
        }
    } else if (name == "_Columns") {
        t.columns.push_back(make_column("Table", 's', 64, false, true, false, false));
        t.columns.push_back(make_column("Number", 'i', 2, false, true, false, false));
        t.columns.push_back(make_column("Name", 's', 64, true, false, false, false));
        t.columns.push_back(make_column("Type", 'i', 2, false, false, false, false));
        for (const std::string& n : pkg.table_names()) {
            const Table* tb = pkg.table(n);
            if (!tb || tb->temporary()) continue;
            u32 number = 0;
            for (const ColumnDef& c : tb->columns) {
                if (c.temporary()) continue;
                t.rows.push_back({n, std::to_string(++number), c.name, std::to_string(c.type & 0x7FFF)});
            }
        }
    } else {
        // `_Streams` / `_Storages`: Name s62, Data V0 (measured).
        t.columns.push_back(make_column("Name", 's', 62, false, true, false, false));
        t.columns.push_back(make_column("Data", 'v', 0, true, false, false, false));
        std::vector<std::string> names = name == "_Streams" ? pkg.stream_names() : pkg.storage_names();
        std::sort(names.begin(), names.end(), cfb_less);
        for (const std::string& n : names) t.rows.push_back({n, n});
    }
    t.row_temporary.assign(t.rows.size(), 0);
    return t;
}

bool value_to_cell(const ColumnDef& column, const Value& v, std::string& cell, bool* range_error) {
    if (range_error) *range_error = false;
    switch (v.kind) {
    case Value::Kind::Null:
        cell.clear();
        return true;
    case Value::Kind::Stream:
        return false;
    case Value::Kind::Int:
        if (column.is_stream()) return false;
        if (column.is_integer() && column.size() != 4 && (v.i < -32768 || v.i > 32767)) {
            if (range_error) *range_error = true;
            return false;
        }
        // MSI_NULL_INTEGER is null.
        cell = v.i == INT32_MIN ? std::string{} : std::to_string(v.i);
        return true;
    case Value::Kind::Str:
        if (column.is_stream()) return false;
        if (column.is_integer()) {
            auto n = cell_int(v.s);
            if (!n) return false;
            if (column.size() != 4 && (*n < -32768 || *n > 32767)) {
                if (range_error) *range_error = true;
                return false;
            }
            cell = std::to_string(*n);
            return true;
        }
        cell = v.s;
        return true;
    }
    return false;
}

Value cell_value(const ColumnDef& column, const std::string& cell) {
    Value v;
    if (cell.empty()) return v;
    if (column.is_stream()) { v.kind = Value::Kind::Stream; v.s = cell; return v; }
    if (column.is_integer()) {
        auto n = cell_int(cell);
        if (n) { v.kind = Value::Kind::Int; v.i = *n; return v; }
    }
    v.kind = Value::Kind::Str;
    v.s = cell;
    return v;
}

}  // namespace vitreo::winapi::msi::sql
