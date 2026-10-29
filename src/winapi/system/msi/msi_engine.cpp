// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_engine.cpp
/// @brief Install-session state: properties, the Formatted and Conditional
///        grammars, directory resolution, and the msiexec command line.
///
/// Every rule below cites Microsoft Learn ("Formatted", "Conditional
/// Statement Syntax", "Command-Line Options") or the oracle probe
/// tools/oracle_probe/msi_engine_session_probe.c (msi_engine32/64.expected,
/// "fmt"/"cond" lines), which settles what Learn leaves open.

#include "msi_engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace vitreo::winapi::msi {

namespace {

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string to_lower(std::string s) {
    for (char& c : s) c = ascii_lower(c);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) if (c < '0' || c > '9') return false;
    return true;
}

/// Learn "Conditional Statement Syntax": a value is compared as an integer
/// when it converts to one. Probe: "-5" converts; "+5", " 5" and "0x10" do
/// not (cond Plus/Sp/Hex = N are false); 99999999999 does (cond Big lines),
/// so the range is wider than 32 bits.
std::optional<i64> strict_integer(std::string_view s) {
    usize i = 0;
    bool negative = false;
    if (!s.empty() && s[0] == '-') { negative = true; i = 1; }
    if (i >= s.size()) return std::nullopt;
    i64 value = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        if (value > (INT64_MAX - 9) / 10) return std::nullopt;
        value = value * 10 + (s[i] - '0');
    }
    return negative ? -value : value;
}

std::string record_field_text(const Record& record, u32 index, bool& present) {
    present = false;
    if (index >= record.fields.size()) return {};
    const Record::Field& f = record.fields[index];
    std::string out;
    switch (f.kind) {
        case Record::Kind::Null: return {};
        case Record::Kind::Integer: out = std::to_string(f.integer); break;
        case Record::Kind::String: {
            out.reserve(f.text.size());
            for (usize k = 0; k < f.text.size(); ++k) {
                u32 cp = f.text[k];
                if (cp >= 0xD800 && cp <= 0xDBFF && k + 1 < f.text.size()) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (f.text[++k] - 0xDC00);
                }
                if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
            }
            break;
        }
        case Record::Kind::Stream: return {};  // a stream field formats as nothing
    }
    present = !out.empty();
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// msiexec command line (Learn "Command-Line Options" and "Standard Installer
// Command-Line Options")
// ---------------------------------------------------------------------------

namespace {

/// Split like msiexec: whitespace separates, double quotes group, and a
/// doubled quote inside a quoted run is a literal quote (Learn: to include a
/// quote in a property value use two, PROPERTY="Embedded ""Quotes"" White").
std::vector<std::string> split_command_line(const std::string& line) {
    std::vector<std::string> args;
    std::string current;
    bool in_quotes = false;
    bool have_token = false;
    for (usize i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                ++i;
                continue;
            }
            in_quotes = !in_quotes;
            have_token = true;
            continue;
        }
        if (!in_quotes && std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (have_token) { args.push_back(current); current.clear(); have_token = false; }
            continue;
        }
        current.push_back(c);
        have_token = true;
    }
    if (have_token) args.push_back(current);
    return args;
}

// INSTALLUILEVEL (Msi.h).
constexpr u32 kUiNone = 2, kUiBasic = 3, kUiReduced = 4, kUiFull = 5;
constexpr u32 kUiEndDialog = 0x80, kUiProgressOnly = 0x40, kUiHideCancel = 0x20;

} // namespace

