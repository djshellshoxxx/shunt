// Golden text file helper (FS-TEST section 3). CHECK_GOLDEN("name.txt", actual) compares `actual`
// with tests/golden/name.txt. On mismatch it prints a unified diff and fails; UPDATE_GOLDEN=1 in the
// environment rewrites the file instead (the PR must show the golden diff and say why).
// SHUNT_GOLDEN_DIR overrides the directory. Original Shunt code.
#pragma once
#include "TestFramework.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tf {

inline std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == '\n') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Unified diff (3 lines of context) of two texts; empty string when they are equal.
inline std::string unifiedDiff(const std::string& want, const std::string& got,
                               const std::string& wantName = "golden", const std::string& gotName = "actual") {
    if (want == got) return "";
    const auto a = splitLines(want), b = splitLines(got);
    const size_t n = a.size(), m = b.size();
    // Trim the common prefix and suffix, then LCS the middle (bounded; falls back to replace-all).
    size_t pre = 0;
    while (pre < n && pre < m && a[pre] == b[pre]) ++pre;
    size_t suf = 0;
    while (suf < n - pre && suf < m - pre && a[n - 1 - suf] == b[m - 1 - suf]) ++suf;
    const size_t an = n - pre - suf, bn = m - pre - suf;
    struct Op { char c; size_t ai, bi; };   // ' ' keep, '-' delete a[ai], '+' insert b[bi]
    std::vector<Op> ops;
    for (size_t i = 0; i < pre; ++i) ops.push_back({' ', i, i});
    if (an * bn <= 4'000'000) {
        std::vector<std::vector<uint32_t>> L(an + 1, std::vector<uint32_t>(bn + 1, 0));
        for (size_t i = an; i-- > 0;)
            for (size_t j = bn; j-- > 0;)
                L[i][j] = a[pre + i] == b[pre + j] ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
        size_t i = 0, j = 0;
        while (i < an && j < bn) {
            if (a[pre + i] == b[pre + j]) { ops.push_back({' ', pre + i, pre + j}); ++i; ++j; }
            else if (L[i + 1][j] >= L[i][j + 1]) { ops.push_back({'-', pre + i, 0}); ++i; }
            else { ops.push_back({'+', 0, pre + j}); ++j; }
        }
        for (; i < an; ++i) ops.push_back({'-', pre + i, 0});
        for (; j < bn; ++j) ops.push_back({'+', 0, pre + j});
    } else {
        for (size_t i = 0; i < an; ++i) ops.push_back({'-', pre + i, 0});
        for (size_t j = 0; j < bn; ++j) ops.push_back({'+', 0, pre + j});
    }
    for (size_t k = 0; k < suf; ++k) ops.push_back({' ', n - suf + k, m - suf + k});

    std::ostringstream o;
    o << "--- " << wantName << "\n+++ " << gotName << "\n";
    constexpr size_t ctx = 3;
    size_t k = 0;
    while (k < ops.size()) {
        while (k < ops.size() && ops[k].c == ' ') ++k;
        if (k >= ops.size()) break;
        const size_t start = k >= ctx ? k - ctx : 0;
        size_t lastChange = k;
        for (size_t p = k; p < ops.size(); ++p) {
            if (ops[p].c != ' ') lastChange = p;
            if (p > lastChange + 2 * ctx) break;
        }
        const size_t end = std::min(ops.size(), lastChange + ctx + 1);
        size_t aCount = 0, bCount = 0, aStart = 0, bStart = 0;
        for (size_t p = 0; p < start; ++p) { if (ops[p].c != '+') ++aStart; if (ops[p].c != '-') ++bStart; }
        for (size_t p = start; p < end; ++p) { if (ops[p].c != '+') ++aCount; if (ops[p].c != '-') ++bCount; }
        o << "@@ -" << (aCount ? aStart + 1 : aStart) << "," << aCount << " +" << (bCount ? bStart + 1 : bStart) << "," << bCount << " @@\n";
        for (size_t p = start; p < end; ++p) {
            if (ops[p].c == '+') o << '+' << b[ops[p].bi] << "\n";
            else if (ops[p].c == '-') o << '-' << a[ops[p].ai] << "\n";
            else o << ' ' << a[ops[p].ai] << "\n";
        }
        k = end;
    }
    return o.str();
}

inline std::string goldenDir() {
    if (const char* d = std::getenv("SHUNT_GOLDEN_DIR")) if (*d) return d;
    std::string here = __FILE__;      // .../tests/Golden.h
    return here.substr(0, here.find_last_of('/')) + "/golden";
}

inline bool updateGoldenRequested() {
    const char* u = std::getenv("UPDATE_GOLDEN");
    return u && *u && std::string(u) != "0";
}

// Returns true when `actual` matches (or the golden was rewritten). `diffOut` receives the diff.
inline bool compareGolden(const std::string& dir, const std::string& name, const std::string& actual,
                          bool update, std::string* diffOut = nullptr) {
    const std::string path = dir + "/" + name;
    std::ifstream in(path, std::ios::binary);
    std::string want;
    const bool exists = bool(in);
    if (exists) { std::ostringstream ss; ss << in.rdbuf(); want = ss.str(); }
    if (update) {
        if (!exists || want != actual) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << actual;
            if (!out) { if (diffOut) *diffOut = "cannot write golden " + path + "\n"; return false; }
            std::printf("  golden updated: %s\n%s", path.c_str(), unifiedDiff(want, actual, path, "new").c_str());
        }
        return true;
    }
    if (!exists) { if (diffOut) *diffOut = "golden file missing: " + path + " (run with UPDATE_GOLDEN=1)\n"; return false; }
    if (want == actual) return true;
    if (diffOut) *diffOut = unifiedDiff(want, actual, path, "actual");
    return false;
}

inline void checkGolden(const char* file, int line, const std::string& name, const std::string& actual) {
    ++checks();
    std::string diff;
    if (!compareGolden(goldenDir(), name, actual, updateGoldenRequested(), &diff)) {
        std::printf("%s", diff.c_str());
        fail(file, line, "golden mismatch: " + name + " (UPDATE_GOLDEN=1 to rewrite)");
    }
}

} // namespace tf

#define CHECK_GOLDEN(name, actual) tf::checkGolden(__FILE__, __LINE__, (name), (actual))
