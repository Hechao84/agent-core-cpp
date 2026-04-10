#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "types.h"

struct Message {
    std::string role;
    std::string content;
};

struct ModelResponse {
    std::string content;
    bool isFinished{false};
    std::string finishReason;
};

class AGENT_API Model {
    public:
        Model(ModelConfig config);
        virtual ~Model() = default;
        virtual std::string Format(const std::string& systemPrompt, const std::vector<Message>& messages) = 0;
        virtual std::string Invoke(const std::string& formattedInput, std::function<void(const std::string&)> onChunk) = 0;
        virtual ModelResponse ParseResponse(const std::string& rawResponse) = 0;
        ModelConfig GetConfig() const;
    protected:
        ModelConfig m_config;
};
