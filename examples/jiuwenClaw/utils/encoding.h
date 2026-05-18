#pragma once

#include <string>

namespace jiuwenClaw {

std::string LocalToUTF8(const std::string& str);

std::string UTF8ToLocal(const std::string& str);

std::string CleanInput(const std::string& input);

std::string FixUTF8(const std::string& str);

std::string FixUTF8Streaming(const std::string& str, std::string& carryBuffer);

} // namespace jiuwenClaw
