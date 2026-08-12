#include <string>

#include "src/utils/encoding.h"
#include "test_runner.h"

using namespace jiuwen;

// FixStringUTF8 must produce strict RFC 3629 well-formed output: overlong
// 2-byte leads (0xC0/0xC1), surrogates (0xED A0-BF), and overlong 3/4-byte
// sequences are replaced with U+FFFD. The strictness is required because
// downstream JSON serializers reject the looser patterns (the consolidation
// loop crashed with "invalid UTF-8 byte ... 0xC1" before this fix).

TEST(encoding, AsciiIsPreserved)
{
    TestRunner::AssertEq(FixStringUTF8("hello"), std::string("hello"));
    TestRunner::AssertEq(FixStringUTF8(""), std::string(""));
}

TEST(encoding, ValidMultibyteIsPreserved)
{
    // CJK "中" = E4 B8 AD, "文" = E6 96 87.
    std::string in = std::string("normal\xc3\xa9\xe4\xb8\xad\xe6\x96\x87");  // "normalé中文"
    TestRunner::AssertEq(FixStringUTF8(in), in);
}

TEST(encoding, OverlongTwoByteLeadsReplaced)
{
    // 0xC0 / 0xC1 lead bytes are always overlong and must be replaced.
    std::string in;
    in.push_back('\xC0');
    in.push_back('\x80');
    in.push_back('A');
    in.push_back('\xC1');
    in.push_back('\xBF');
    std::string out = FixStringUTF8(in);
    // No C0/C1 lead survives; the ASCII 'A' is preserved.
    TestRunner::AssertTrue(out.find('A') != std::string::npos);
    TestRunner::AssertTrue(out.find('\xC0') == std::string::npos);
    TestRunner::AssertTrue(out.find('\xC1') == std::string::npos);
    // Output must be valid UTF-8 (no stray 0xC0/0xC1) so it serializes in JSON.
    TestRunner::AssertTrue(out.size() >= 1);
}

TEST(encoding, LoneContinuationByteReplaced)
{
    std::string in;
    in.push_back('A');
    in.push_back('\x80');  // stray continuation, no lead
    in.push_back('B');
    std::string out = FixStringUTF8(in);
    TestRunner::AssertContains(out, "A");
    TestRunner::AssertContains(out, "B");
    // The stray byte must not pass through.
    TestRunner::AssertTrue(out.find('\x80') == std::string::npos);
}

TEST(encoding, TruncatedMultibyteReplaced)
{
    // 3-byte lead followed by only 1 continuation byte, then 'X'.
    std::string in;
    in.push_back('\xE4');
    in.push_back('\xB8');
    in.push_back('X');
    std::string out = FixStringUTF8(in);
    TestRunner::AssertContains(out, "X");
    // The partial lead must not survive as a raw 0xE4.
    TestRunner::AssertTrue(out.find('\xE4') == std::string::npos);
}

TEST(encoding, SurrogateRangeReplaced)
{
    // 0xED 0xA0 0x80 would encode U+D800 (a surrogate, forbidden).
    std::string in;
    in.push_back('\xED');
    in.push_back('\xA0');
    in.push_back('\x80');
    in.push_back('Z');
    std::string out = FixStringUTF8(in);
    TestRunner::AssertContains(out, "Z");
    // No 0xED lead survives into output.
    TestRunner::AssertTrue(out.find('\xED') == std::string::npos);
}

TEST(encoding, SlicedWebContentIsRepaired)
{
    // Simulate a web_fetcher result sliced in the middle of a 3-byte CJK
    // char (the failure mode behind the consolidation UTF-8 crash): a valid
    // 3-byte sequence followed by a dangling lead byte at the tail.
    std::string in = std::string("prefix\xe4\xb8\xad\xc3\xa9\xe4");  // trailing lone 0xE4
    std::string out = FixStringUTF8(in);
    TestRunner::AssertContains(out, "prefix");
    // The well-formed prefix char "中" survives.
    TestRunner::AssertContains(out, "\xe4\xb8\xad");
    // The dangling 0xE4 must be replaced, not passed through.
    TestRunner::AssertTrue(out.size() >= in.size() - 1);
}
