// A tiny test harness: TEST(name) { CHECK(...); } and RUN_TESTS() in main.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "%s:%d: FAILED %s\n", file, line, what.c_str());
}

template <typename A, typename B>
std::string show(const A& a, const B& b) {
    std::ostringstream s;
    s << a << " != " << b;
    return s.str();
}

inline int run_all(const char* filter) {
    int run = 0;
    for (const auto& t : registry()) {
        if (filter && std::string(t.name).find(filter) == std::string::npos) continue;
        int before = failures();
        try {
            t.body();
        } catch (const std::exception& e) {
            fail(t.name, 0, std::string("exception: ") + e.what());
        }
        std::printf("%s %s\n", failures() == before ? "ok  " : "FAIL", t.name);
        ++run;
    }
    std::printf("%d tests, %d failed checks\n", run, failures());
    return failures() ? 1 : 0;
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                          \
    static void CHECK_CAT(test_, name)();                                   \
    static check::Register CHECK_CAT(reg_, name)(#name, CHECK_CAT(test_, name)); \
    static void CHECK_CAT(test_, name)()
#define CHECK(cond) \
    do {            \
        if (!(cond)) check::fail(__FILE__, __LINE__, #cond); \
    } while (0)
#define CHECK_EQ(a, b) \
    do {               \
        auto va_ = (a); \
        auto vb_ = (b); \
        if (!(va_ == vb_)) check::fail(__FILE__, __LINE__, #a " == " #b ": " + check::show(va_, vb_)); \
    } while (0)
#define CHECK_NEAR(a, b, tol) \
    do {                      \
        double va_ = (a), vb_ = (b); \
        if (!(std::fabs(va_ - vb_) <= (tol))) check::fail(__FILE__, __LINE__, #a " ~ " #b ": " + check::show(va_, vb_)); \
    } while (0)
#define RUN_TESTS(argc, argv) check::run_all((argc) > 1 ? (argv)[1] : nullptr)