std::optional<MsiexecCommand> parse_msiexec_command_line(const std::string& command_line) {
    const std::vector<std::string> args = split_command_line(command_line);
    if (args.empty()) return std::nullopt;

    MsiexecCommand cmd;
    bool mode_seen = false;

    auto set_mode = [&](MsiexecCommand::Mode mode) {
        // Two different operations on one line is an invalid command line.
        if (mode_seen && cmd.mode != mode && cmd.invalid.empty()) cmd.invalid = "multiple operations";
        cmd.mode = mode;
        mode_seen = true;
    };

    for (usize i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg.empty()) continue;

        if (arg[0] == '/' || arg[0] == '-') {
            const std::string sw = to_lower(arg.substr(1));
            if (sw.empty()) { if (cmd.invalid.empty()) cmd.invalid = arg; continue; }
            auto next_value = [&]() -> std::string {
                if (i + 1 < args.size()) return args[++i];
                return std::string{};
            };
            auto value_after = [&](usize letters) -> std::string {
                if (arg.size() > 1 + letters) return arg.substr(1 + letters);
                return next_value();
            };

            // Long forms first (Learn "Standard Installer Command-Line Options").
            if (sw == "package") { set_mode(MsiexecCommand::Mode::Install); cmd.package = next_value(); continue; }
            if (sw == "uninstall") { set_mode(MsiexecCommand::Mode::Uninstall); cmd.package = next_value(); continue; }
            if (sw == "update") { set_mode(MsiexecCommand::Mode::Patch); cmd.package = next_value(); continue; }
            if (sw == "quiet") { cmd.ui_level = kUiNone; cmd.quiet = true; continue; }
            if (sw == "passive") {
                cmd.ui_level = kUiBasic | kUiProgressOnly | kUiHideCancel;
                cmd.quiet = true;
                continue;
            }
            if (sw == "norestart") { cmd.restart = MsiexecCommand::Restart::No; continue; }
            if (sw == "forcerestart") { cmd.restart = MsiexecCommand::Restart::Force; continue; }
            if (sw == "promptrestart") { cmd.restart = MsiexecCommand::Restart::Prompt; continue; }
            if (sw == "log") { cmd.log_modes = "*"; cmd.log_file = next_value(); continue; }
            if (sw == "help" || sw == "?" || sw == "h") { set_mode(MsiexecCommand::Mode::Help); continue; }

            const char letter = sw[0];
            switch (letter) {
                case 'i': set_mode(MsiexecCommand::Mode::Install); cmd.package = value_after(1); break;
                case 'x': set_mode(MsiexecCommand::Mode::Uninstall); cmd.package = value_after(1); break;
                case 'a': set_mode(MsiexecCommand::Mode::Administrative); cmd.package = value_after(1); break;
                case 'p': set_mode(MsiexecCommand::Mode::Patch); cmd.package = value_after(1); break;
                case 'y': set_mode(MsiexecCommand::Mode::RegisterServer); cmd.package = value_after(1); break;
                case 'z': set_mode(MsiexecCommand::Mode::UnregisterServer); cmd.package = value_after(1); break;
                case 'j': {
                    set_mode(MsiexecCommand::Mode::Advertise);
                    if (sw.size() >= 2 && (sw[1] == 'u' || sw[1] == 'm')) {
                        cmd.advertise_target = sw[1];
                        cmd.package = value_after(2);
                    } else if (cmd.invalid.empty()) {
                        cmd.invalid = arg;  // /j needs u or m
                    }
                    break;
                }
                case 'f': {
                    set_mode(MsiexecCommand::Mode::Repair);
                    std::string letters = sw.substr(1);
                    for (const char c : letters) {
                        if (std::string("pecdoamusv").find(c) == std::string::npos && cmd.invalid.empty()) cmd.invalid = arg;
                    }
                    // Learn: "/f ... The default argument list is 'pecms'".
                    cmd.repair_modes = letters.empty() ? "pecms" : letters;
                    cmd.package = next_value();
                    break;
                }
                case 'q': {
                    const std::string q = sw.substr(1);
                    usize k = 0;
                    u32 level = kUiNone;
                    if (k < q.size()) {
                        switch (q[k]) {
                            case 'n': level = kUiNone; ++k; break;
                            case 'b': level = kUiBasic; ++k; break;
                            case 'r': level = kUiReduced; ++k; break;
                            case 'f': level = kUiFull; ++k; break;
                            default: break;
                        }
                    }
                    for (; k < q.size(); ++k) {
                        if (q[k] == '+') level |= kUiEndDialog;
                        else if (q[k] == '-' && (level & 0xF) == kUiBasic) level |= kUiProgressOnly;
                        else if (q[k] == '!' && (level & 0xF) == kUiBasic) level |= kUiHideCancel;
                        else if (cmd.invalid.empty()) cmd.invalid = arg;
                    }
                    cmd.ui_level = level;
                    cmd.quiet = (level & 0xF) == kUiNone || (level & 0xF) == kUiBasic;
                    break;
                }
                case 'l': {
                    cmd.log_modes = sw.substr(1);
                    for (const char c : cmd.log_modes) {
                        if (std::string("iwearucmopvx+!*").find(c) == std::string::npos && cmd.invalid.empty()) cmd.invalid = arg;
                    }
                    cmd.log_file = next_value();
                    break;
                }
                case 't': cmd.transforms = value_after(1); break;
                case 'g': cmd.language = value_after(1); break;
                case 'c': break;  // /c with /j: advertise a new instance
                case 'n': (void)value_after(1); break;  // /n {ProductCode}: instance
                default:
                    if (cmd.invalid.empty()) cmd.invalid = arg;
                    break;
            }
            continue;
        }

        // PROPERTY=VALUE.
        const usize eq = arg.find('=');
        if (eq != std::string::npos && eq > 0) {
            cmd.properties[arg.substr(0, eq)] = arg.substr(eq + 1);
            continue;
        }

        // A bare path with no switch: the package for an implicit install.
        if (cmd.package.empty() && !mode_seen) {
            cmd.package = arg;
            continue;
        }
        if (cmd.invalid.empty()) cmd.invalid = arg;
    }

    if (!mode_seen && !cmd.package.empty()) cmd.mode = MsiexecCommand::Mode::Install;
    return cmd;
}

