#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> body;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Failure {
    std::string message;
};

struct Register {
    Register(const char* name, std::function<void()> body) { Registry().push_back({name, std::move(body)}); }
};

}

#define TEST_JOIN2(a, b) a##b
#define TEST_JOIN(a, b) TEST_JOIN2(a, b)
#define TEST(name) \
    static void name(); \
    static const test::Register TEST_JOIN(register_, name)(#name, name); \
    static void name()

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::ostringstream stream; \
            stream << __FILE__ << ':' << __LINE__ << ": CHECK(" #condition ") failed"; \
            throw test::Failure{stream.str()}; \
        } \
    } while (false)

#define CHECK_EQ(left, right) \
    do { \
        const auto& checkLeft = (left); \
        const auto& checkRight = (right); \
        if (!(checkLeft == checkRight)) { \
            std::ostringstream stream; \
            stream << __FILE__ << ':' << __LINE__ << ": CHECK_EQ(" #left ", " #right ") failed: \"" << checkLeft << "\" != \"" << checkRight << '"'; \
            throw test::Failure{stream.str()}; \
        } \
    } while (false)

#define CHECK_THROWS(expression) \
    do { \
        bool checkThrew = false; \
        try { \
            (void)(expression); \
        } catch (const std::exception&) { \
            checkThrew = true; \
        } \
        if (!checkThrew) { \
            std::ostringstream stream; \
            stream << __FILE__ << ':' << __LINE__ << ": CHECK_THROWS(" #expression ") did not throw"; \
            throw test::Failure{stream.str()}; \
        } \
    } while (false)
