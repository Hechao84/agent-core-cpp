#include "cron_watcher.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "include/resource_manager.h"
#include "examples/jiuwenClaw/utils/agent_response_handler.h"
#include "examples/jiuwenClaw/utils/encoding.h"
#include "examples/jiuwenClaw/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;

namespace fs = std::filesystem;

namespace {

bool MatchesField(const std::string& field, int value, int minVal, int maxVal)
{
    if (field == "*" || field == "?") return true;

    if (field.find(',') != std::string::npos) {
        std::istringstream iss(field);
        std::string part;
        bool matched = false;
        while (std::getline(iss, part, ',')) {
            if (MatchesField(part, value, minVal, maxVal)) {
                matched = true;
                break;
            }
        }
        return matched;
    }

    if (field.find('-') != std::string::npos) {
        size_t dashPos = field.find('-');
        int start = std::stoi(field.substr(0, dashPos));
        int end = std::stoi(field.substr(dashPos + 1));
        return value >= start && value <= end;
    }

    if (field.find('/') != std::string::npos) {
        size_t slashPos = field.find('/');
        std::string base = field.substr(0, slashPos);
        int step = std::stoi(field.substr(slashPos + 1));
        if (step <= 0) return false;
        int start = (base == "*") ? minVal : std::stoi(base);
        for (int v = start; v <= maxVal; v += step) {
            if (v == value) return true;
        }
        return false;
    }

    int target = std::stoi(field);
    return value == target;
}

bool MatchesCronExpression(const std::string& cronExpr, const std::tm& tm)
{
    std::istringstream iss(cronExpr);
    std::string minStr, hourStr, dayStr, monthStr, weekdayStr;

    if (!(iss >> minStr >> hourStr >> dayStr >> monthStr >> weekdayStr)) {
        return false;
    }

    if (!MatchesField(minStr, tm.tm_min, 0, 59)) return false;
    if (!MatchesField(hourStr, tm.tm_hour, 0, 23)) return false;
    if (!MatchesField(dayStr, tm.tm_mday, 1, 31)) return false;
    if (!MatchesField(monthStr, tm.tm_mon + 1, 1, 12)) return false;
    if (!MatchesField(weekdayStr, tm.tm_wday, 0, 6)) return false;

    return true;
}

} // namespace

namespace jiuwenClaw {

time_t CalculateNextFire(const std::string& cronExpr, time_t afterTime)
{
    std::istringstream iss(cronExpr);
    std::string minStr, hourStr, dayStr, monthStr, weekdayStr;

    if (!(iss >> minStr >> hourStr >> dayStr >> monthStr >> weekdayStr)) {
        return afterTime + 60;
    }

    std::tm tm = *std::localtime(&afterTime);
    tm.tm_sec = 0;
    tm.tm_min++;

    bool domIsWildcard = (dayStr == "*" || dayStr == "?");
    bool dowIsWildcard = (weekdayStr == "*" || weekdayStr == "?");

    for (int i = 0; i < 525600; ++i) {
        time_t currentTime = std::mktime(&tm);
        if (currentTime == -1) {
            return afterTime + 60;
        }
        tm = *std::localtime(&currentTime);

        bool dayMatches = false;
        if (domIsWildcard && dowIsWildcard) {
            dayMatches = true;
        } else if (domIsWildcard) {
            dayMatches = MatchesField(weekdayStr, tm.tm_wday, 0, 6);
        } else if (dowIsWildcard) {
            dayMatches = MatchesField(dayStr, tm.tm_mday, 1, 31);
        } else {
            bool domMatches = MatchesField(dayStr, tm.tm_mday, 1, 31);
            bool dowMatches = MatchesField(weekdayStr, tm.tm_wday, 0, 6);
            dayMatches = domMatches || dowMatches;
        }

        if (MatchesField(minStr, tm.tm_min, 0, 59) &&
            MatchesField(hourStr, tm.tm_hour, 0, 23) &&
            dayMatches &&
            MatchesField(monthStr, tm.tm_mon + 1, 1, 12)) {
            return currentTime;
        }

        tm.tm_min++;
    }

    return afterTime + 60;
}

CronWatcher::CronWatcher(const std::string& dataDir, Agent& agent, const ModelConfig& modelConfig, int intervalSeconds)
    : dataDir_(dataDir), agent_(agent), modelConfig_(modelConfig), intervalSeconds_(intervalSeconds), running_(true)
{
    try {
        fs::path dir = fs::path(dataDir_) / "cron";
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
        LOG(INFO) << "[CronWatcher] Initialized, dataDir=" << dir << ", interval=" << intervalSeconds_ << "s";
        thread_ = std::thread(&CronWatcher::Run, this);
    } catch (const std::exception& e) {
        LOG(ERR) << "[CronWatcher] Init Error: " << e.what();
    }
    LOG(INFO) << "[CronWatcher] Start complete.";
}

CronWatcher::~CronWatcher() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG(INFO) << "[CronWatcher] Shutdown complete.";
}