std::string default_dir_target(const std::string& default_dir) {
    std::string target = default_dir;
    const usize colon = target.find(':');
    if (colon != std::string::npos) target = target.substr(0, colon);
    const usize pipe = target.find('|');
    if (pipe != std::string::npos) target = target.substr(pipe + 1);
    if (target == ".") return std::string{};
    return target;
}

// ---------------------------------------------------------------------------
// Session setup and properties
// ---------------------------------------------------------------------------

bool Session::initialize(std::shared_ptr<Package> package) {
    if (package == nullptr) return false;
    m_package = std::move(package);

    const Table* property_table = m_package->table("Property");
    if (property_table == nullptr) return false;

    const auto name_col = property_table->column_index("Property");
    const auto value_col = property_table->column_index("Value");
    if (!name_col.has_value() || !value_col.has_value()) return false;

    for (const auto& row : property_table->rows) {
        if (*name_col >= row.size() || *value_col >= row.size()) continue;
        if (row[*name_col].empty() || row[*value_col].empty()) continue;
        m_properties[row[*name_col]] = row[*value_col];
    }
    return true;
}

std::string Session::property(const std::string& name) const {
    auto it = m_properties.find(name);
    return (it == m_properties.end()) ? std::string{} : it->second;
}

bool Session::has_property(const std::string& name) const {
    return m_properties.find(name) != m_properties.end();
}

void Session::set_property(const std::string& name, const std::string& value) {
    if (value.empty()) {
        m_properties.erase(name);
        return;
    }
    m_properties[name] = value;
}

bool Session::is_public_property(const std::string& name) {
    if (name.empty()) return false;
    for (const char c : name) if (c >= 'a' && c <= 'z') return false;
    return true;
}

std::optional<std::string> Session::environment(const std::string& name) const {
    if (!m_environment) return std::nullopt;
    return m_environment(name);
}

// ---------------------------------------------------------------------------
// Formatted strings
// ---------------------------------------------------------------------------

