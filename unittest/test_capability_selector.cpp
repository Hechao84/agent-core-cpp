// Tests for the V2 CapabilitySelector (round5 §5.4.1 条 10):
//   1. BuildRecallPrompt contains tool/skill name+desc + query + context
//   2. ParseRecallResponse parses valid JSON {"tools":[...],"skills":[...]}
//   3. ParseRecallResponse handles malformed/empty/missing braces → empty
//   4. findRelevant end-to-end with a stub Model: prompt → response → result
//   5. findRelevant on stub model returning empty/error → empty CapabilitySelection
//
// No real LLM call: tests use a stub Model subclass registered with
// ResourceManager::RegisterModel so CapabilitySelector's CreateModel picks
// it up. Pattern mirrors test_resource_manager.cpp's TestModel.

#include <memory>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/resource_manager.h"
#include "include/types.h"
#include "src/core/capability_selector.h"
#include "src/skills/skill_engine.h"
#include "third_party/include/nlohmann/json.hpp"
#include "test_runner.h"

using namespace jiuwen;

namespace {

// Stub Model that returns a pre-set response and captures the prompt passed
// to Format() for inspection. The provider string "test_capability_selector"
// selects this model via ModelConfig::provider.
class StubRecallModel : public Model {
public:
    StubRecallModel(ModelConfig config, std::string responseToReturn,
                    std::string* capturedPrompt = nullptr,
                    std::string finishReason = "stop")
        : Model(std::move(config)),
          responseToReturn_(std::move(responseToReturn)),
          capturedPrompt_(capturedPrompt),
          finishReason_(std::move(finishReason)) {}

    std::string Format(const std::string& systemPrompt,
                       const std::vector<Message>& messages,
                       const std::vector<ToolSchema>&) override
    {
        // Capture the user message (the recall prompt) for inspection.
        if (capturedPrompt_ != nullptr && !messages.empty()) {
            *capturedPrompt_ = messages.front().content;
        }
        return "formatted:" + systemPrompt + ":" + (messages.empty() ? "" : messages.front().content);
    }

    ModelResponse Invoke(const std::string& /*formattedInput*/,
                          std::function<void(const std::string&)> /*onChunk*/,
                          std::function<bool()> /*shouldCancel*/) override
    {
        ModelResponse resp;
        resp.content = responseToReturn_;
        resp.isFinished = true;
        resp.finishReason = finishReason_;
        return resp;
    }

private:
    std::string responseToReturn_;
    std::string* capturedPrompt_;
    std::string finishReason_;
};

// Helper to construct a CapabilitySelector with a stub model registered.
// Returns the CapabilitySelector + the prompt capture buffer.
struct CapabilitySelectorFixture {
    CapabilitySelectorFixture(std::string stubResponse,
                              std::string finishReason = "stop")
    {
        // Register a stub model under a unique provider name so
        // ResourceManager::CreateModel picks it up.
        ModelConfig cfg;
        cfg.provider = "test_capability_selector_provider";
        cfg.formatType = ModelFormatType::OPENAI;
        config_.modelConfig = cfg;
        // Use a fresh SkillEngine (no root dir → empty skill catalog).
        skillEngine_ = std::make_shared<SkillEngine>();
        // Register the model factory. ResourceManager is a singleton; tests
        // that overwrite the same provider are idempotent.
        ResourceManager::GetInstance().RegisterModel(
            cfg.provider,
            [response = std::move(stubResponse),
             capture = &capturedPrompt,
             finish = std::move(finishReason)](const ModelConfig& c) {
                return std::make_unique<StubRecallModel>(c, response, capture, finish);
            });
        selector_ = std::make_unique<CapabilitySelector>(config_, skillEngine_.get());
    }
    AgentConfig config_;
    std::shared_ptr<SkillEngine> skillEngine_;
    std::unique_ptr<CapabilitySelector> selector_;
    std::string capturedPrompt;
};

} // namespace

// --- 1. BuildRecallPrompt contains tool/skill name+desc + query + context ---
TEST(capability_selector, BuildRecallPromptContainsQueryAndContext)
{
    CapabilitySelectorFixture f("{\"tools\": [], \"skills\": []}");
    // Construct sample context (a user message and an assistant reply).
    std::vector<Message> ctx;
    Message u; u.role = "user"; u.content = "How do I fix the bug?";
    Message a; a.role = "assistant"; a.content = "Let me check the file.";
    ctx.push_back(u);
    ctx.push_back(a);
    // Call findRelevant — the stub captures the prompt.
    auto result = f.selector_->findRelevant("fix the bug", ctx);
    // The captured prompt should contain the query and context messages.
    TestRunner::AssertTrue(f.capturedPrompt.find("fix the bug") != std::string::npos,
                           "Prompt should contain the raw query");
    TestRunner::AssertTrue(f.capturedPrompt.find("How do I fix the bug?") != std::string::npos,
                           "Prompt should contain the user context message");
    TestRunner::AssertTrue(f.capturedPrompt.find("Let me check the file.") != std::string::npos,
                           "Prompt should contain the assistant context message");
    TestRunner::AssertTrue(f.capturedPrompt.find("Available Tools") != std::string::npos,
                           "Prompt should contain the tool catalog header");
    TestRunner::AssertTrue(f.capturedPrompt.find("Output Format") != std::string::npos,
                           "Prompt should specify the output format");
    // Result should be empty (stub returned empty tools/skills).
    TestRunner::AssertTrue(result.tools.empty(), "Empty stub response → empty tools");
    TestRunner::AssertTrue(result.skills.empty(), "Empty stub response → empty skills");
}

