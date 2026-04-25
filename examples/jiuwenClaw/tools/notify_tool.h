#pragma once

#include <string>
#include "include/tool.h"

using namespace jiuwen;

namespace jiuwenClaw {

class NotifyTool : public Tool {
public:
    NotifyTool();
    std::string Invoke(const std::string& input) override;

private:
    void SendNotification(const std::string& title, const std::string& message) const;
};

} // namespace jiuwenClaw