namespace {

struct FormatState {
    bool any_reference{false};
    bool all_resolved{true};
};

class Formatter {
public:
    Formatter(const Session& session, const Record* record, bool with_session)
        : m_session(session), m_record(record), m_with_session(with_session) {}

    std::string expand(std::string_view s, FormatState& st, int depth = 0) const {
        std::string out;
        out.reserve(s.size());
        usize i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '[') {
                // [\x]: the first character after the backslash, nothing else
                // (probe: [\abc] -> "a"; [\] has no character and stays literal).
                if (i + 1 < s.size() && s[i + 1] == '\\') {
                    if (i + 2 < s.size()) {
                        const usize close = s.find(']', i + 3);
                        if (close != std::string_view::npos) {
                            out.push_back(s[i + 2]);
                            st.any_reference = true;
                            i = close + 1;
                            continue;
                        }
                    }
                    out.append(s.substr(i));
                    break;
                }
                // Brackets pair by nesting; an opening bracket with no partner
                // leaves the rest literal (probe: "[[S]" stays "[[S]").
                usize j = i + 1;
                int level = 1;
                for (; j < s.size(); ++j) {
                    if (s[j] == '[') ++level;
                    else if (s[j] == ']' && --level == 0) break;
                }
                if (j >= s.size()) {
                    out.append(s.substr(i));
                    break;
                }
                std::string_view inner = s.substr(i + 1, j - i - 1);
                std::string name;
                if (inner.find('[') != std::string_view::npos && depth < 32) {
                    FormatState nested;
                    name = expand(inner, nested, depth + 1);  // [[A]]: inside out
                } else {
                    name = std::string(inner);
                }
                st.any_reference = true;
                bool resolved = false;
                out += resolve(name, inner, resolved, depth);
                if (!resolved) st.all_resolved = false;
                i = j + 1;
                continue;
            }
            if (c == '{') {
                // {{..}}: text for the log only, never part of the result
                // (probe: "{{abc}}" and "{{[S]}}" format as "").
                if (i + 1 < s.size() && s[i + 1] == '{') {
                    const usize close = s.find("}}", i + 2);
                    if (close != std::string_view::npos) { i = close + 2; continue; }
                    out.push_back(c);
                    ++i;
                    continue;
                }
                usize j = i + 1;
                int level = 1;
                for (; j < s.size(); ++j) {
                    if (s[j] == '{') ++level;
                    else if (s[j] == '}' && --level == 0) break;
                }
                if (j >= s.size()) { out.push_back(c); ++i; continue; }
                FormatState block;
                const std::string body = expand(s.substr(i + 1, j - i - 1), block, depth + 1);
                if (!block.any_reference) out += "{" + body + "}";  // no references: unchanged
                else if (block.all_resolved) out += body;
                i = j + 1;
                continue;
            }
            out.push_back(c);
            ++i;
        }
        return out;
    }

private:
    std::string resolve(const std::string& name, std::string_view raw, bool& resolved, int depth) const {
        resolved = false;
        if (name.empty()) return {};
        if (name == "~") { resolved = true; return std::string(1, '\0'); }
        if (all_digits(name)) {
            if (m_record == nullptr) return "[" + std::string(raw) + "]";
            const u64 index = std::strtoull(name.c_str(), nullptr, 10);
            if (index == 0) { resolved = true; return "[" + std::string(raw) + "]"; }  // probe: [0] literal
            bool present = false;
            const std::string value = index <= 0xFFFFFFFFull
                ? record_field_text(*m_record, static_cast<u32>(index), present) : std::string{};
            if (!present) return {};
            resolved = true;
            // A field's text is itself formatted for properties, but not for
            // field numbers again (probe: field "[S]" -> "abc", "[1]" stays).
            if (value.find('[') == std::string::npos && value.find('{') == std::string::npos) return value;
            Formatter inner(m_session, nullptr, m_with_session);
            FormatState st;
            return inner.expand(value, st, depth + 1);
        }
        if (!m_with_session) return "[" + std::string(raw) + "]";  // hInstall 0: probe "a[S]"
        const char prefix = name[0];
        if (prefix == '%') {
            auto v = m_session.environment(name.substr(1));
            if (v.has_value() && !v->empty()) { resolved = true; return *v; }
            return {};
        }
        if (prefix == '#' || prefix == '!') {
            const auto& fn = m_session.item_states().file_path;
            if (fn) {
                auto v = fn(name.substr(1), prefix == '!');
                if (v.has_value() && !v->empty()) { resolved = true; return *v; }
            }
            return {};
        }
        if (prefix == '$') {
            const auto& fn = m_session.item_states().component_directory;
            if (fn) {
                auto v = fn(name.substr(1));
                if (v.has_value() && !v->empty()) { resolved = true; return *v; }
            }
            return {};
        }
        // A property value is inserted as is (probe: [Br] with Br="[S]" -> "[S]").
        std::string value = m_session.property(name);
        if (!value.empty()) resolved = true;
        return value;
    }

    const Session& m_session;
    const Record* m_record;
    bool m_with_session;
};

} // namespace

