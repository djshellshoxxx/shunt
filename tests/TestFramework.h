// Self-contained minimal test framework: TEST_CASE, CHECK, REQUIRE, CHECK_NEAR, plus
// CHECK_STR, TEMP_DIR(), WITH_TIMEOUT(ms, body) and CHECK_JSON(expr, path, value) (FS-TEST section 3).
// Original Shunt code.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#if __has_include("shunt/app/Json.h")
#include "shunt/app/Json.h"
#define TF_HAVE_JSON 1
#endif

namespace tf {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
struct RequireFailed {};
inline void fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::printf("  FAIL %s:%d: %s\n", file, line, msg.c_str());
}
inline int runAll(const char* filter) {
    int ran = 0;
    for (auto& c : cases()) {
        if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        try { c.fn(); } catch (RequireFailed&) {} catch (std::exception& e) { fail(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
        std::printf("[ %s ] %s\n", failures() == before ? " OK " : "FAIL", c.name);
        ++ran;
    }
    std::printf("%d test cases, %d checks, %d failures\n", ran, checks(), failures());
    return failures() == 0 ? 0 : 1;
}

// Temporary directory removed (recursively) when the object is destroyed.
class TempDir {
public:
    TempDir() {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/shunt-test-XXXXXX";
        if (!mkdtemp(tmpl.data())) { std::printf("  FAIL TEMP_DIR: mkdtemp failed\n"); std::abort(); }
        path_ = tmpl;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
    const std::string& path() const { return path_; }
    std::string file(const std::string& name) const { return path_ + "/" + name; }
    operator const std::string&() const { return path_; }
private:
    std::string path_;
};

// Exits the whole test process (code 124) when its scope outlives the budget, so a hung
// test fails CI quickly instead of waiting for the ctest timeout.
class Watchdog {
public:
    Watchdog(int ms, const char* file, int line) : th_([this, ms, file, line] {
        std::unique_lock<std::mutex> l(m_);
        if (!cv_.wait_for(l, std::chrono::milliseconds(ms), [this] { return done_; })) {
            std::printf("  FAIL %s:%d: WITH_TIMEOUT(%d ms) expired, test hung\n", file, line, ms);
            std::fflush(stdout);
            std::_Exit(124);
        }
    }) {}
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    ~Watchdog() { { std::lock_guard<std::mutex> l(m_); done_ = true; } cv_.notify_all(); th_.join(); }
private:
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread th_;
};

#ifdef TF_HAVE_JSON
// CHECK_JSON support. `expr` is a shunt::app::Json or a string holding JSON text; `path` is
// dot-separated keys with optional [index] (e.g. "features.onair.enabled", "devices[0].name").
inline shunt::app::Json asJson(const shunt::app::Json& j) { return j; }
inline shunt::app::Json asJson(const std::string& text) {
    shunt::app::Json j; std::string err;
    if (!shunt::app::Json::parse(text, j, &err)) return shunt::app::Json();
    return j;
}
inline shunt::app::Json asJson(const char* text) { return asJson(std::string(text)); }
// Returns nullptr when the path does not exist.
inline const shunt::app::Json* jsonAt(const shunt::app::Json& root, const std::string& path) {
    const shunt::app::Json* cur = &root;
    size_t i = 0;
    while (i < path.size() && cur) {
        if (path[i] == '.') { ++i; continue; }
        if (path[i] == '[') {
            const size_t close = path.find(']', i);
            if (close == std::string::npos || !cur->isArray()) return nullptr;
            const size_t idx = size_t(std::strtoul(path.substr(i + 1, close - i - 1).c_str(), nullptr, 10));
            if (idx >= cur->items().size()) return nullptr;
            cur = &cur->items()[idx];
            i = close + 1;
        } else {
            size_t j = i;
            while (j < path.size() && path[j] != '.' && path[j] != '[') ++j;
            const std::string key = path.substr(i, j - i);
            if (!cur->isObject() || !cur->has(key)) return nullptr;
            cur = &cur->get(key);
            i = j;
        }
    }
    return cur;
}
inline void checkJson(const char* file, int line, const char* exprText, const shunt::app::Json& doc,
                      const std::string& path, const shunt::app::Json& want) {
    ++checks();
    const shunt::app::Json* got = jsonAt(doc, path);
    if (!got) { fail(file, line, std::string(exprText) + ": path '" + path + "' not found in " + doc.dump()); return; }
    if (got->dump() != want.dump())
        fail(file, line, std::string(exprText) + " at '" + path + "': got " + got->dump() + ", want " + want.dump());
}
inline void checkJsonType(const char* file, int line, const char* exprText, const shunt::app::Json& doc,
                          const std::string& path, shunt::app::Json::Type want) {
    ++checks();
    const shunt::app::Json* got = jsonAt(doc, path);
    if (!got) { fail(file, line, std::string(exprText) + ": path '" + path + "' not found"); return; }
    if (got->type() != want) fail(file, line, std::string(exprText) + " at '" + path + "': wrong JSON type " + std::to_string(int(got->type())));
}
#endif
} // namespace tf

#define TF_CAT2(a, b) a##b
#define TF_CAT(a, b) TF_CAT2(a, b)
#define TEST_CASE(name) static void TF_CAT(tf_fn_, __LINE__)(); \
    static tf::Reg TF_CAT(tf_reg_, __LINE__)(name, TF_CAT(tf_fn_, __LINE__)); static void TF_CAT(tf_fn_, __LINE__)()
#define CHECK(expr) do { ++tf::checks(); if (!(expr)) tf::fail(__FILE__, __LINE__, #expr); } while (0)
#define REQUIRE(expr) do { ++tf::checks(); if (!(expr)) { tf::fail(__FILE__, __LINE__, #expr); throw tf::RequireFailed{}; } } while (0)
#define CHECK_EQ(a, b) do { ++tf::checks(); auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) \
    tf::fail(__FILE__, __LINE__, std::string(#a " == " #b " (") + std::to_string(va_) + " vs " + std::to_string(vb_) + ")"); } while (0)
#define CHECK_NEAR(a, b, tol) do { ++tf::checks(); double va_ = double(a), vb_ = double(b); if (std::fabs(va_ - vb_) > (tol)) \
    tf::fail(__FILE__, __LINE__, std::string(#a " ~= " #b " (") + std::to_string(va_) + " vs " + std::to_string(vb_) + ", tol " + std::to_string(double(tol)) + ")"); } while (0)
#define INFO(...) std::printf("  info: " __VA_ARGS__)
#define CHECK_STR(a, b) do { ++tf::checks(); const std::string sa_ = (a), sb_ = (b); if (sa_ != sb_) \
    tf::fail(__FILE__, __LINE__, std::string(#a " == " #b " (\"") + sa_ + "\" vs \"" + sb_ + "\")"); } while (0)
// Usage: auto dir = TEMP_DIR(); dir.path(), dir.file("x.json"). Removed when `dir` goes out of scope.
#define TEMP_DIR() tf::TempDir()
// Usage: WITH_TIMEOUT(2000, { ...body... });  The process exits with code 124 if the body takes longer.
#define WITH_TIMEOUT(ms, ...) do { tf::Watchdog tf_wd_((ms), __FILE__, __LINE__); __VA_ARGS__ } while (0)
#ifdef TF_HAVE_JSON
// CHECK_JSON(jsonOrText, "a.b[0].c", value): value is anything Json is constructible from (bool, int,
// double, string, const char*; cast unsigned values to int or double); compares type and value.
// CHECK_JSON_TYPE checks only the JSON type.
#define CHECK_JSON(expr, path, value) tf::checkJson(__FILE__, __LINE__, #expr, tf::asJson(expr), (path), shunt::app::Json(value))
#define CHECK_JSON_TYPE(expr, path, type) tf::checkJsonType(__FILE__, __LINE__, #expr, tf::asJson(expr), (path), (type))
#endif
