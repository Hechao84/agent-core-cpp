#pragma once

#include <string>

namespace jiuwen {

/// Validate and repair UTF-8 sequences.
/// Invalid bytes are replaced with the Unicode replacement character U+FFFD.
std::string FixStringUTF8(const std::string& str);

} // namespace jiuwen