std::string Session::format_impl(const std::string& text, const Record* record, bool with_session) const {
    Formatter f(*this, record, with_session);
    FormatState st;
    return f.expand(text, st);
}

std::string Session::format(const std::string& text) const {
    if (text.find('[') == std::string::npos && text.find('{') == std::string::npos) return text;
    return format_impl(text, nullptr, true);
}

namespace {
/// A record whose field 0 is null formats as "1: v1 2: v2 ... " (probe:
/// "fmt nulltemplate" -> "1: a 2: 7 3: c ").
std::string null_template_text(const Record& record) {
    std::string out;
    for (u32 i = 1; i <= record.count(); ++i) {
        bool present = false;
        out += std::to_string(i) + ": " + record_field_text(record, i, present) + " ";
    }
    return out;
}
}  // namespace

std::string Session::format_record(const Record& record) const {
    if (record.fields.empty() || record.fields[0].kind == Record::Kind::Null) return null_template_text(record);
    bool present = false;
    const std::string tmpl = record_field_text(record, 0, present);
    return format_impl(tmpl, &record, true);
}

std::string Session::format_record_without_session(const Record& record) {
    if (record.fields.empty() || record.fields[0].kind == Record::Kind::Null) return null_template_text(record);
    bool present = false;
    const std::string tmpl = record_field_text(record, 0, present);
    Session empty;
    return empty.format_impl(tmpl, &record, false);
}

// ---------------------------------------------------------------------------
// Conditions (Learn "Conditional Statement Syntax"; probe "cond" lines)
// ---------------------------------------------------------------------------

