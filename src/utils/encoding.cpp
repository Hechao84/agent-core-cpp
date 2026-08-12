#include "src/utils/encoding.h"

namespace jiuwen {

namespace {

// Replacement character U+FFFD, UTF-8 encoded (EF BF BD).
constexpr char kReplacement[] = "\xEF\xBF\xBD";

inline bool IsCont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Append kReplacement for `count` invalid bytes. When a multibyte lead is
// malformed, only the lead byte is consumed as invalid; the trailing bytes
// are re-evaluated on the next loop pass so a stray continuation byte does
// not swallow following valid bytes.
inline void EmitReplacement(std::string& out, size_t& i, size_t count)
{
    out += kReplacement;
    i += count;
}

} // namespace

std::string FixStringUTF8(const std::string& str)
{
    std::string result;
    result.reserve(str.size());
    const size_t len = str.length();
    size_t i = 0;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(str[i]);

        // 1-byte: 0x00-0x7F.
        if (c <= 0x7F) {
            result.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        // Determine expected length and validate the lead byte per RFC 3629.
        // 0xC0/0xC1 are overlong 2-byte leads and are invalid. 0xF5-0xFF are
        // out of Unicode range. These strict checks matter because downstream
        // JSON serializers (nlohmann/json) reject the looser "well-formed
        // ISCES" byte patterns that an earlier validator accepted, surfacing
        // as "invalid UTF-8 byte ... 0xC1" at consolidation time.
        int expectedLength = 0;
        unsigned char minSecond = 0x80;  // minimum legal value of the 2nd byte
        unsigned char maxSecond = 0xBF;  // maximum legal value of the 2nd byte

        if (c >= 0xC2 && c <= 0xDF) {
            expectedLength = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            expectedLength = 3;
            // 0xE0 requires 2nd byte 0xA0-0xBF (avoid overlong).
            // 0xED requires 2nd byte 0x80-0x9F (avoid UTF-16 surrogates).
            if (c == 0xE0) { minSecond = 0xA0; }
            else if (c == 0xED) { maxSecond = 0x9F; }
        } else if (c >= 0xF0 && c <= 0xF4) {
            expectedLength = 4;
            // 0xF0 requires 2nd byte 0x90-0xBF (avoid overlong).
            // 0xF4 requires 2nd byte 0x80-0x8F (stay <= U+10FFFF).
            if (c == 0xF0) { minSecond = 0x90; }
            else if (c == 0xF4) { maxSecond = 0x8F; }
        } else {
            // 0x80-0xBF (stray continuation), 0xC0/0xC1 (overlong), 0xF5-0xFF.
            EmitReplacement(result, i, 1);
            continue;
        }

        // Need the full sequence to be present.
        if (i + expectedLength > len) {
            EmitReplacement(result, i, 1);
            continue;
        }

        // Validate the 2nd byte (lead-specific range) then remaining
        // continuation bytes (0x80-0xBF).
        unsigned char b2 = static_cast<unsigned char>(str[i + 1]);
        if (b2 < minSecond || b2 > maxSecond) {
            EmitReplacement(result, i, 1);
            continue;
        }
        bool valid = true;
        for (int j = 2; j < expectedLength; ++j) {
            if (!IsCont(static_cast<unsigned char>(str[i + j]))) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            EmitReplacement(result, i, 1);
            continue;
        }

        result.append(str, i, expectedLength);
        i += expectedLength;
    }
    return result;
}

} // namespace jiuwen
