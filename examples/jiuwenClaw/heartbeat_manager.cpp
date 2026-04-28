#include "heartbeat_manager.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "include/resource_manager.h"
#include "examples/jiuwenClaw/utils/agent_response_handler.h"
#include "examples/jiuwenClaw/utils/encoding.h"
#include "examples/jiuwenClaw/utils/logger.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;

namespace fs = std::filesystem;

namespace jiuwenClaw {

HeartbeatManager::HeartbeatManager(const std::string& heartbeatFilePath, Agent& agent, const ModelConfig& modelConfig, int intervalSeconds)
    : path_(heartbeatFilePath), agent_(agent), modelConfig_(modelConfig), intervalSeconds_(intervalSeconds), running_(true)
{
    if (!fs::exists(fs::path(path_))) {
        fs::path parentDir = fs::path(path_).parent_path();
        if (!parentDir.empty()) {
            fs::create_directories(parentDir);
        }
        std::ofstream ofs(path_);
        ofs << "# Heartbeat Tasks\n\n## Active Tasks\n\n<!-- Add periodic tasks below -->\n\n## Completed\n\n";
    }
    thread_ = std::thread(&HeartbeatManager::Run, this);
    LOG(INFO) << "[HB-CHECK] Start complete.";
}

HeartbeatManager::~HeartbeatManager() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG(INFO) << "[HB-CHECK] Shutdown complete.";
}

std::string HeartbeatManager::ReadFile() const {
    std::ifstream file(path_);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool HeartbeatManager::DecideAction(const std::string& content, std::string& tasksSummary) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%d %H:%M:%S UTC");

    std::string systemPrompt = "You are a heartbeat coordinator. You review the heartbeat file and decide whether to wake the main agent.\n"
                               "To make a decision, you MUST output a JSON object with the EXACT format:\n"
                               "{\"name\": \"decision\", \"arguments\": {\"action\": \"run\" OR \"skip\", \"tasks\": \"exact task text if run\"}}\n"
                               "Do NOT wrap the JSON in markdown code blocks.\n";

    std::string userPrompt = "Current Time: " + ss.str() + "\n\n"
                             "Review the following HEARTBEAT.md and decide whether to run:\n\n"
                             + content;

    try {
        auto model = ResourceManager::GetInstance().CreateModel(modelConfig_);
        std::string formatted = model->Format(systemPrompt, {{"user", userPrompt}});
        LOG(INFO) << "[HB-CHECK] Decision prompt send to model " << formatted;
        std::string response = model->Invoke(formatted, nullptr);
        LOG(INFO) << "[HB-CHECK] Model response is: " << response;

        std::size_t start = response.find("{");
        std::size_t end = response.find_last_of("}");
        if (start != std::string::npos && end != std::string::npos) {
            std::string jsonStr = response.substr(start, end - start + 1);
            auto j = nlohmann::json::parse(jsonStr, nullptr, false);
            if (!j.is_discarded() && j.count("name")) {
                std::string name = j["name"];
                if (name == "decision") {
                    if (j.count("arguments") && j["arguments"].is_object()) {
                        auto args = j["arguments"];
                        if (args.count("action") && args["action"].is_string()) {
                            std::string action = args["action"];
                            if (args.count("tasks") && args["tasks"].is_string()) {
                                tasksSummary = args["tasks"];
                            }
                            return (action == "run");
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[HeartbeatDecision] Decision error: " << e.what() << std::endl;
    }

    return false;
}

void HeartbeatManager::Run() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(intervalSeconds_), [this]() { return !running_; });
            if (!running_) break;
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream timeSs;
        timeSs << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");

        LOG(INFO) << "[HB-CHECK] Heartbeat check started at " << timeSs.str();

        std::string content = ReadFile();
        if (content.empty()) {
            LOG(INFO) << "[HB-CHECK] HEARTBEAT.md is empty, skipping.";
            continue;
        }

        if (agent_.IsBusy()) {
            LOG(INFO) << "[HB-CHECK] Agent busy, deferring heartbeat, will retry next cycle";
            continue;
        }

        std::string tasks;
        if (DecideAction(content, tasks)) {
            LOG(INFO) << "[HB-TRIGGER] Task triggered: " << tasks;
            std::cout << UTF8ToLocal("\n[Heartbeat] Decision: RUN. Executing task: ") << UTF8ToLocal(tasks) << UTF8ToLocal("...\n") << std::flush;

            std::string heartbeatPrompt = "HEARTBEAT ALERT: You are required to execute the following periodic task right now:\n\n"
                                          "**Task**: " + tasks + "\n\n"
                                          "INSTRUCTIONS:\n"
                                          "1. Execute the task above.\n"
                                          "2. IMMEDIATELY after successful execution, update the heartbeat file:\n"
                                          "   - **IMPORTANT**: If the task is **recurring** or **periodic** (e.g., 'every day', 'every hour'), you must **KEEP IT IN ACTIVE TASKS**. Do NOT move it to 'Completed'.\n"
                                          "   - ONLY move to 'Completed' if the task is explicitly a **one-time** task.\n"
                                          "   - Use your file editing tools to make this update in `./data/HEARTBEAT.md`.";

            std::string fullResponse = agent_.Invoke(heartbeatPrompt, AgentResponseHandler().GetCallback());
            LOG(INFO) << "[HB-COMPLETED] Task execution finished. Response length: " << fullResponse.length();
            std::cout << "\n";
        } else {
            LOG(INFO) << "[HB-SKIP] No tasks to execute or conditions not met.";
            std::cout << UTF8ToLocal("\n[Heartbeat] Decision: ") << UTF8ToLocal("SKIP. No active tasks or conditions not met.\n") << std::flush;
        }
    }
}

} // namespace jiuwenClaw
