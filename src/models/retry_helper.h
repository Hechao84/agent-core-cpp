#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>

#include "include/types.h"
#include "third_party/include/curl/curl.h"

namespace jiuwen {

inline int ComputeBackoffDelayMs(int attempt, const RetryPolicy& policy)
{
    int delay = policy.baseDelayMs;
    for (int i = 0; i < attempt; ++i) {
        delay *= 2;
    }
    delay = std::min(delay, policy.maxDelayMs);

    if (policy.withJitter) {
        static std::mt19937 rng(std::random_device{}());
        int jitter = std::uniform_int_distribution<int>(0, delay / 2)(rng);
        delay += jitter;
    }

    return std::min(delay, policy.maxDelayMs);
}

inline bool IsRetryableHttpStatus(long httpCode)
{
    return httpCode == 429
        || httpCode == 500
        || httpCode == 502
        || httpCode == 503
        || httpCode == 504;
}

inline bool IsRetryableCurlError(CURLcode code)
{
    return code == CURLE_OPERATION_TIMEDOUT
        || code == CURLE_COULDNT_CONNECT
        || code == CURLE_COULDNT_RESOLVE_HOST
        || code == CURLE_RECV_ERROR
        || code == CURLE_SEND_ERROR
        || code == CURLE_GOT_NOTHING
        || code == CURLE_PARTIAL_FILE;
}

inline void SleepBackoff(int attempt, const RetryPolicy& policy)
{
    int delayMs = ComputeBackoffDelayMs(attempt, policy);
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
}

} // namespace jiuwen
