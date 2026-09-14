// -----------------------------------------------------------------------------
// mini_gtest -- a minimal stand-in for the subset of the GoogleTest API used by
// this repository's tests.
//
// *** THIS IS NOT GOOGLETEST. ***
//
// Why it exists: master spec section 52 makes "tests pass" a phase-gate condition,
// and section 49 forbids claiming a test passed when it was not run. GoogleTest is
// provisioned by FetchContent from codeload.github.com; in an environment where
// that host is unreachable the suite would be unbuildable and therefore unrunnable.
// This shim makes it runnable there.
//
// What it is NOT:
//   * Not a conformance implementation. It supports TEST, EXPECT_*/ASSERT_* for the
//     comparison, boolean and floating-point predicates this repo uses, ADD_FAILURE,
//     message streaming and --gtest_filter=<substring>. Nothing else.
//   * No TEST_F/TEST_P fixtures, no death tests, no XML output, no sharding, no
//     gmock, no --gtest_repeat.
//
// The build prefers real GoogleTest. This header is on the include path only when
// CMake is configured with -DUSN_TEST_FRAMEWORK=mini, which also prints a warning at
// configure time and makes every test binary print the banner below, so a run
// against the shim can never be mistaken for a run against GoogleTest.
//
// Implementation notes:
//   * Comparisons go through a common_type cast, so the mixed signed/unsigned
//     comparisons tests naturally contain do not trip -Wsign-compare (an error here).
//   * Each operand is evaluated exactly once, because tests pass expressions with
//     side effects (a Status accessor, a container size).
//   * ASSERT_* uses the `return Helper(...) = Message()` shape GoogleTest uses:
//     operator= returns void, so `ASSERT_TRUE(x) << "why";` both records the message
//     and returns from the test body.
// -----------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mini_gtest {

// --- value printing ---------------------------------------------------------

template <typename T, typename = void>
struct IsStreamable : std::false_type {};
template <typename T>
struct IsStreamable<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string printValue(const T& value) {
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        // enum class has no operator<<; the underlying value is more useful in a
        // failure message than nothing at all.
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (IsStreamable<T>::value) {
        std::ostringstream ss;
        ss << value;
        return ss.str();
    } else {
        return "<value of non-printable type>";
    }
}

// Contextual conversion to bool without a redundant cast: tests pass plain bools,
// StatusOr (explicit operator bool) and pointers, and -Wuseless-cast is an error
// in this build.
template <typename T>
constexpr bool truthy(const T& value) {
    if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        return value;
    } else {
        return static_cast<bool>(value);
    }
}

// --- comparison -------------------------------------------------------------

struct OpEq { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a == b; } };
struct OpNe { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a != b; } };
struct OpLt { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a < b; } };
struct OpLe { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a <= b; } };
struct OpGt { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a > b; } };
struct OpGe { template <typename A, typename B> bool operator()(const A& a, const B& b) const { return a >= b; } };

template <typename A, typename B, typename Op>
bool compareValues(const A& a, const B& b, Op op) {
    if constexpr (std::is_arithmetic_v<std::decay_t<A>> &&
                  std::is_arithmetic_v<std::decay_t<B>>) {
        using Common = std::common_type_t<std::decay_t<A>, std::decay_t<B>>;
        return op(static_cast<Common>(a), static_cast<Common>(b));
    } else {
        return op(a, b);
    }
}

// --- message ----------------------------------------------------------------

class Message {
public:
    template <typename T>
    Message& operator<<(const T& value) {
        m_text += printValue(value);
        return *this;
    }
    // Streaming a manipulator (std::endl, std::hex) must not break compilation.
    Message& operator<<(std::ostream& (*manipulator)(std::ostream&)) {
        (void)manipulator;
        return *this;
    }
    [[nodiscard]] const std::string& str() const noexcept { return m_text; }

private:
    std::string m_text;
};

// --- failures ---------------------------------------------------------------

struct Failure {
    std::string file;
    int line{0};
    std::string text;
};

class Registry;   // defined below; AssertHelper::operator= is defined after it

// Records one failure. operator= returns void so ASSERT_* can be written as
// `return AssertHelper(...) = Message() << "context";`.
class AssertHelper {
public:
    AssertHelper(const char* file, int line, std::string text)
        : m_file(file), m_line(line), m_text(std::move(text)) {}
    void operator=(const Message& message) const;

private:
    const char* m_file;
    int m_line;
    std::string m_text;
};

// --- registry ---------------------------------------------------------------

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

class Registry {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    void add(std::string suite, std::string name, std::function<void()> body) {
        m_cases.push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
    }
    void pushFailure(Failure failure) { m_currentFailures.push_back(std::move(failure)); }
    void clearFailures() { m_currentFailures.clear(); }
    [[nodiscard]] bool hasFailures() const noexcept { return !m_currentFailures.empty(); }
    [[nodiscard]] std::size_t failureCount() const noexcept { return m_currentFailures.size(); }
    [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return m_cases; }

    int runAll(std::string_view filter);

private:
    std::vector<TestCase> m_cases;
    std::vector<Failure> m_currentFailures;
};

