#pragma once

#include <functional>
#include <string>
#include "include/tool.h"

namespace jiuwen {

class AskUserDispatcher;

// AskUserTool emits a structured question via the streaming callback and
// blocks until the application layer (HTTP / CLI / channel) calls back
// with a response, or until the 60s timeout elapses.
class AskUserTool : public Tool {
public:
    using StreamCallback = std::function<void(const std::string&)>;

    AskUserTool(AskUserDispatcher* dispatcher, StreamCallback streamCallback);
    std::string Invoke(const std::string& input) override;

private:
    AskUserDispatcher* dispatcher_;
    StreamCallback streamCallback_;
};

} // namespace jiuwen
