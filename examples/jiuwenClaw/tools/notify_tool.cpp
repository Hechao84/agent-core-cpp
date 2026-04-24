#include "notify_tool.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "src/utils/logger.h"
#include "src/3rd-party/include/nlohmann/json.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
    #include <wincrypt.h>
    #pragma comment(lib, "Crypt32.lib")
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kBalloonTipTimeoutMs = 5000;
constexpr int kDisposeDelaySeconds = 6;
constexpr int kIconLoadDelayMs = 200;
constexpr const char* kPowerShellExePath = R"(C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe)";

std::string EscapePowerShellString(const std::string& input)
{
    std::string result;
    result.reserve(input.size());
    for (char ch : input) {
        if (ch == '\'') {
            result += "''";
        } else {
            result += ch;
        }
    }
    return result;
}

// Build PowerShell command for WSL environment
std::string CreateWslNotificationCommand(const std::string& title, const std::string& message)
{
    std::string escapedTitle = EscapePowerShellString(title);
    std::string escapedMsg = EscapePowerShellString(message);

    std::string cmd = "powershell.exe -WindowStyle Hidden -ExecutionPolicy Bypass -Command "
        "\"Add-Type -AssemblyName System.Windows.Forms; "
        "Add-Type -AssemblyName System.Drawing; "
        "\\$n = New-Object System.Windows.Forms.NotifyIcon; "
        "\\$n.Icon = [System.Drawing.SystemIcons]::Information; "
        "\\$n.Visible = \\$true; "
        "\\$n.ShowBalloonTip(" + std::to_string(kBalloonTipTimeoutMs) + ", '" +
        escapedTitle + "', '" + escapedMsg + "', "
        "[System.Windows.Forms.ToolTipIcon]::Info); Start-Sleep -Seconds " +
        std::to_string(kDisposeDelaySeconds) + "; \\$n.Dispose()\"";
    return cmd;
}

#ifdef _WIN32
// Encode UTF-8 script content to Base64 (UTF-16 LE) for PowerShell -EncodedCommand
std::string EncodeScriptToBase64(const std::string& script)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, script.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        return "";
    }

    std::vector<wchar_t> wideBuf(len);
    MultiByteToWideChar(CP_UTF8, 0, script.c_str(), -1, &wideBuf[0], len);

    DWORD base64Len = 0;
    CryptBinaryToStringA(
        reinterpret_cast<const BYTE*>(&wideBuf[0]),
        (len - 1) * sizeof(wchar_t),
        CRYPT_STRING_BASE64,
        nullptr,
        &base64Len);

    std::string base64Encoded(base64Len, '\0');
    CryptBinaryToStringA(
        reinterpret_cast<const BYTE*>(&wideBuf[0]),
        (len - 1) * sizeof(wchar_t),
        CRYPT_STRING_BASE64,
        &base64Encoded[0],
        &base64Len);

    base64Encoded.resize(std::strlen(base64Encoded.c_str()));
    return base64Encoded;
}

// Build PowerShell script content for native Windows notification
std::string BuildWinNotificationScript(const std::string& title, const std::string& message)
{
    std::string escapedTitle = EscapePowerShellString(title);
    std::string escapedMsg = EscapePowerShellString(message);

    return
        "$path = 'C:\\Windows\\System32\\Shell32.dll'; "
        "Add-Type -AssemblyName System.Windows.Forms; "
        "Add-Type -AssemblyName System.Drawing; "
        "$icon = [System.Drawing.Icon]::ExtractAssociatedIcon($path); "
        "$obj = New-Object System.Windows.Forms.NotifyIcon; "
        "$obj.Icon = $icon; "
        "$obj.Text = '" + escapedTitle + "'; "
        "$obj.Visible = $true; "
        "Start-Sleep -Milliseconds " + std::to_string(kIconLoadDelayMs) + "; "
        "$obj.ShowBalloonTip(" + std::to_string(kBalloonTipTimeoutMs) + ", '" +
        escapedTitle + "', '" + escapedMsg + "', "
        "[System.Windows.Forms.ToolTipIcon]::Info); "
        "Start-Sleep -Seconds " + std::to_string(kDisposeDelaySeconds) + "; "
        "$obj.Dispose()";
}
#endif

} // namespace

NotifyTool::NotifyTool()
    : Tool("notify",
           "Send a system notification to the user. Use this to display desktop notifications, "
           "popup messages, or alerts across all platforms (Windows, Linux, WSL).",
           {
               ToolParam{"title", "Notification title/header", "string", true},
               ToolParam{"message", "Notification body/message content", "string", true}
           })
{
}

void NotifyTool::SendNotification(const std::string& title, const std::string& message) const
{
    std::thread([title, message]() {
        try {
            bool isWsl = (std::system("grep -qi microsoft /proc/version 2>/dev/null") == 0);
            bool isWinNative = !isWsl && fs::exists(kPowerShellExePath);

            if (isWsl) {
                std::string cmd = CreateWslNotificationCommand(title, message);
                int ret = std::system(("nohup " + cmd + " </dev/null >/dev/null 2>&1 &").c_str());
                (void)ret;
            } else if (isWinNative) {
#ifdef _WIN32
                std::string script = BuildWinNotificationScript(title, message);
                std::string encodedCmd = EncodeScriptToBase64(script);
                if (encodedCmd.empty()) {
                    LOG(ERR) << "[NotifyTool] Failed to encode PowerShell command";
                    return;
                }

                std::string params =
                    "-NoProfile -WindowStyle Hidden -EncodedCommand \"" + encodedCmd + "\"";
                ShellExecuteA(nullptr, nullptr, "powershell.exe", params.c_str(), nullptr,
                              SW_HIDE);
#endif
            } else {
                bool hasNotify =
                    (std::system("which notify-send > /dev/null 2>&1") == 0);
                if (hasNotify) {
                    std::string cmd =
                        "notify-send '" + title + "' '" + message + "' &";
                    int ret = std::system(cmd.c_str());
                    (void)ret;
                } else {
                    std::cerr << "\a[ALERT] " << title << ": " << message << std::endl;
                }
            }
        } catch (const std::exception& e) {
            LOG(ERR) << "[NotifyTool] Failed to send notification: " << e.what();
        } catch (...) {
            LOG(ERR) << "[NotifyTool] Unknown error sending notification";
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
        return "Error: Invalid JSON input. Expected: {\"title\": \"...\", \"message\": "
               "\"...\"}\nDetails: " +
               std::string(e.what());
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