inline int Registry::runAll(std::string_view const filter) {
    std::size_t ran = 0;
    std::size_t failed = 0;
    std::size_t filtered = 0;
    std::string currentSuite;
    std::cout << "[==========] mini_gtest (NOT GoogleTest) -- see the header banner" << std::endl;
    for (const auto& testCase : m_cases) {
        std::string const full = testCase.suite + "." + testCase.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) {
            ++filtered;
            continue;
        }
        if (testCase.suite != currentSuite) {
            currentSuite = testCase.suite;
            std::cout << "[----------] " << currentSuite << std::endl;
        }
        clearFailures();
        std::cout << "[ RUN      ] " << full << std::endl;
        try {
            testCase.body();
        } catch (const std::exception& e) {
            // Printed as well as recorded: a test that dies on an exception must
            // say so in the log, otherwise "[ FAILED ]" is the only clue.
            std::cerr << "<exception>: Failure\nunexpected exception: " << e.what()
                      << std::endl;
            pushFailure(Failure{"<exception>", 0,
                                std::string("unexpected exception: ") + e.what()});
        } catch (...) {
            std::cerr << "<exception>: Failure\nunexpected non-standard exception" << std::endl;
            pushFailure(Failure{"<exception>", 0, "unexpected non-standard exception"});
        }
        ++ran;
        if (hasFailures()) {
            ++failed;
            std::cout << "[  FAILED  ] " << full << "  (" << failureCount() << " failure(s))"
                      << std::endl;
        } else {
            std::cout << "[       OK ] " << full << std::endl;
        }
    }
    std::cout << "[==========] " << ran << " test(s) ran, " << failed << " failed.";
    if (filtered != 0) {
        std::cout << " " << filtered << " filtered out.";
    }
    std::cout << std::endl;
    return failed == 0 ? 0 : 1;
}

inline void AssertHelper::operator=(const Message& message) const {
    std::string full = m_text;
    if (!message.str().empty()) {
        full += "\n   Additional: " + message.str();
    }
    std::cerr << m_file << ":" << m_line << ": Failure\n" << full << std::endl;
    Registry::instance().pushFailure(Failure{m_file, m_line, std::move(full)});
}

// --- registration -----------------------------------------------------------

class TestRegistrar {
public:
    TestRegistrar(const char* suite, const char* name, void (*body)()) {
        Registry::instance().add(suite, name, body);
    }
};

// --- check construction -----------------------------------------------------

// Holds the outcome of one comparison plus everything needed to report it, so the
// macros can support `<< extra context` after the fact.
class Check {
public:
    Check(bool passed, const char* file, int line, std::string text)
        : m_passed(passed), m_file(file), m_line(line), m_text(std::move(text)) {}

    [[nodiscard]] bool failed() const noexcept { return !m_passed; }
    [[nodiscard]] AssertHelper helper() const { return AssertHelper(m_file, m_line, m_text); }

private:
    bool m_passed;
    const char* m_file;
    int m_line;
    std::string m_text;
};

template <typename A, typename B, typename Op>
Check makeComparison(const char* file, int line, const char* exprA, const char* exprB,
                     const A& a, const B& b, Op op, const char* opName) {
    bool passed = false;
    try {
        passed = compareValues(a, b, op);
    } catch (...) {
        passed = false;   // a throwing comparison is a failure, not a crash
    }
    std::string text = passed
        ? std::string()
        : std::string("Expected: (") + exprA + ") " + opName + " (" + exprB +
              ")\n     Actual: " + printValue(a) + " vs " + printValue(b);
    return Check(passed, file, line, std::move(text));
}

inline Check makeBool(const char* file, int line, const char* expr, bool value, bool expected) {
    std::string text = value == expected
        ? std::string()
        : std::string("Value of: ") + expr + "\n     Actual: " + (value ? "true" : "false") +
              "\n   Expected: " + (expected ? "true" : "false");
    return Check(value == expected, file, line, std::move(text));
}

template <typename A, typename B, typename E>
Check makeNear(const char* file, int line, const char* exprA, const char* exprB, const A& a,
               const B& b, const E& epsilon) {
    auto const difference = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    bool const passed = difference <= static_cast<double>(epsilon);
    std::string text = passed
        ? std::string()
        : std::string("The difference between ") + exprA + " and " + exprB + " is " +
              printValue(difference) + ", which exceeds " + printValue(epsilon);
    return Check(passed, file, line, std::move(text));
}

// Set from argv by the generated main() before RUN_ALL_TESTS().
inline std::string& activeFilter() {
    static std::string filter;
    return filter;
}

}  // namespace mini_gtest

// --- public macros ----------------------------------------------------------

#define MINI_GTEST_JOIN_(a, b) a##b
#define MINI_GTEST_JOIN(a, b) MINI_GTEST_JOIN_(a, b)

