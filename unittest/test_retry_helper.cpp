#include "unittest/test_runner.h"
#include "src/utils/retry_helper.h"

using namespace jiuwen;

TEST(RetryHelper, BackoffExponentialGrowth)
{
    RetryPolicy policy;
    policy.baseDelayMs = 400;
    policy.maxDelayMs = 3000;
    policy.withJitter = false;

    int d0 = ComputeBackoffDelayMs(0, policy);
    int d1 = ComputeBackoffDelayMs(1, policy);
    int d2 = ComputeBackoffDelayMs(2, policy);

    TestRunner::AssertEq(d0, 400, "attempt 0 delay");
    TestRunner::AssertEq(d1, 800, "attempt 1 delay");
    TestRunner::AssertEq(d2, 1600, "attempt 2 delay");
}

TEST(RetryHelper, BackoffCapAtMaxDelay)
{
    RetryPolicy policy;
    policy.baseDelayMs = 1000;
    policy.maxDelayMs = 3000;
    policy.withJitter = false;

    int d3 = ComputeBackoffDelayMs(3, policy);
    int d4 = ComputeBackoffDelayMs(4, policy);

    TestRunner::AssertEq(d3, 3000, "attempt 3 capped");
    TestRunner::AssertEq(d4, 3000, "attempt 4 capped");
}

TEST(RetryHelper, BackoffWithJitterInRange)
{
    RetryPolicy policy;
    policy.baseDelayMs = 400;
    policy.maxDelayMs = 3000;
    policy.withJitter = true;

    for (int attempt = 0; attempt < 5; ++attempt) {
        int delay = ComputeBackoffDelayMs(attempt, policy);
        int baseNoJitter = std::min(policy.baseDelayMs * (1 << attempt), policy.maxDelayMs);
        TestRunner::AssertTrue(delay >= baseNoJitter, "jitter >= base");
        TestRunner::AssertTrue(delay <= baseNoJitter + baseNoJitter / 2, "jitter <= base + base/2");
        TestRunner::AssertTrue(delay <= policy.maxDelayMs, "capped at max");
    }
}

TEST(RetryHelper, RetryableHttpStatus)
{
    TestRunner::AssertTrue(IsRetryableHttpStatus(429), "429 rate-limit retryable");
    TestRunner::AssertTrue(IsRetryableHttpStatus(500), "500 retryable");
    TestRunner::AssertTrue(IsRetryableHttpStatus(502), "502 retryable");
    TestRunner::AssertTrue(IsRetryableHttpStatus(503), "503 retryable");
    TestRunner::AssertTrue(IsRetryableHttpStatus(504), "504 retryable");

    TestRunner::AssertFalse(IsRetryableHttpStatus(400), "400 not retryable");
    TestRunner::AssertFalse(IsRetryableHttpStatus(401), "401 auth not retryable");
    TestRunner::AssertFalse(IsRetryableHttpStatus(403), "403 forbidden not retryable");
    TestRunner::AssertFalse(IsRetryableHttpStatus(404), "404 not retryable");
    TestRunner::AssertFalse(IsRetryableHttpStatus(200), "200 success not retryable");
}

TEST(RetryHelper, RetryableCurlError)
{
    TestRunner::AssertTrue(IsRetryableCurlError(CURLE_OPERATION_TIMEDOUT), "timeout retryable");
    TestRunner::AssertTrue(IsRetryableCurlError(CURLE_COULDNT_CONNECT), "connect fail retryable");
    TestRunner::AssertTrue(IsRetryableCurlError(CURLE_RECV_ERROR), "recv error retryable");
    TestRunner::AssertTrue(IsRetryableCurlError(CURLE_SEND_ERROR), "send error retryable");

    TestRunner::AssertFalse(IsRetryableCurlError(CURLE_OK), "CURLE_OK not retryable");
}