void CronWatcher::Notify() {
    cv_.notify_all();
}

void CronWatcher::HandleEvent(const CronTriggerEvent& event) {
    LOG(INFO) << "[CRON-AGENT] Handling event: " << event.message;
    std::cout << UTF8ToLocal("\n[CronAgent] Processing: ") << UTF8ToLocal(event.message) << UTF8ToLocal("...\n") << std::flush;

    std::string prompt = "You have been triggered by a scheduled cron event:\n\n"
                         "**Event**: " + event.message + "\n\n"
                         "INSTRUCTIONS:\n"
                         "1. Understand the context of this event.\n"
                         "2. Execute the appropriate action based on what this reminder is about.\n"
                         "3. If this requires updating files or state, use your tools to do so.\n";

    LOG(INFO) << "[CRON-AGENT] Pseudo query for Cron task " << prompt;
    AgentResponseHandler handler;
    std::string fullResponse = agent_.Invoke(prompt, handler.GetCallback());

    LOG(INFO) << "[CRON-AGENT] Event handled. Response length: " << fullResponse.length();
    std::cout << "\n";
}

time_t CronWatcher::CalculateSleepDuration() {
    std::string jobsPath = (fs::path(dataDir_) / "cron" / "jobs.json").string();
    if (!fs::exists(jobsPath)) {
        return intervalSeconds_;
    }

    try {
        std::ifstream ifs(jobsPath);
        nlohmann::json j = nlohmann::json::parse(ifs);
        ifs.close();

        if (!j.is_array() || j.empty()) {
            return intervalSeconds_;
        }

        time_t now = std::time(nullptr);
        time_t earliestNext = 0;

        for (const auto& job : j) {
            if (job.value("removed", false)) continue;
            if (job.value("fired", false) && job.value("type", "") == "one-time") continue;

            std::string type = job.value("type", "");
            time_t next = job.value("next_fire", 0);
            double intervalSec = job.value("every_seconds", 0.0);

            if (type == "one-time") {
                if (next > now && (earliestNext == 0 || next < earliestNext)) {
                    earliestNext = next;
                }
            } else if (type == "recurring") {
                if (next > 0) {
                    time_t actualNext = next;
                    if (actualNext <= now) {
                        if (intervalSec <= 0) intervalSec = 60.0;
                        while (actualNext <= now) {
                            actualNext += static_cast<time_t>(intervalSec);
                        }
                    }
                    if (earliestNext == 0 || actualNext < earliestNext) {
                        earliestNext = actualNext;
                    }
                }
            } else if (type == "cron") {
                std::string expr = job.value("cron_expr", "");
                if (!expr.empty() && next > 0) {
                    time_t actualNext = next;
                    if (actualNext <= now) {
                        actualNext = CalculateNextFire(expr, now);
                    }
                    if (earliestNext == 0 || actualNext < earliestNext) {
                        earliestNext = actualNext;
                    }
                }
            }
        }

        if (earliestNext > now) {
            return std::min(static_cast<time_t>(intervalSeconds_), earliestNext - now);
        }
    } catch (...) {
    }

    return intervalSeconds_;
}

