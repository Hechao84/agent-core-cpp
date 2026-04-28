#pragma once

#include <functional>
#include <iostream>
#include <string>
#include "examples/jiuwenClaw/utils/encoding.h"

namespace jiuwenClaw {

class AgentResponseHandler {
public:
    using OutputCallback = std::function<void(const std::string&)>;

    explicit AgentResponseHandler(bool enableStreamCarry = true);

    OutputCallback GetCallback() const;

    std::string GetFullResponse() const;

    void Reset();

private:
    void HandleResponse(const std::string& resp) const;
    std::string ExtractTagContent(const std::string& s, const std::string& tag) const;

    mutable std::string utf8Carry_;
    std::string fullResponse_;
    bool enableStreamCarry_;
};

} // namespace jiuwenClaw
