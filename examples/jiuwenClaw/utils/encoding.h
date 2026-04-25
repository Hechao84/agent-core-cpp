#pragma once

#include <string>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace jiuwenClaw {

/// Convert local encoding (GBK/ACP) to UTF-8 (for input like stdin/file)
inline std::string LocalToUTF8(const std::string& str)
{
#ifdef _WIN32
    try {
        int wlen = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return str;
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], wlen);

        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return str;
        std::string result(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
        if (!result.empty()) result.pop_back();
        return result;
    } catch (...) {
        return str;
    }
#else
    return str;
#endif
}

/// Convert UTF-8 to local encoding (GBK/ACP) for console output
inline std::string UTF8ToLocal(const std::string& str)
{
#ifdef _WIN32
    try {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return str;
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], wlen);

        int len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return str;
        std::string result(len, 0);
        WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
        if (!result.empty()) result.pop_back();
        return result;
    } catch (...) {
        return str;
    }
#else
    return str;
#endif
}

/// Strip ANSI escape sequences, backspace, delete, and control characters
/// while preserving UTF-8 multi-byte boundaries
inline std::string CleanInput(const std::string& input)
{
    std::string buffer;
    buffer.reserve(input.length());

    for (size_t i = 0; i < input.length(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);

        if (c == 0x08 || c == 0x7F) {
            if (!buffer.empty()) {
                size_t idx = buffer.length() - 1;
                while (idx > 0 && (buffer[idx] & 0xC0) == 0x80) {
                    idx--;
                }
                buffer.erase(idx);
            }
            continue;
        }

        if (c == 0x1B) {
            if (i + 1 < input.length() && input[i+1] == '[') {
                i++;
                while (i + 1 < input.length()) {
                    i++;
                    unsigned char next = static_cast<unsigned char>(input[i]);
                    if ((next >= 'A' && next <= 'z') || next == '@') break;
                }
            }
            continue;
        }

        if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') {
            continue;
        }

        buffer += static_cast<char>(c);
    }

    return buffer;
}

/// Validate and repair UTF-8 sequences
inline std::string FixUTF8(const std::string& str)
{
    std::string result;
    result.reserve(str.length());
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        int expectedLength = 0;

        if ((c & 0x80) == 0) {
            expectedLength = 1;
        } else if ((c & 0xE0) == 0xC0) {
            expectedLength = 2;
        } else if ((c & 0xF0) == 0xE0) {
            expectedLength = 3;
        } else if ((c & 0xF8) == 0xF0) {
            expectedLength = 4;
        } else {
            i++;
            continue;
        }

        if (i + expectedLength > str.length()) {
            break;
        }

        bool valid = true;
        for (int j = 1; j < expectedLength; ++j) {
            if ((str[i + j] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }

        if (valid) {
            result.append(str.substr(i, expectedLength));
            i += expectedLength;
        } else {
            i++;
        }
    }
    return result;
}

/// Streaming UTF-8 validator that handles incomplete multi-byte sequences
/// at chunk boundaries via a carryBuffer
inline std::string FixUTF8Streaming(const std::string& str, std::string& carryBuffer)
{
    std::string fullInput = carryBuffer + str;
    carryBuffer.clear();

    std::string result;
    size_t i = 0;
    while (i < fullInput.length()) {
        unsigned char c = static_cast<unsigned char>(fullInput[i]);
        int expectedLength = 0;

        if ((c & 0x80) == 0) {
            expectedLength = 1;
        } else if ((c & 0xE0) == 0xC0) {
            expectedLength = 2;
        } else if ((c & 0xF0) == 0xE0) {
            expectedLength = 3;
        } else if ((c & 0xF8) == 0xF0) {
            expectedLength = 4;
        } else {
            result += "\xEF\xBF\xBD";
            i++;
            continue;
        }

        if (i + expectedLength > fullInput.length()) {
            carryBuffer = fullInput.substr(i);
            break;
        }

        bool valid = true;
        for (int j = 1; j < expectedLength; ++j) {
            if ((fullInput[i + j] & 0xC0) != 0x80) {
                valid = false;
                break;
            }
        }

        if (valid) {
            result.append(fullInput.substr(i, expectedLength));
        } else {
            result += "\xEF\xBF\xBD";
        }
        i += expectedLength;
    }
    return result;
}

} // namespace jiuwenClaw