namespace {

enum class Tok { End, LParen, RParen, Not, And, Or, Xor, Eqv, Imp, Op, Symbol, String, Integer, Error };

struct Token {
    Tok kind{Tok::End};
    std::string text;   ///< operator text (with ~), symbol name, literal, digits
    char prefix{0};     ///< symbol access prefix: 0 % $ ? & !
};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.'; }

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    usize i = 0;
    auto fail = [&]() { out.assign(1, Token{Tok::Error, {}, 0}); return out; };
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) { ++i; continue; }
        if (c == '(') { out.push_back({Tok::LParen}); ++i; continue; }
        if (c == ')') { out.push_back({Tok::RParen}); ++i; continue; }
        if (c == '"') {
            const usize end = s.find('"', i + 1);
            if (end == std::string::npos) return fail();  // unterminated literal
            out.push_back({Tok::String, s.substr(i + 1, end - i - 1)});
            i = end + 1;
            continue;
        }
        if (c == '~' || c == '=' || c == '<' || c == '>') {
            std::string op;
            if (c == '~') { op.push_back('~'); ++i; }
            if (i >= s.size()) return fail();
            const char a = s[i];
            const char b = i + 1 < s.size() ? s[i + 1] : '\0';
            if (a == '=') { op += "="; i += 1; }
            else if (a == '<' && (b == '>' || b == '=' || b == '<')) { op += a; op += b; i += 2; }
            else if (a == '>' && (b == '=' || b == '<' || b == '>')) { op += a; op += b; i += 2; }
            else if (a == '<' || a == '>') { op += a; i += 1; }
            else return fail();
            out.push_back({Tok::Op, op});
            continue;
        }
        if (c == '-' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0) {
            usize j = i + 1;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) ++j;
            out.push_back({Tok::Integer, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            usize j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) != 0) ++j;
            if (j < s.size() && ident_char(s[j])) return fail();
            out.push_back({Tok::Integer, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '%' || c == '$' || c == '?' || c == '&' || c == '!') {
            usize j = i + 1;
            if (j >= s.size() || !(ident_start(s[j]) || (c == '%' && std::isdigit(static_cast<unsigned char>(s[j])) != 0))) return fail();
            while (j < s.size() && ident_char(s[j])) ++j;
            out.push_back({Tok::Symbol, s.substr(i + 1, j - i - 1), c});
            i = j;
            continue;
        }
        if (ident_start(c)) {
            usize j = i;
            while (j < s.size() && ident_char(s[j])) ++j;
            const std::string word = s.substr(i, j - i);
            Tok kind = Tok::Symbol;
            if (iequals(word, "NOT")) kind = Tok::Not;
            else if (iequals(word, "AND")) kind = Tok::And;
            else if (iequals(word, "OR")) kind = Tok::Or;
            else if (iequals(word, "XOR")) kind = Tok::Xor;
            else if (iequals(word, "EQV")) kind = Tok::Eqv;
            else if (iequals(word, "IMP")) kind = Tok::Imp;
            out.push_back({kind, word});
            i = j;
            continue;
        }
        return fail();
    }
    out.push_back({Tok::End});
    return out;
}

struct Value {
    enum class Kind { Integer, String, Symbol } kind{Kind::String};
    i64 integer{0};
    std::string text;
};

class Evaluator {
public:
    Evaluator(const std::vector<Token>& t, const Session& s) : m_t(t), m_s(s) {}

    std::optional<bool> run() {
        if (!m_t.empty() && m_t[0].kind == Tok::Error) return std::nullopt;
        auto v = imp();
        if (!v || peek().kind != Tok::End) return std::nullopt;
        return v;
    }

private:
    const Token& peek() const { return m_t[m_pos]; }
    void next() { if (m_t[m_pos].kind != Tok::End) ++m_pos; }

    // Learn: Not, And, Or, Xor, Eqv, Imp, from high to low precedence.
    std::optional<bool> imp() {
        auto l = eqv();
        while (l && peek().kind == Tok::Imp) { next(); auto r = eqv(); if (!r) return std::nullopt; l = !*l || *r; }
        return l;
    }
    std::optional<bool> eqv() {
        auto l = xor_();
        while (l && peek().kind == Tok::Eqv) { next(); auto r = xor_(); if (!r) return std::nullopt; l = *l == *r; }
        return l;
    }
    std::optional<bool> xor_() {
        auto l = or_();
        while (l && peek().kind == Tok::Xor) { next(); auto r = or_(); if (!r) return std::nullopt; l = *l != *r; }
        return l;
    }
    std::optional<bool> or_() {
        auto l = and_();
        while (l && peek().kind == Tok::Or) { next(); auto r = and_(); if (!r) return std::nullopt; l = *l || *r; }
        return l;
    }
    std::optional<bool> and_() {
        auto l = not_();
        while (l && peek().kind == Tok::And) { next(); auto r = not_(); if (!r) return std::nullopt; l = *l && *r; }
        return l;
    }
    std::optional<bool> not_() {
        if (peek().kind == Tok::Not) { next(); auto v = not_(); if (!v) return std::nullopt; return !*v; }
        return term();
    }

