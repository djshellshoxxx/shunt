// Self-contained minimal test framework: TEST_CASE, CHECK, REQUIRE, CHECK_NEAR.
#pragma once
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

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
