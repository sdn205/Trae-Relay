// Json.cpp
#include "common/Json.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

const std::string Json::m_empty;

Json Json::array() { Json v; v.m_type = Type::Array; return v; }
Json Json::object() { Json v; v.m_type = Type::Object; return v; }

bool Json::asBool(bool def) const {
    if (m_type == Type::Bool) return m_bool;
    if (m_type == Type::Int) return m_int != 0;
    if (m_type == Type::String) return m_str == "true" || m_str == "1";
    return def;
}
int64_t Json::asInt(int64_t def) const {
    if (m_type == Type::Int) return m_int;
    if (m_type == Type::Double) return static_cast<int64_t>(m_dbl);
    if (m_type == Type::Bool) return m_bool ? 1 : 0;
    if (m_type == Type::String) {
        char* end = nullptr;
        long long v = _strtoi64(m_str.c_str(), &end, 10);
        if (end && end != m_str.c_str() && *end == '\0') return v;
    }
    return def;
}
double Json::asDouble(double def) const {
    if (m_type == Type::Double) return m_dbl;
    if (m_type == Type::Int) return static_cast<double>(m_int);
    return def;
}

size_t Json::size() const {
    if (m_type == Type::Array) return m_arr.size();
    if (m_type == Type::Object) return m_obj.size();
    return 0;
}
void Json::ensureArray() {
    if (m_type != Type::Array) { m_type = Type::Array; m_arr.clear(); m_obj.clear(); m_str.clear(); }
}
void Json::ensureObject() {
    if (m_type != Type::Object) { m_type = Type::Object; m_obj.clear(); m_arr.clear(); m_str.clear(); }
}
const Json* Json::find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    for (const auto& m : m_obj)
        if (m.first == key) return &m.second;
    return nullptr;
}
Json Json::get(const std::string& key, Json def) const {
    const Json* p = find(key);
    return p ? *p : def;
}
Json& Json::operator[](const std::string& key) {
    ensureObject();
    for (auto& m : m_obj)
        if (m.first == key) return m.second;
    m_obj.emplace_back(key, Json());
    return m_obj.back().second;
}
void Json::set(const std::string& key, Json v) {
    ensureObject();
    for (auto& m : m_obj)
        if (m.first == key) { m.second = std::move(v); return; }
    m_obj.emplace_back(key, std::move(v));
}
void Json::erase(const std::string& key) {
    if (m_type != Type::Object) return;
    for (size_t i = 0; i < m_obj.size(); ++i)
        if (m_obj[i].first == key) { m_obj.erase(m_obj.begin() + i); return; }
}
void Json::mergeMissing(const Json& other) {
    if (m_type != Type::Object || other.m_type != Type::Object) return;
    for (const auto& m : other.m_obj) {
        Json* mine = nullptr;
        for (auto& mm : m_obj)
            if (mm.first == m.first) { mine = &mm.second; break; }
        if (!mine) {
            m_obj.emplace_back(m.first, m.second);
        } else if (mine->isObject() && m.second.isObject()) {
            mine->mergeMissing(m.second);
        }
    }
}

static void dumpString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

static bool nearlyIntegral(double d, int64_t& out) {
    if (!std::isfinite(d)) return false;
    double r = std::nearbyint(d);
    if (std::fabs(d - r) < 1e-9 && std::fabs(r) < 9.0e15) {
        out = static_cast<int64_t>(r);
        return true;
    }
    return false;
}

void Json::dumpTo(std::string& out, bool pretty, int depth) const {
    auto nl = [&](int d) {
        if (pretty) {
            out += '\n';
            out.append(static_cast<size_t>(d) * 2, ' ');
        }
    };
    switch (m_type) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += m_bool ? "true" : "false"; break;
    case Type::Int: {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_int));
        out += buf;
        break;
    }
    case Type::Double: {
        int64_t i;
        if (nearlyIntegral(m_dbl, i)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(i));
            out += buf;
        } else {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.17g", m_dbl);
            out += buf;
        }
        break;
    }
    case Type::String: dumpString(m_str, out); break;
    case Type::Array: {
        if (m_arr.empty()) { out += "[]"; break; }
        out += '[';
        for (size_t i = 0; i < m_arr.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            m_arr[i].dumpTo(out, pretty, depth + 1);
        }
        nl(depth);
        out += ']';
        break;
    }
    case Type::Object: {
        if (m_obj.empty()) { out += "{}"; break; }
        out += '{';
        for (size_t i = 0; i < m_obj.size(); ++i) {
            if (i) out += ',';
            nl(depth + 1);
            dumpString(m_obj[i].first, out);
            out += pretty ? ": " : ":";
            m_obj[i].second.dumpTo(out, pretty, depth + 1);
        }
        nl(depth);
        out += '}';
        break;
    }
    }
}

