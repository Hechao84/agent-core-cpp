

#include <string>
#include <vector>
#include "include/model.h"

class OpenAIModel : public Model {public:
    OpenAIModel(ModelConfig config) : Model(std::move(config)){} 
    std::string Format(const std::string& systemPrompt, const std::vector<Message>& messages) override;
    std::string Invoke(const std::string& formattedInput, std::function<void(const std::string&)> onChunk) override;
    ModelResponse ParseResponse(const std::string& rawResponse) override;
};
