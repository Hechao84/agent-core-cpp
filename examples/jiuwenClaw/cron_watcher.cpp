#include "cron_watcher.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <chrono>
#include "include/resource_manager.h"
#include "src/3rd-party/include/nlohmann/json.hpp"
#include "src/utils/logger.h"

namespace fs = std::filesystem;

static bool MatchesCronExpression(const std::string& cronExpr, const std::tm& tm)
{
    std::istringstream iss(cronExpr);
    std::string minStr, hourStr, dayStr, monthStr, weekdayStr;

    if (!(iss >> minStr >> hourStr >> dayStr >> monthStr >> weekdayStr)) {
        return false;
    }

    if (minStr != "*") {
        int cronMin = std::stoi(minStr);
        if (tm.tm_min != cronMin) return false;
    }

    if (hourStr != "*") {
        int cronHour = std::stoi(hourStr);
        if (tm.tm_hour != cronHour) return false;
    }

    if (dayStr != "*" && dayStr != "?") {
        int cronDay = std::stoi(dayStr);
        if (tm.tm_mday != cronDay) return false;
    }

    if (monthStr != "*") {
        int cronMonth = std::stoi(monthStr);
        if (tm.tm_mon + 1 != cronMonth) return false;
    }

    if (weekdayStr != "*") {
        if (weekdayStr.find('-') != std::string::npos) {
            size_t dashPos = weekdayStr.find('-');
            int start = std::stoi(weekdayStr.substr(0, dashPos));
            int end = std::stoi(weekdayStr.substr(dashPos + 1));
            if (tm.tm_wday < start || tm.tm_wday > end) return false;
        } else {
            int cronWday = std::stoi(weekdayStr);
            if (tm.tm_wday != cronWday) return false;
        }
    }

    return true;
}

CronWatcher::CronWatcher(const std::string& dataDir, Agent& agent, const ModelConfig& modelConfig, int intervalSeconds)
    : dataDir_(dataDir), agent_(agent), modelConfig_(modelConfig), intervalSeconds_(intervalSeconds), running_(true)
{
    try {
        fs::path dir = fs::path(dataDir_) / "cron";
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
        LOG(INFO) << "[CronWatcher] Initialized, dataDir=" << dataDir_ << ", interval=" << intervalSeconds_ << "s";
        thread_ = std::thread(&CronWatcher::Run, this);
    } catch (const std::exception& e) {
        LOG(ERROR) << "[CronWatcher] Init Error: " << e.what();
    }
}

CronWatcher::~CronWatcher() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG(INFO) << "[CronWatcher] Shutdown complete.";
}

void CronWatcher::HandleEvent(const CronTriggerEvent& event) {
    LOG(INFO) << "[CRON-AGENT] Handling event: " << event.message;
    std::cout << "\n[CronAgent] Processing: " << event.message << "...\n" << std::flush;

    std::string prompt = "You have been triggered by a scheduled cron event:\n\n"
                         "**Event**: " + event.message + "\n\n"
                         "INSTRUCTIONS:\n"
                         "1. Understand the context of this event.\n"
                         "2. Execute the appropriate action based on what this reminder is about.\n"
                         "3. If this requires updating files or state, use your tools to do so.\n";

    std::string fullResponse = agent_.Invoke(prompt, [](const std::string& resp) {
        std::string s = resp;

        std::vector<std::string> streamTags = {"[STREAM] ", "[STREAM]"};
        for (const auto& st : streamTags) {
            size_t p = s.find(st);
            while (p != std::string::npos) {
                s.erase(p, st.length());
                p = s.find(st, p);
            }
        }

        std::vector<std::string> hiddenTags = {"[STATUS]", "[THOUGHT]", "[ACTION]",
                                               "[TOOL_CALLS]", "[TOOL_RESPONSE]",
                                               "[RESPONSE]", "[FINAL]", "[ERROR]"};
        for (const auto& tag : hiddenTags) {
            size_t pos = s.find(tag);
            while (pos != std::string::npos) {
                size_t contentStart = s.find('{', pos);
                size_t endPos = std::string::npos;

                if (contentStart != std::string::npos && contentStart < pos + tag.length() + 2) {
                    int depth = 1;
                    for (size_t i = contentStart + 1; i < s.length() && depth > 0; i++) {
                        if (s[i] == '{') depth++;
                        else if (s[i] == '}') depth--;
                        if (depth == 0) endPos = i + 1;
                    }
                }

                if (endPos == std::string::npos) {
                    size_t nextTagPos = std::string::npos;
                    for (const auto& ht : hiddenTags) {
                        size_t p2 = s.find(ht, pos + tag.length());
                        if (p2 != std::string::npos && (nextTagPos == std::string::npos || p2 < nextTagPos)) {
                            nextTagPos = p2;
                        }
                    }
                    endPos = (nextTagPos != std::string::npos) ? nextTagPos : s.length();
                }

                s.erase(pos, endPos - pos);
                pos = s.find(tag);
            }
        }

        if (!s.empty()) {
            std::cout << s << std::flush;
        }
    });

    LOG(INFO) << "[CRON-AGENT] Event handled. Response length: " << fullResponse.length();
    std::cout << "\n";
}

void CronWatcher::Run() {
    while (running_) {
        for (int i = 0; i < intervalSeconds_ && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_) break;

        CheckAndFireCrons();
    }
}

void CronWatcher::CheckAndFireCrons() {
    std::string jobsPath = fs::path(dataDir_) / "cron" / "jobs.json";
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

            // Skip already-fired one-time tasks
            if (job.value("fired", false) && job.value("type", "") == "one-time") continue;

            bool shouldFire = false;
            std::string type = job.value("type", "");
            double intervalSec = 0;

            if (type == "one-time") {
                time_t at = job.value("at", 0);
                if (at > 0 && now >= at) shouldFire = true;
            } else if (type == "recurring") {
                intervalSec = job.value("every_seconds", 60.0);
                time_t next = job.value("next_fire", 0);
                if (now >= next) shouldFire = true;
            } else if (type == "cron") {
                std::string expr = job.value("cron_expr", "");
                time_t next = job.value("next_fire", 0);
                if (!expr.empty() && now >= next) {
                    // Advance window on every check to prevent stale next_fire
                    // from causing unintended matches
                    job["next_fire"] = now + 60;
                    modified = true;
                    if (MatchesCronExpression(expr, localTm)) {
                        shouldFire = true;
                    }
                }
            }

            if (shouldFire) {
                std::string msg = job.value("message", "Timer Fired");
                std::string jobId = job.value("id", "unknown");
                CronTriggerEvent event{msg, jobId, type, (type == "one-time")};

                LOG(INFO) << "[CronWatcher] Triggered [" << type << "] job=" << jobId << " message=" << msg;

                HandleEvent(event);

                if (type == "one-time") {
                    // Mark for removal instead of just setting fired=true
                    job["removed"] = true;
                    modified = true;
                } else {
                    // Update next_fire for recurring/cron
                    if (intervalSec <= 0) intervalSec = 60.0;
                    job["next_fire"] = now + static_cast<time_t>(intervalSec);
                    job["fired"] = false;
                    modified = true;
                    LOG(INFO) << "[CronWatcher] Updated next_fire for recurring job=" << jobId;
                }
            }
        }

        // Remove one-time jobs that have been fired
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
        LOG(ERROR) << "[CronWatcher] Error processing jobs: " << e.what();
        std::cerr << "\n[CRON WATCHER] Error: " << e.what() << "\n" << std::flush;
    }
}