std::string Json::dump(bool pretty) const {
    std::string out;
    out.reserve(256);
    dumpTo(out, pretty, 0);
    return out;
}

// ---------- 解析 ----------
namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;
    std::string err;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }

    bool parseValue(Json& out) {
        if (++depth > 128) return fail("nesting too deep");
        skipWs();
        if (p >= end) return fail("unexpected end");
        bool ok;
        switch (*p) {
        case '{': ok = parseObject(out); break;
        case '[': ok = parseArray(out); break;
        case '"': {
            std::string s;
            ok = parseString(s);
            if (ok) out = Json(std::move(s));
            break;
        }
        case 't':
            ok = expect("true");
            if (ok) out = Json(true);
            break;
        case 'f':
            ok = expect("false");
            if (ok) out = Json(false);
            break;
        case 'n':
            ok = expect("null");
            if (ok) out = Json(nullptr);
            break;
        default: ok = parseNumber(out); break;
        }
        --depth;
        return ok;
    }
    bool expect(const char* word) {
        size_t n = strlen(word);
        if (static_cast<size_t>(end - p) < n || strncmp(p, word, n) != 0) return fail("invalid literal");
        p += n;
        return true;
    }
    bool parseNumber(Json& out) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool isDouble = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) {
            if (*p == '.' || *p == 'e' || *p == 'E') isDouble = true;
            ++p;
        }
        if (p == start) return fail("invalid number");
        std::string num(start, p);
        if (isDouble) {
            out = Json(strtod(num.c_str(), nullptr));
        } else {
            out = Json(static_cast<int64_t>(_strtoi64(num.c_str(), nullptr, 10)));
        }
        return true;
    }
    void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool parseHex4(unsigned& u) {
        if (end - p < 4) return fail("bad \\u");
        u = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            u <<= 4;
            if (c >= '0' && c <= '9') u |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') u |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') u |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u");
        }
        return true;
    }
    bool parseString(std::string& s) {
        if (*p != '"') return fail("expected string");
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    unsigned u;
                    if (!parseHex4(u)) return false;
                    if (u >= 0xD800 && u <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        unsigned lo;
                        if (!parseHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            appendUtf8(s, u), u = lo; // 非法代理对，两个都输出
                    }
                    appendUtf8(s, u);
                    break;
                }
                default: return fail("bad escape");
                }
            } else {
                s += c;
            }
        }
        return fail("unterminated string");
    }
    bool parseArray(Json& out) {
        out = Json::array();
        ++p; // [
        skipWs();
        if (p < end && *p == ']') { ++p; return true; }
        while (true) {
            Json v;
            if (!parseValue(v)) return false;
            out.push_back(std::move(v));
            skipWs();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return fail("bad array");
        }
    }
    bool parseObject(Json& out) {
        out = Json::object();
        ++p; // {
        skipWs();
        if (p < end && *p == '}') { ++p; return true; }
        while (true) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            Json v;
            if (!parseValue(v)) return false;
            out.set(std::move(key), std::move(v));
            skipWs();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail("bad object");
        }
    }
};

} // namespace

bool Json::parse(const std::string& in, Json& out, std::string* err) {
    // 跳过 UTF-8 BOM
    const char* b = in.data();
    size_t n = in.size();
    if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) {
        b += 3;
        n -= 3;
    }
    Parser ps{ b, b + n };
    if (!ps.parseValue(out)) {
        if (err) *err = ps.err;
        return false;
    }
    ps.skipWs();
    if (ps.p != ps.end) {
        if (err) *err = "trailing characters";
        return false;
    }
    return true;
}
