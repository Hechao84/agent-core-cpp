
// Test declaration macros - tests are registered via static constructors
// in the individual test files. This main just runs them all.

#include <iostream>
#include "test_runner.h"

int main()
{
    std::cout << "Running jiuwen-lite Unit Tests...\n";
    std::cout << "================================\n";
    return RunAllTests();
}
