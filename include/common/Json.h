// Json.h - 极简 JSON 解析/序列化（保序对象，满足 config.json 稳定键序需求）
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : m_type(Type::Bool), m_bool(b) {}
    Json(int i) : m_type(Type::Int), m_int(i) {}
    Json(unsigned int i) : m_type(Type::Int), m_int(i) {}
    Json(int64_t i) : m_type(Type::Int), m_int(i) {}
    Json(uint64_t i) : m_type(Type::Int), m_int(static_cast<int64_t>(i)) {}
    Json(double d) : m_type(Type::Double), m_dbl(d) {}
    Json(const char* s) : m_type(Type::String), m_str(s ? s : "") {}
    Json(std::string s) : m_type(Type::String), m_str(std::move(s)) {}

    static Json array();
    static Json object();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isInt() const { return m_type == Type::Int; }
    bool isDouble() const { return m_type == Type::Double; }
    bool isNumber() const { return m_type == Type::Int || m_type == Type::Double; }
    bool isString() const { return m_type == Type::String; }
    bool isArray() const { return m_type == Type::Array; }
    bool isObject() const { return m_type == Type::Object; }

    bool asBool(bool def = false) const;
    int64_t asInt(int64_t def = 0) const;
    double asDouble(double def = 0) const;
    const std::string& asString() const { return m_type == Type::String ? m_str : m_empty; }

    // 数组
    size_t size() const;
    bool empty() const { return size() == 0; }
    Json& at(size_t i) { return m_arr[i]; }
    const Json& at(size_t i) const { return m_arr[i]; }
    const Json& operator[](size_t i) const { return m_arr[i]; }
    Json& operator[](size_t i) { return m_arr[i]; }
    void push_back(Json v) { ensureArray(); m_arr.push_back(std::move(v)); }

    // 对象
    const Json* find(const std::string& key) const;
    Json get(const std::string& key, Json def) const;
    Json& operator[](const std::string& key);           // 取或创建
    void set(const std::string& key, Json v);           // 覆盖或追加
    void erase(const std::string& key);
    const std::vector<Member>& members() const { return m_obj; }

    // 只把 other 中本对象没有的键并入（保留未知字段，防升级丢字段）
    void mergeMissing(const Json& other);

    std::string dump(bool pretty = false) const;
    static bool parse(const std::string& in, Json& out, std::string* err = nullptr);

private:
    void ensureArray();
    void ensureObject();
    void dumpTo(std::string& out, bool pretty, int depth) const;

    Type m_type = Type::Null;
    bool m_bool = false;
    int64_t m_int = 0;
    double m_dbl = 0;
    std::string m_str;
    std::vector<Json> m_arr;
    std::vector<Member> m_obj;
    static const std::string m_empty;
};