// --- 2. ParseRecallResponse parses valid JSON ---
TEST(capability_selector, ParseValidJson)
{
    // Use a stub response with specific tool/skill names.
    CapabilitySelectorFixture f("{\"tools\": [\"read_file\", \"exec\"], \"skills\": [\"debugging\"]}");
    auto result = f.selector_->findRelevant("anything", {});
    TestRunner::AssertEq(result.tools.size(), size_t(2), "Parsed 2 tools");
    TestRunner::AssertEq(result.tools[0], std::string("read_file"), "First tool is read_file");
    TestRunner::AssertEq(result.tools[1], std::string("exec"), "Second tool is exec");
    TestRunner::AssertEq(result.skills.size(), size_t(1), "Parsed 1 skill");
    TestRunner::AssertEq(result.skills[0], std::string("debugging"), "First skill is debugging");
}

// --- 2b. ParseRecallResponse handles JSON wrapped in markdown fences ---
TEST(capability_selector, ParseJsonWithMarkdownFence)
{
    CapabilitySelectorFixture f("```json\n{\"tools\": [\"a\"], \"skills\": []}\n```");
    auto result = f.selector_->findRelevant("anything", {});
    TestRunner::AssertEq(result.tools.size(), size_t(1), "Parsed 1 tool despite fence");
    TestRunner::AssertEq(result.tools[0], std::string("a"), "Tool name is 'a'");
}

// --- 3. ParseRecallResponse handles malformed/empty responses ---
TEST(capability_selector, ParseMalformedResponse)
{
    // Empty response.
    {
        CapabilitySelectorFixture f("");
        auto result = f.selector_->findRelevant("anything", {});
        TestRunner::AssertTrue(result.tools.empty(), "Empty response → empty result");
        TestRunner::AssertTrue(result.skills.empty(), "Empty response → empty skills");
    }
    // Non-JSON text.
    {
        CapabilitySelectorFixture f("This is not JSON at all.");
        auto result = f.selector_->findRelevant("anything", {});
        TestRunner::AssertTrue(result.tools.empty(), "Non-JSON → empty result");
    }
    // Missing braces.
    {
        CapabilitySelectorFixture f("\"tools\": [\"a\"]");
        auto result = f.selector_->findRelevant("anything", {});
        TestRunner::AssertTrue(result.tools.empty(), "Missing braces → empty result");
    }
    // Valid JSON but missing tools/skills keys → empty result, no crash.
    {
        CapabilitySelectorFixture f("{\"foo\": \"bar\"}");
        auto result = f.selector_->findRelevant("anything", {});
        TestRunner::AssertTrue(result.tools.empty(), "No tools key → empty tools");
        TestRunner::AssertTrue(result.skills.empty(), "No skills key → empty skills");
    }
    // tools array with non-string elements → skipped, no crash.
    {
        CapabilitySelectorFixture f("{\"tools\": [123, null, \"valid\"], \"skills\": [456]}");
        auto result = f.selector_->findRelevant("anything", {});
        TestRunner::AssertEq(result.tools.size(), size_t(1), "Only string elements parsed");
        TestRunner::AssertEq(result.tools[0], std::string("valid"), "valid string preserved");
        TestRunner::AssertTrue(result.skills.empty(), "Non-string skills skipped");
    }
}

// --- 4. findRelevant on stub model returning error → empty result ---
TEST(capability_selector, ModelErrorReturnsEmpty)
{
    CapabilitySelectorFixture f("irrelevant response", "error");
    auto result = f.selector_->findRelevant("anything", {});
    TestRunner::AssertTrue(result.tools.empty(), "Model error → empty tools");
    TestRunner::AssertTrue(result.skills.empty(), "Model error → empty skills");
}

// --- 5. ParseRecallResponse with extra prose around the JSON ---
TEST(capability_selector, ParseJsonWithProseAround)
{
    CapabilitySelectorFixture f(
        "Here are the relevant tools:\n{\"tools\": [\"a\"], \"skills\": [\"b\"]}\nThat's all.");
    auto result = f.selector_->findRelevant("anything", {});
    TestRunner::AssertEq(result.tools.size(), size_t(1), "Parsed 1 tool with prose around");
    TestRunner::AssertEq(result.tools[0], std::string("a"), "Tool is 'a'");
    TestRunner::AssertEq(result.skills.size(), size_t(1), "Parsed 1 skill with prose around");
    TestRunner::AssertEq(result.skills[0], std::string("b"), "Skill is 'b'");
}
