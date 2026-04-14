#include "test_runner.h"

// Test declaration macros - tests are registered via static constructors
// in the individual test files. This main just runs them all.

int main() {
    std::cout << "Running jiuwen-lite Unit Tests...\n";
    std::cout << "================================\n";
    return RunAllTests();
}
