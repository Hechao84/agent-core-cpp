#include "src/utils/time_utils.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace jiuwen {

std::string NowUtcIso8601()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

}  // namespace jiuwen
