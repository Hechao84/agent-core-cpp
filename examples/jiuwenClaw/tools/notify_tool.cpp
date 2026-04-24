#include "notify_tool.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include "src/utils/logger.h"
#include "src/3rd-party/include/nlohmann/json.hpp"

NotifyTool::NotifyTool() : Tool("notify", "Send a system notification to the user. Use this to display desktop notifications, popup messages, or alerts across all platforms (Windows, Linux, WSL).", {
    ToolParam{"title", "Notification title/header", "string", true},
    ToolParam{"message", "Notification body/message content", "string", true}
})
{
}

std::string CreateWslNotificationCommand(const std::string& title, const std::string& message) {
    std::string cmd = "powershell.exe -WindowStyle Hidden -ExecutionPolicy Bypass -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "Add-Type -AssemblyName System.Drawing; "
        "\\$n = New-Object System.Windows.Forms.NotifyIcon; "
        "\\$n.Icon = [System.Drawing.SystemIcons]::Information; "
        "\\$n.Visible = \\$true; "
        "\\$n.ShowBalloonTip(5000, '" + title + "', '" + message + "', "
        "[System.Windows.Forms.ToolTipIcon]::Info); Start-Sleep -Seconds 6; "
        "\\$n.Dispose()\"";
    return cmd;
}

std::string CreateWindowsNotificationCommand(const std::string& title, const std::string& message) {
    std::string cmd = "powershell.exe -NoProfile -WindowStyle Hidden -Command "
                "\"Add-Type -AssemblyName System.Windows.Forms; "
                "$obj = New-Object System.Windows.Forms.NotifyIcon; "
                "$obj.Text = '" + title + "'; "
                "$obj.Visible = $true; "
                "$obj.ShowBalloonTip(5000, '" + title + "', '" + message + "', [System.Windows.Forms.ToolTipIcon]::Info); "
                "Start-Sleep -Seconds 6; "
                "$obj.Visible = $false\"";
    return cmd;
}

void NotifyTool::SendNotification(const std::string& title, const std::string& message) const
{
    std::thread([title, message]() {
        bool isWsl = (std::system("grep -qi microsoft /proc/version 2>/dev/null") == 0);
        bool isWinNative = !isWsl && (std::system("where powershell.exe > /dev/null 2>&1") == 0);

        if (isWsl) {
            std::string cmd = CreateWslNotificationCommand(title, message);
            LOG(INFO) << "[NotifyTool] Sending WSL -> Windows notification, cmd is " << cmd;
            int ret = std::system(("nohup " + cmd + " </dev/null >/dev/null 2>&1 &").c_str());
            (void)ret;
        } else if (isWinNative) {
            std::string cmd = CreateWindowsNotificationCommand(title, message);
            LOG(INFO) << "[NotifyTool] Sending Windows notification, cmd is " << cmd;
            int ret = std::system(("start /B " + cmd + " > nul 2>&1").c_str());
            (void)ret;
        } else {            
            bool hasNotify = (std::system("which notify-send > /dev/null 2>&1") == 0);
            if (hasNotify) {
                int ret = std::system(("notify-send '" + title + "' '" + message + "' &").c_str());
                (void)ret;
            } else {
                std::cerr << "\a[ALERT] " << title << ": " << message << std::endl;
            }
        }
    }).detach();
}

std::string NotifyTool::Invoke(const std::string& input)
{
    std::string title;
    std::string message;

    try {
        nlohmann::json j = nlohmann::json::parse(input);
        title = j.value("title", "");
        message = j.value("message", "");
    } catch (const std::exception& e) {
        return "Error: Invalid JSON input. Expected: {\"title\": \"...\", \"message\": \"...\"}\nDetails: " + std::string(e.what());
    }

    if (title.empty()) {
        return "Error: 'title' parameter is required.";
    }
    if (message.empty()) {
        return "Error: 'message' parameter is required.";
    }

    SendNotification(title, message);
    return "Notification sent: \"" + title + "\" - \"" + message + "\"";
}
