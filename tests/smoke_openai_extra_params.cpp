// Verify OpenAIModel forwards common request fields from
// ModelConfig::extraParams into the wire payload.
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"
#include "src/models/openai_model.h"
#include "third_party/include/nlohmann/json.hpp"

using namespace jiuwen;
using json = nlohmann::json;

static void Check(bool cond, const std::string& tag)
{
    if (!cond) { std::cerr << "[FAIL] " << tag << "\n"; std::exit(1); }
    std::cout << "[OK]   " << tag << "\n";
}

int main()
{
    ModelConfig cfg;
    cfg.modelName = "ark-code-latest";
    cfg.baseUrl = "https://example/v1";
    cfg.useNativeFunctionCalling = true;
    cfg.extraParams.Set("max_tokens", 8192);
    cfg.extraParams.Set("temperature", 0.2f);
    cfg.extraParams.Set("top_p", 0.9f);
    cfg.extraParams.Set("presence_penalty", 0.1f);
    cfg.extraParams.Set("frequency_penalty", 0.05f);
    cfg.extraParams.Set("seed", 42);

    OpenAIModel model(cfg);

    Message u; u.role = "user"; u.content = "ping";
    auto payload = json::parse(model.Format("sys", {u}, {}));

    Check(payload.value("max_tokens", 0) == 8192, "max_tokens forwarded");
    Check(std::abs(payload.value("temperature", 0.0f) - 0.2f) < 1e-5, "temperature forwarded");
    Check(std::abs(payload.value("top_p", 0.0f) - 0.9f) < 1e-5, "top_p forwarded");
    Check(std::abs(payload.value("presence_penalty", 0.0f) - 0.1f) < 1e-5, "presence_penalty forwarded");
    Check(std::abs(payload.value("frequency_penalty", 0.0f) - 0.05f) < 1e-5, "frequency_penalty forwarded");
    Check(payload.value("seed", 0) == 42, "seed forwarded");

    // Absent keys must not appear
    ModelConfig empty;
    empty.modelName = "x";
    empty.baseUrl = "http://x";
    OpenAIModel m2(empty);
    auto p2 = json::parse(m2.Format("sys", {u}, {}));
    Check(!p2.contains("max_tokens"), "absent: no max_tokens");
    Check(!p2.contains("temperature"), "absent: no temperature");
    Check(!p2.contains("top_p"), "absent: no top_p");
    Check(!p2.contains("seed"), "absent: no seed");

    std::cout << "\nAll extraParams checks passed.\n";
    return 0;
}