    std::optional<Value> value() {
        const Token& t = peek();
        Value v;
        if (t.kind == Tok::Integer) {
            auto n = strict_integer(t.text);
            if (!n) return std::nullopt;
            v.kind = Value::Kind::Integer;
            v.integer = *n;
        } else if (t.kind == Tok::String) {
            v.kind = Value::Kind::String;
            v.text = t.text;
        } else if (t.kind == Tok::Symbol) {
            v.kind = Value::Kind::Symbol;
            const ItemStateResolvers& st = m_s.item_states();
            std::optional<i32> state;
            bool is_state = true;
            switch (t.prefix) {
                case 0: v.text = m_s.property(t.text); is_state = false; break;
                case '%': v.text = m_s.environment(t.text).value_or(std::string{}); is_state = false; break;
                case '$': if (st.component_action) state = st.component_action(t.text); break;
                case '?': if (st.component_installed) state = st.component_installed(t.text); break;
                case '&': if (st.feature_action) state = st.feature_action(t.text); break;
                case '!': if (st.feature_installed) state = st.feature_installed(t.text); break;
                default: return std::nullopt;
            }
            // A state with no value yet is a null operand: probe "&F1 = -1"
            // false and "&NoFeat = \"\"" true before CostFinalize.
            if (is_state && state.has_value()) {
                v.kind = Value::Kind::Integer;
                v.integer = *state;
            }
        } else {
            return std::nullopt;
        }
        next();
        return v;
    }

    static bool truth(const Value& v) {
        // Probe: "0" -> false, "-1" -> true, B="0" -> true, "\"0\"" -> true.
        if (v.kind == Value::Kind::Integer) return v.integer != 0;
        return !v.text.empty();
    }

    std::optional<bool> term() {
        if (peek().kind == Tok::LParen) {
            next();
            auto v = imp();
            if (!v || peek().kind != Tok::RParen) return std::nullopt;
            next();
            return v;
        }
        auto l = value();
        if (!l) return std::nullopt;
        if (peek().kind != Tok::Op) return truth(*l);
        const std::string op = peek().text;
        next();
        auto r = value();
        if (!r) return std::nullopt;
        return compare(*l, op, *r);
    }

    static bool compare(const Value& a, std::string op, const Value& b) {
        bool fold = false;
        if (!op.empty() && op[0] == '~') { fold = true; op.erase(0, 1); }
        std::optional<i64> ai, bi;
        if (a.kind == Value::Kind::Integer) ai = a.integer;
        if (b.kind == Value::Kind::Integer) bi = b.integer;
        const bool a_int_literal = ai.has_value();
        const bool b_int_literal = bi.has_value();
        if (a_int_literal || b_int_literal) {
            // An integer against a value that is not one: false, except <>
            // (Learn). A property value that converts is an integer (probe:
            // "N = 10" true); a quoted literal never is.
            if (!ai && a.kind == Value::Kind::Symbol) ai = strict_integer(a.text);
            if (!bi && b.kind == Value::Kind::Symbol) bi = strict_integer(b.text);
            if (!ai || !bi) return op == "<>";
            return compare_int(*ai, op, *bi);
        }
        if (a.kind == Value::Kind::Symbol && b.kind == Value::Kind::Symbol) {
            ai = strict_integer(a.text);
            bi = strict_integer(b.text);
            if (ai && bi) return compare_int(*ai, op, *bi);  // probe: "N > N2" true
        }
        std::string l = a.text, r = b.text;
        if (fold) { l = to_lower(l); r = to_lower(r); }
        if (op == "><") return l.find(r) != std::string::npos;
        if (op == "<<") return l.rfind(r, 0) == 0;
        if (op == ">>") return r.size() <= l.size() && l.compare(l.size() - r.size(), r.size(), r) == 0;
        const int c = l.compare(r);
        if (op == "=") return c == 0;
        if (op == "<>") return c != 0;
        if (op == "<") return c < 0;
        if (op == ">") return c > 0;
        if (op == "<=") return c <= 0;
        if (op == ">=") return c >= 0;
        return false;
    }

