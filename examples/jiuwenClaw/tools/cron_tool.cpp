#include "cron_tool.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "src/3rd-party/include/nlohmann/json.hpp"
#include "src/utils/data_dir.h"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

// Generate a simple job ID based on timestamp
static std::string GenerateJobId()
{
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return "job_" + std::to_string(epoch);
}

// Parse ISO datetime string to epoch seconds.
// Handles both UTC (Z suffix) and local time formats.
// If isUTC is true, treats the input as UTC; otherwise as local time.
static time_t ParseISOTime(const std::string& iso, bool isUTC)
{
    std::tm tm = {};
    std::string input = iso;

    // Check for Z suffix (UTC)
    if (!input.empty() && input.back() == 'Z') {
        isUTC = true;
        input.pop_back();
    }

    // Parse the datetime
    std::istringstream iss(input);
    iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (iss.fail()) {
        iss.clear();
        iss.str(input);
        iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    }
    if (iss.fail()) return 0;

    // Normalize tm fields not set by get_time
    tm.tm_isdst = -1;

    // Convert to epoch
    if (isUTC) {
#ifdef _WIN32
        return _mkgmtime(&tm);
#else
        time_t result = timegm(&tm);
        if (result == (time_t)-1) result = 0;
        return result;
#endif
    } else {
        return mktime(&tm);
    }
}

CronTool::CronTool() : Tool("cron", "Schedule, list, and remove reminders. Use when user asks to set a reminder, list scheduled tasks, or remove a scheduled task.", {
    ToolParam{"action", "Action: 'add', 'list', or 'remove'", "string", true},
    ToolParam{"message", "Reminder message", "string", false},
    ToolParam{"every_seconds", "Interval in seconds for recurring reminders", "integer", false},
    ToolParam{"at", "ISO datetime for one-time reminders (e.g. 2026-01-15T14:00:00)", "string", false},
    ToolParam{"cron_expr", "Cron expression (e.g. \"0 9 * * 1-5\" for weekdays at 9am)", "string", false},
    ToolParam{"job_id", "Job ID to remove (from list output)", "string", false},
    ToolParam{"timezone", "IANA timezone name (e.g. Asia/Shanghai). Default: local timezone", "string", false}
})
{
}

std::string CronTool::GetDataFile() const
{
    return GetDataDir().GetFilePath("cron", "jobs.json");
}

std::string CronTool::AddReminder(const std::string& message, double everySeconds, const std::string& atTime,
                                   const std::string& cronExpr, const std::string& tz)
{
    (void)tz;
    std::string dataFile = GetDataFile();
    nlohmann::json jobs;

    // Load existing jobs
    if (fs::exists(dataFile)) {
        try {
            std::ifstream ifs(dataFile);
            jobs = nlohmann::json::parse(ifs);
            if (!jobs.is_array()) jobs = nlohmann::json::array();
        } catch (...) {
            jobs = nlohmann::json::array();
        }
    } else {
        jobs = nlohmann::json::array();
    }

    std::string jobId = GenerateJobId();
    nlohmann::json job;
    job["id"] = jobId;
    job["message"] = message.empty() ? "Reminder" : message;
    job["created_at"] = std::time(nullptr);
    job["fired"] = false;

    if (everySeconds > 0) {
        job["type"] = "recurring";
        job["every_seconds"] = everySeconds;
        job["next_fire"] = std::time(nullptr) + static_cast<time_t>(everySeconds);
    } else if (!atTime.empty()) {
        // Determine timezone interpretation mode
        bool isUTC = false;
        bool hasZ = (!atTime.empty() && atTime.back() == 'Z');
        // Check for explicit UTC offset (e.g. +08:00, -05:00)
        bool hasOffset = (atTime.find('+', 11) != std::string::npos || atTime.find('-', 11) != std::string::npos);

        if (hasZ) {
            isUTC = true;
        } else if (!hasOffset && tz.empty()) {
            // No timezone indicator provided.
            // Default assumption: LLMs typically output UTC time when no timezone is specified.
            // If we treated this as local time, it might be interpreted as "in the past".
            isUTC = true;
        } else {
            // Fallback to local time if tz param is provided (assuming tz matches system local)
            // Or if offset is present (though offset parsing isn't fully implemented here)
            isUTC = false;
        }

        time_t targetTime = ParseISOTime(atTime, isUTC);
        if (targetTime == 0) {
            return "Error: Invalid datetime format for 'at'. Use ISO format like 2026-01-15T14:00:00Z";
        }
        if (targetTime <= std::time(nullptr)) {
            return "Error: The specified time '" + atTime + "' is in the past.";
        }
        job["type"] = "one-time";
        job["at"] = targetTime;
        job["next_fire"] = targetTime;
    } else if (!cronExpr.empty()) {
        job["type"] = "cron";
        job["cron_expr"] = cronExpr;
        // Calculate approximate next fire
        job["next_fire"] = std::time(nullptr) + 60; // Will be checked on next poll
    } else {
        return "Error: Must specify one of: every_seconds, at, or cron_expr for adding a reminder.";
    }

    jobs.push_back(job);

    std::string jobLog = "Added " + job["type"].get<std::string>() + " job id=" + jobId + " msg=" + message;
    LOG(INFO) << "[CronTool] " << jobLog;

    // Save
    std::ofstream ofs(dataFile);
    ofs << jobs.dump(2);
    ofs.close();

    // Format next_fire time for display
    time_t nextFire = job.value("next_fire", std::time(nullptr));
    std::tm* local = std::localtime(&nextFire);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", local);

    std::string result = "Reminder scheduled successfully!\n";
    result += "Job ID: " + jobId + "\n";
    result += "Message: " + job["message"].get<std::string>() + "\n";
    result += "Next fire: " + std::string(buf) + "\n";
    result += "Type: " + job["type"].get<std::string>();

    return result;
}

