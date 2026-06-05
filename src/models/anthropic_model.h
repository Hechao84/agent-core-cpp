#pragma once

#include <functional>
#include <string>
#include <vector>

#include "include/model.h"

namespace jiuwen {

class AnthropicModel : public Model {
public:
    explicit AnthropicModel(ModelConfig config) : Model(std::move(config)) {}

    std::string Format(const std::string& systemPrompt,
                       const std::vector<Message>& messages,
                       const std::vector<ToolSchema>& tools) override;

    ModelResponse Invoke(const std::string& formattedInput,
                          std::function<void(const std::string&)> onChunk) override;
};

} // namespace jiuwen