void CronWatcher::Run() {
    while (running_) {
        time_t sleepSec = CalculateSleepDuration();
        if (sleepSec <= 0) sleepSec = 1;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(sleepSec), [this]() { return !running_; });
        }

        if (!running_) break;

        CheckAndFireCrons();
    }
}

void CronWatcher::CheckAndFireCrons() {
    std::string jobsPath = (fs::path(dataDir_) / "cron" / "jobs.json").string();
    if (!fs::exists(jobsPath)) return;

    try {
        std::ifstream ifs(jobsPath);
        nlohmann::json j = nlohmann::json::parse(ifs);
        ifs.close();

        if (!j.is_array()) return;

        time_t now = std::time(nullptr);
        std::tm localTm = *std::localtime(&now);
        bool modified = false;

        for (int i = 0; i < static_cast<int>(j.size()); ++i) {
            auto& job = j[i];
            if (job.value("removed", false)) continue;

            if (job.value("fired", false) && job.value("type", "") == "one-time") continue;

            std::string type = job.value("type", "");
            time_t next = job.value("next_fire", 0);
            double intervalSec = job.value("every_seconds", 0.0);

            bool shouldFire = false;
            if (type == "one-time") {
                time_t at = job.value("at", 0);
                if (at > 0 && now >= at) shouldFire = true;
            } else if (type == "recurring") {
                if (next > 0 && now >= next) shouldFire = true;
            } else if (type == "cron") {
                std::string expr = job.value("cron_expr", "");
                if (!expr.empty() && next > 0 && now >= next) {
                    if (MatchesCronExpression(expr, localTm)) {
                        shouldFire = true;
                    } else {
                        job["next_fire"] = CalculateNextFire(expr, now);
                        modified = true;
                        LOG(INFO) << "[CronWatcher] Recalculated next_fire for missed cron job=" << job.value("id", "unknown");
                    }
                }
            }

            if (shouldFire && running_) {
                std::string msg = job.value("message", "Timer Fired");
                std::string jobId = job.value("id", "unknown");
                CronTriggerEvent event{msg, jobId, type, (type == "one-time")};

                LOG(INFO) << "[CronWatcher] Triggered [" << type << "] job=" << jobId << " message=" << msg;

                HandleEvent(event);

                if (type == "one-time") {
                    job["removed"] = true;
                    modified = true;
                } else if (type == "cron") {
                    std::string expr = job.value("cron_expr", "");
                    job["next_fire"] = CalculateNextFire(expr, now);
                    job["fired"] = false;
                    modified = true;
                    LOG(INFO) << "[CronWatcher] Updated next_fire for cron job=" << jobId;
                } else if (type == "recurring") {
                    if (intervalSec <= 0) intervalSec = 60.0;
                    time_t originalNext = job.value("next_fire", now);
                    job["next_fire"] = originalNext + static_cast<time_t>(intervalSec);
                    job["fired"] = false;
                    modified = true;
                    LOG(INFO) << "[CronWatcher] Updated next_fire for recurring job=" << jobId;
                }

                if (!running_) {
                    return;
                }
            }
        }

        for (int idx = static_cast<int>(j.size()) - 1; idx >= 0; --idx) {
            if (j[idx].value("removed", false) && j[idx].value("type", "") == "one-time") {
                std::string jobMsg = j[idx].value("message", "");
                std::string jobId = j[idx].value("id", "unknown");
                j.erase(idx);
                modified = true;
                LOG(INFO) << "[CronWatcher] Deleted completed one-time job=" << jobId << " message=" << jobMsg;
            }
        }

        if (modified) {
            std::ofstream ofs(jobsPath);
            ofs << j.dump(2);
            ofs.close();
            LOG(INFO) << "[CronWatcher] Jobs state saved to " << jobsPath;
        }

    } catch (const std::exception& e) {
        LOG(ERR) << "[CronWatcher] Error processing jobs: " << e.what();
        std::cerr << "\n[CRON WATCHER] Error: " << e.what() << "\n" << std::flush;
    }
}

} // namespace jiuwenClaw
