#include "src/utils/encoding.h"

namespace jiuwen {

std::string FixStringUTF8(const std::string& str)
{
    std::string result;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        int expectedLength = 0;

        if ((c & 0x80) == 0) expectedLength = 1;
        else if ((c & 0xE0) == 0xC0) expectedLength = 2;
        else if ((c & 0xF0) == 0xE0) expectedLength = 3;
        else if ((c & 0xF8) == 0xF0) expectedLength = 4;
        else { result += "\xEF\xBF\xBD"; i++; continue; }

        if (i + expectedLength > str.length()) { result += "\xEF\xBF\xBD"; break; }

        bool valid = true;
        for (int j = 1; j < expectedLength; ++j) {
            if ((str[i + j] & 0xC0) != 0x80) { valid = false; break; }
        }

        if (valid) result.append(str.substr(i, expectedLength));
        else result += "\xEF\xBF\xBD";
        i += expectedLength;
    }
    return result;
}

} // namespace jiuwen
