#include "src/utils/tool_parser.h"

namespace jiuwen {

std::string ExtractJson(const std::string& text, size_t startPos)
{
    if (startPos >= text.length()) return "";
    if (text[startPos] != '{') return "";

    int depth = 0;
    bool inString = false;
    for (size_t i = startPos; i < text.length(); ++i) {
        if (text[i] == '\\') {
            i++;
            continue;
        }
        if (text[i] == '"') inString = !inString;
        if (!inString) {
            if (text[i] == '{') {
                depth++;
            } else if (text[i] == '}') {
                depth--;
                if (depth == 0) return text.substr(startPos, i - startPos + 1);
            }
        }
    }
    return "";
}

std::vector<ParsedToolCall> ExtractAllToolCalls(const std::string& response)
{
    std::vector<ParsedToolCall> calls;
    size_t searchPos = 0;

    while (searchPos < response.length()) {
        size_t jsonStart = response.find('{', searchPos);
        if (jsonStart == std::string::npos) break;

        std::string jsonStr = ExtractJson(response, jsonStart);
        if (jsonStr.empty()) {
            searchPos = jsonStart + 1;
            continue;
        }

        size_t nameKey = jsonStr.find("\"name\"");
        size_t argsKey = jsonStr.find("\"arguments\"");
        if (nameKey != std::string::npos && argsKey != std::string::npos) {
            ParsedToolCall call;
            call.arguments = "{}";

            // Extract name value
            size_t colon = jsonStr.find(':', nameKey + 6);
            size_t valStart = jsonStr.find_first_not_of(" \t", colon + 1);
            if (valStart != std::string::npos && jsonStr[valStart] == '"') {
                size_t valEnd = jsonStr.find('"', valStart + 1);
                if (valEnd != std::string::npos) {
                    call.name = jsonStr.substr(valStart + 1, valEnd - valStart - 1);
                }
            }

            // Extract arguments value
            colon = jsonStr.find(':', argsKey + 11);
            valStart = jsonStr.find_first_not_of(" \t", colon + 1);
            if (valStart != std::string::npos && valStart < jsonStr.length()) {
                if (jsonStr[valStart] == '{') {
                    std::string argsObj = ExtractJson(jsonStr, valStart);
                    if (!argsObj.empty()) {
                        call.arguments = argsObj;
                    }
                } else if (jsonStr[valStart] == '"') {
                    size_t aEnd = jsonStr.find('"', valStart + 1);
                    if (aEnd != std::string::npos) {
                        call.arguments = "\"" + jsonStr.substr(valStart + 1, aEnd - valStart - 1) + "\"";
                    }
                }
            }

            // Validate name
            if (!call.name.empty() && call.name.length() < 50 &&
                call.name.find('{') == std::string::npos &&
                call.name.find('}') == std::string::npos) {
                calls.push_back(call);
            }
        }

        searchPos = jsonStart + jsonStr.length();
    }

    return calls;
}

std::string ParseAction(const std::string& response, std::string& actionInput)
{
    actionInput = "{}";

    // Strategy 1: Try JSON format {"name": "...", "arguments": ...}
    size_t jsonStart = response.find('{');
    if (jsonStart != std::string::npos) {
        std::string jsonStr = ExtractJson(response, jsonStart);
        if (!jsonStr.empty()) {
            size_t nameKey = jsonStr.find("\"name\"");
            if (nameKey != std::string::npos) {
                size_t colon = jsonStr.find(':', nameKey + 6);
                size_t valStart = jsonStr.find_first_not_of(" \t", colon + 1);
                if (valStart != std::string::npos && jsonStr[valStart] == '"') {
                    size_t valEnd = jsonStr.find('"', valStart + 1);
                    if (valEnd != std::string::npos) {
                        std::string name = jsonStr.substr(valStart + 1, valEnd - valStart - 1);
                        if (!name.empty() && name.length() < 50 &&
                            name.find('{') == std::string::npos &&
                            name.find('}') == std::string::npos) {
                            size_t argsKey = jsonStr.find("\"arguments\"");
                            if (argsKey != std::string::npos) {
                                size_t aColon = jsonStr.find(':', argsKey + 11);
                                size_t aValStart = jsonStr.find_first_not_of(" \t", aColon + 1);
                                if (aValStart != std::string::npos && aValStart < jsonStr.length()) {
                                    if (jsonStr[aValStart] == '{') {
                                        std::string argsObj = ExtractJson(jsonStr, aValStart);
                                        if (!argsObj.empty()) {
                                            actionInput = argsObj;
                                        }
                                    } else if (jsonStr[aValStart] == '"') {
                                        size_t aEnd = jsonStr.find('"', aValStart + 1);
                                        if (aEnd != std::string::npos) {
                                            actionInput = "\"" + jsonStr.substr(aValStart + 1, aEnd - aValStart - 1) + "\"";
                                        }
                                    }
                                }
                            }
                            return name;
                        }
                    }
                }
            }
        }
    }

    // Strategy 2: Classic ReAct format
    size_t actPos = response.find("Action:");
    if (actPos != std::string::npos) {
        size_t end = response.find('\n', actPos);
        std::string actLine = response.substr(actPos + 7, end - actPos - 7);

        size_t f = actLine.find_first_not_of(" \t\r\n");
        if (f != std::string::npos) actLine.erase(0, f);
        size_t l = actLine.find_last_not_of(" \t\r\n");
        if (l != std::string::npos) actLine.erase(l + 1);

        // If Action line is a JSON, recurse
        if (!actLine.empty() && actLine.front() == '{') {
            std::string multiLine = response.substr(actPos);
            return ParseAction(multiLine, actionInput);
        }

        std::string name = actLine;
        if (!name.empty() && name.front() == '{') {
            size_t braceEnd = name.find('}');
            if (braceEnd != std::string::npos) name.erase(0, braceEnd + 1);
        }
        if (!name.empty() && name.back() == '}') {
            size_t braceStart = name.rfind('{');
            if (braceStart != std::string::npos) name.erase(braceStart);
        }
        f = name.find_first_not_of(" \t\r\n");
        if (f != std::string::npos) name.erase(0, f);
        l = name.find_last_not_of(" \t\r\n");
        if (l != std::string::npos) name.erase(l + 1);

        size_t inputPos = response.find("Action Input:");
        if (inputPos != std::string::npos) {
            size_t inputEnd = response.find('\n', inputPos);
            std::string input = response.substr(inputPos + 13, inputEnd - inputPos - 13);
            f = input.find_first_not_of(" \t\r\n");
            if (f != std::string::npos) input.erase(0, f);
            l = input.find_last_not_of(" \t\r\n");
            if (l != std::string::npos) input.erase(l + 1);
            if (!input.empty()) {
                if (input.front() == '{') actionInput = input;
                else actionInput = "{\"input\": \"" + input + "\"}";
            }
        }

        return name;
    }

    return "";
}

std::string TrimStr(const std::string& str)
{
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

} // namespace jiuwen