    static bool compare_int(i64 a, const std::string& op, i64 b) {
        // Bitwise forms (Learn "Bitwise Numeric Operators"; probe Bits lines).
        if (op == "><") return (a & b) != 0;
        if (op == "<<") return ((static_cast<u64>(a) >> 16) & 0xFFFF) == static_cast<u64>(b);
        if (op == ">>") return (static_cast<u64>(a) & 0xFFFF) == static_cast<u64>(b);
        if (op == "=") return a == b;
        if (op == "<>") return a != b;
        if (op == "<") return a < b;
        if (op == ">") return a > b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        return false;
    }

    const std::vector<Token>& m_t;
    const Session& m_s;
    usize m_pos{0};
};

bool blank(const std::string& s) {
    for (const char c : s) if (std::isspace(static_cast<unsigned char>(c)) == 0) return false;
    return true;
}

} // namespace

std::optional<bool> Session::evaluate_condition(const std::string& expression) const {
    if (blank(expression)) return true;
    const auto tokens = tokenize(expression);
    Evaluator e(tokens, *this);
    return e.run();
}

ConditionResult Session::evaluate_condition_ex(const std::string& expression) const {
    if (blank(expression)) return ConditionResult::None;
    const auto r = evaluate_condition(expression);
    if (!r.has_value()) return ConditionResult::Error;
    return *r ? ConditionResult::True : ConditionResult::False;
}

// ---------------------------------------------------------------------------
// Directory resolution
// ---------------------------------------------------------------------------

void Session::resolve_directories() {
    m_directories.clear();
    m_directory_rows.clear();
    if (m_package == nullptr) return;

    const Table* dirs = m_package->table("Directory");
    if (dirs == nullptr) return;

    const auto key_col    = dirs->column_index("Directory");
    const auto parent_col = dirs->column_index("Directory_Parent");
    const auto def_col    = dirs->column_index("DefaultDir");
    if (!key_col.has_value() || !parent_col.has_value() || !def_col.has_value()) return;

    for (const auto& row : dirs->rows) {
        if (*key_col >= row.size()) continue;
        const std::string& key = row[*key_col];
        if (key.empty()) continue;
        m_directory_rows[key] = {
            (*parent_col < row.size()) ? row[*parent_col] : std::string{},
            (*def_col < row.size()) ? row[*def_col] : std::string{},
        };
    }
    for (const auto& [key, _] : m_directory_rows) (void)resolve_directory(key, 0);
}

std::string Session::resolve_directory(const std::string& key, int depth) {
    if (key.empty()) return std::string{};
    auto cached = m_directories.find(key);
    if (cached != m_directories.end()) return cached->second;
    if (depth > 64) return std::string{};

    auto row = m_directory_rows.find(key);
    if (row == m_directory_rows.end()) return property(key);

    const std::string& parent_key = row->second.first;
    const std::string& default_dir = row->second.second;

    std::string base;
    if (parent_key.empty() || parent_key == key) {
        base = has_property(key) ? property(key) : property("ROOTDRIVE");
        if (base.empty()) base = "C:\\";
        if (base.back() != '\\') base.push_back('\\');
        m_directories[key] = base;
        return base;
    }
    if (has_property(key)) {
        std::string overridden = property(key);
        if (overridden.back() != '\\') overridden.push_back('\\');
        m_directories[key] = overridden;
        return overridden;
    }
    base = resolve_directory(parent_key, depth + 1);
    const std::string segment = default_dir_target(default_dir);
    std::string full = base;
    if (!segment.empty()) {
        if (!full.empty() && full.back() != '\\') full.push_back('\\');
        full += segment;
    }
    if (!full.empty() && full.back() != '\\') full.push_back('\\');
    m_directories[key] = full;
    return full;
}

std::string Session::directory_path(const std::string& key) const {
    if (m_item_states.directory_path) {
        if (auto v = m_item_states.directory_path(key)) return *v;
    }
    auto it = m_directories.find(key);
    return (it == m_directories.end()) ? std::string{} : it->second;
}

} // namespace vitreo::winapi::msi
