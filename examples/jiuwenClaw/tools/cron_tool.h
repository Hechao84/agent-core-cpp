#pragma once

#include <string>
#include "include/tool.h"

using namespace jiuwen;

namespace jiuwenClaw {

class CronTool : public Tool {
public:
    CronTool();
    std::string Invoke(const std::string& input) override;

private:
    std::string AddReminder(const std::string& message, double everySeconds, const std::string& atTime,
                            const std::string& cronExpr, const std::string& tz);
    std::string ListReminders();
    std::string RemoveReminder(const std::string& jobId);

    std::string GetDataFile() const;
};

} // namespace jiuwenClaw
