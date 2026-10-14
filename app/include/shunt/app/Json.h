// Minimal JSON value, parser and writer (objects keep insertion order so unknown keys survive a save).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace shunt::app {

class Json {
public:
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object };
    Json() = default;
    Json(bool b) : type_(Bool), b_(b) {}
    Json(int v) : type_(Number), n_(v) {}
    Json(int64_t v) : type_(Number), n_(double(v)) {}
    Json(uint64_t v) : type_(Number), n_(double(v)) {}
    Json(double v) : type_(Number), n_(v) {}
    Json(const char* s) : type_(String), s_(s) {}
    Json(std::string s) : type_(String), s_(std::move(s)) {}
    static Json array() { Json j; j.type_ = Array; return j; }
    static Json object() { Json j; j.type_ = Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isObject() const { return type_ == Object; }
    bool isArray() const { return type_ == Array; }
    bool asBool(bool def = false) const { return type_ == Bool ? b_ : def; }
    double asNumber(double def = 0) const { return type_ == Number ? n_ : def; }
    int asInt(int def = 0) const { return type_ == Number ? int(n_) : def; }
    const std::string& asString() const { return s_; }
    std::string asString(const std::string& def) const { return type_ == String ? s_ : def; }

    // Object access: operator[] inserts; get() returns Null when absent.
    Json& operator[](const std::string& key);
    const Json& get(const std::string& key) const;
    bool has(const std::string& key) const;
    void erase(const std::string& key);
    const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }
    // Array access.
    void push(Json v) { if (type_ != Array) { *this = array(); } arr_.push_back(std::move(v)); }
    const std::vector<Json>& items() const { return arr_; }
    size_t size() const { return type_ == Array ? arr_.size() : obj_.size(); }

    std::string dump() const;
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

private:
    void write(std::string& o) const;
    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

std::string jsonEscape(const std::string& s);

} // namespace shunt::app
