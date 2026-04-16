#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "stdexcept"

// Simple unit test framework
class TestRunner 
{
public:
    struct TestCase {
        std::string suite;
        std::string name;
        std::string input;
        std::string expected;
        std::string actual;
        bool passed;
    };

    static TestRunner& Instance()
    {
        static TestRunner instance;
        return instance;
    }

    void AddTest(const std::string& suite, const std::string& name,
                 std::function<void()> test_fn)
    {
        try {
            test_fn();
            results_.push_back({suite, name, "", "", "", true});
        } catch (const std::exception& e) {
            results_.push_back({suite, name, "", "", e.what(), false});
        }
    }

    template<typename T, typename U>
    static void AssertEq(const T& actual, const U& expected, const std::string& msg = "")
    {
        if (actual != expected) {
            std::ostringstream oss;
            oss << "Assertion failed";
            if (!msg.empty()) oss << " (" << msg << ")";
            oss << "\n  Expected: " << expected;
            oss << "\n  Actual:   " << actual;
            throw std::runtime_error(oss.str());
        }
    }

    static void AssertTrue(bool condition, const std::string& msg = "")
    {
        if (!condition) {
            throw std::runtime_error(std::string("Assertion failed: ") + (msg.empty() ? "expected true" : msg));
        }
    }

    static void AssertFalse(bool condition, const std::string& msg = "")
    {
        if (condition) {
            throw std::runtime_error(std::string("Assertion failed: ") + (msg.empty() ? "expected false" : msg));
        }
    }

    static void AssertContains(const std::string& haystack, const std::string& needle, const std::string& msg = "")
    {
        if (haystack.find(needle) == std::string::npos) {
            std::ostringstream oss;
            oss << "Assertion failed: '" << needle << "' not found in result";
            if (!msg.empty()) oss << " (" << msg << ")";
            oss << "\n  Haystack (first 200): " << haystack.substr(0, 200);
            throw std::runtime_error(oss.str());
        }
    }

    static void AssertStartsWith(const std::string& text, const std::string& prefix, const std::string& msg = "")
    {
        if (text.rfind(prefix, 0) != 0) {
            throw std::runtime_error(std::string("Assertion failed: '") + text.substr(0, 50) +
                "...' does not start with '" + prefix + "'" + (msg.empty() ? "" : " (") + msg + ")");
        }
    }

    void PrintReport() const
    {
        int total = static_cast<int>(results_.size());
        int passed = 0;
        std::string currentSuite;

        std::cout << "\n========================================\n";
        std::cout << "           TEST REPORT\n";
        std::cout << "========================================\n\n";

        for (const auto& tc : results_) {
            if (tc.suite != currentSuite) {
                currentSuite = tc.suite;
                std::cout << "[" << currentSuite << "]\n";
            }
            std::string status = tc.passed ? "PASS" : "FAIL";
            std::cout << "  [" << status << "] " << tc.name;
            if (!tc.passed) {
                std::cout << " -> " << tc.actual;
            }
            std::cout << "\n";
            if (tc.passed) passed++;
        }

        std::cout << "\n========================================\n";
        std::cout << "Total: " << total << " | Passed: " << passed << " | Failed: " << (total - passed) << "\n";
        std::cout << "========================================\n";
    }

    int GetTotal() const 
    { 
        return static_cast<int>(results_.size()); 
    }

    int GetPassed() const
    {
        int c = 0;
        for (const auto& r : results_) if (r.passed) c++;
        return c;
    }

    int GetFailed() const 
    { 
        return GetTotal() - GetPassed(); 
    }

    const std::vector<TestCase>& GetResults() const 
    { 
        return results_; 
    }

private:
    std::vector<TestCase> results_;
};

// Test registration macro
#define TEST(suite, name) \
    static void test_##suite##_##name(); \
    struct test_reg_##suite##_##name { \
        test_reg_##suite##_##name() { \
            TestRunner::Instance().AddTest(#suite, #name, test_##suite##_##name); \
        } \
    } test_reg_##suite##_##name##_instance; \
    static void test_##suite##_##name()

// Run all registered tests and return exit code
inline int RunAllTests()
{
    TestRunner::Instance().PrintReport();
    return TestRunner::Instance().GetFailed() > 0 ? 1 : 0;
}