std::string CronTool::ListReminders()
{
    std::string dataFile = GetDataFile();
    if (!fs::exists(dataFile)) {
        return "No scheduled reminders.";
    }

    try {
        std::ifstream ifs(dataFile);
        nlohmann::json jobs = nlohmann::json::parse(ifs);
        if (!jobs.is_array() || jobs.empty()) {
            return "No scheduled reminders.";
        }

        std::string result = "Scheduled reminders:\n\n";
        for (const auto& job : jobs) {
            if (job.value("fired", false)) continue;
            if (job.value("removed", false)) continue;

            result += "* ID: " + job.value("id", "unknown") + "\n";
            result += "  Message: " + job.value("message", "") + "\n";
            result += "  Type: " + job.value("type", "") + "\n";

            time_t nextFire = job.value("next_fire", 0);
            if (nextFire > 0) {
                std::tm* local = std::localtime(&nextFire);
                char buf[64];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", local);
                result += "  Next fire: " + std::string(buf) + "\n";
            }
            result += "\n";
        }

        if (result == "Scheduled reminders:\n\n") {
            return "No active reminders.";
        }
        return result;
    } catch (const std::exception& e) {
        return "Error reading reminders: " + std::string(e.what());
    }
}

std::string CronTool::RemoveReminder(const std::string& jobId)
{
    std::string dataFile = GetDataFile();
    if (!fs::exists(dataFile)) {
        return "No reminders found with ID: " + jobId;
    }

    try {
        std::ifstream ifs(dataFile);
        nlohmann::json jobs = nlohmann::json::parse(ifs);
        if (!jobs.is_array()) return "Invalid reminder data.";

        bool found = false;
        for (auto& job : jobs) {
            if (job.value("id", "") == jobId) {
                job["removed"] = true;
                found = true;
                break;
            }
        }

        if (!found) return "No reminder found with ID: " + jobId;

        // Clean up removed jobs
        nlohmann::json cleaned;
        for (const auto& job : jobs) {
            if (!job.value("removed", false) && !job.value("fired", false)) {
                cleaned.push_back(job);
            }
        }

        std::ofstream ofs(dataFile);
        ofs << cleaned.dump(2);
        ofs.close();

        return "Reminder " + jobId + " removed successfully.";
    } catch (const std::exception& e) {
        return "Error removing reminder: " + std::string(e.what());
    }
}

std::string CronTool::Invoke(const std::string& input)
{
    std::string action;
    std::string message;
    double everySeconds = 0;
    std::string atTime;
    std::string cronExpr;
    std::string tz;
    std::string jobId;

    try {
        nlohmann::json j = nlohmann::json::parse(input);
        action = j.value("action", "");
        message = j.value("message", "");
        everySeconds = j.value("every_seconds", 0.0);
        atTime = j.value("at", "");
        cronExpr = j.value("cron_expr", "");
        tz = j.value("timezone", "");
        jobId = j.value("job_id", "");
    } catch (const std::exception& e) {
        return "Error: Invalid JSON input. Expected: {\"action\": \"add|list|remove\", ...}\nDetails: " + std::string(e.what());
    }

    if (action.empty()) {
        return "Error: 'action' parameter is required. Use 'add', 'list', or 'remove'.";
    }

    if (action == "add") {
        return AddReminder(message, everySeconds, atTime, cronExpr, tz);
    } else if (action == "list") {
        return ListReminders();
    } else if (action == "remove") {
        if (jobId.empty()) {
            return "Error: 'job_id' parameter is required for remove action.";
        }
        return RemoveReminder(jobId);
    } else {
        return "Error: Invalid action '" + action + "'. Use 'add', 'list', or 'remove'.";
    }
}