#define TEST(suite, name)                                                          \
    static void MINI_GTEST_JOIN(mini_body_, MINI_GTEST_JOIN(suite, name))();       \
    static const ::mini_gtest::TestRegistrar MINI_GTEST_JOIN(                      \
        mini_reg_, MINI_GTEST_JOIN(suite, name))(                                  \
        #suite, #name, &MINI_GTEST_JOIN(mini_body_, MINI_GTEST_JOIN(suite, name)));\
    static void MINI_GTEST_JOIN(mini_body_, MINI_GTEST_JOIN(suite, name))()

// Non-fatal: the helper is assigned only when the check failed, so a passing check
// records nothing and the trailing `<< context` is harmless.
#define MINI_GTEST_REPORT(check_expr)                                              \
    if (auto mini_check_ = (check_expr); mini_check_.failed())                     \
    mini_check_.helper() = ::mini_gtest::Message()

#define MINI_GTEST_REPORT_FATAL(check_expr)                                        \
    if (auto mini_check_ = (check_expr); mini_check_.failed())                     \
    return mini_check_.helper() = ::mini_gtest::Message()

#define MINI_GTEST_CMP(kind, a, b, Op, opName)                                     \
    ::mini_gtest::makeComparison(__FILE__, __LINE__, #a, #b, (a), (b),             \
                                 ::mini_gtest::Op{}, opName)

#define EXPECT_EQ(a, b) MINI_GTEST_REPORT(MINI_GTEST_CMP(eq, a, b, OpEq, "=="))
#define EXPECT_NE(a, b) MINI_GTEST_REPORT(MINI_GTEST_CMP(ne, a, b, OpNe, "!="))
#define EXPECT_LT(a, b) MINI_GTEST_REPORT(MINI_GTEST_CMP(lt, a, b, OpLt, "<"))
#define EXPECT_LE(a, b) MINI_GTEST_REPORT(MINI_GTEST_CMP(le, a, b, OpLe, "<="))
#define EXPECT_GT(a, b) MINI_GTEST_REPORT(MINI_GTEST_CMP(gt, a, b, OpGt, ">"))
#define EXPECT_GE(a, b) MINI_GTEST_REPORT(MINI_GTEST_CMP(ge, a, b, OpGe, ">="))

#define ASSERT_EQ(a, b) MINI_GTEST_REPORT_FATAL(MINI_GTEST_CMP(eq, a, b, OpEq, "=="))
#define ASSERT_NE(a, b) MINI_GTEST_REPORT_FATAL(MINI_GTEST_CMP(ne, a, b, OpNe, "!="))
#define ASSERT_LT(a, b) MINI_GTEST_REPORT_FATAL(MINI_GTEST_CMP(lt, a, b, OpLt, "<"))
#define ASSERT_LE(a, b) MINI_GTEST_REPORT_FATAL(MINI_GTEST_CMP(le, a, b, OpLe, "<="))
#define ASSERT_GT(a, b) MINI_GTEST_REPORT_FATAL(MINI_GTEST_CMP(gt, a, b, OpGt, ">"))
#define ASSERT_GE(a, b) MINI_GTEST_REPORT_FATAL(MINI_GTEST_CMP(ge, a, b, OpGe, ">="))

#define MINI_GTEST_BOOL(x, expected)                                               \
    ::mini_gtest::makeBool(__FILE__, __LINE__, #x, ::mini_gtest::truthy(x), expected)

#define EXPECT_TRUE(x)  MINI_GTEST_REPORT(MINI_GTEST_BOOL(x, true))
#define EXPECT_FALSE(x) MINI_GTEST_REPORT(MINI_GTEST_BOOL(x, false))
#define ASSERT_TRUE(x)  MINI_GTEST_REPORT_FATAL(MINI_GTEST_BOOL(x, true))
#define ASSERT_FALSE(x) MINI_GTEST_REPORT_FATAL(MINI_GTEST_BOOL(x, false))

#define MINI_GTEST_NEAR(a, b, eps)                                                 \
    ::mini_gtest::makeNear(__FILE__, __LINE__, #a, #b, (a), (b), (eps))

#define EXPECT_NEAR(a, b, eps) MINI_GTEST_REPORT(MINI_GTEST_NEAR(a, b, eps))
#define ASSERT_NEAR(a, b, eps) MINI_GTEST_REPORT_FATAL(MINI_GTEST_NEAR(a, b, eps))

#define ADD_FAILURE()                                                              \
    ::mini_gtest::AssertHelper(__FILE__, __LINE__, "Failed") = ::mini_gtest::Message()

#define FAIL()                                                                     \
    return ::mini_gtest::AssertHelper(__FILE__, __LINE__, "Failed") =               \
        ::mini_gtest::Message()

#define SUCCEED() static_cast<void>(0)

// --- GoogleTest-compatible entry points --------------------------------------

namespace testing {

inline void InitGoogleTest(int* argc, char** argv) {
    if (argc == nullptr || argv == nullptr) {
        return;
    }
    constexpr std::string_view kPrefix = "--gtest_filter=";
    for (int i = 1; i < *argc; ++i) {
        std::string_view const argument = argv[i];
        if (argument.substr(0, kPrefix.size()) == kPrefix) {
            mini_gtest::activeFilter() = std::string(argument.substr(kPrefix.size()));
        }
    }
}

}  // namespace testing

#define RUN_ALL_TESTS() ::mini_gtest::Registry::instance().runAll(::mini_gtest::activeFilter())
