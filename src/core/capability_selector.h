#pragma once

#include <string>
#include <vector>

#include "include/model.h"
#include "include/types.h"

namespace jiuwen {

class SkillEngine;

// V2 LLM-backed capability recall (round5 §5.4.1 条 10). Replaces the
// deprecated ToolSelector (whose selection methods were stubs returning 1.0
// and whose data channel only held names, never descriptions). Pool source:
// ResourceManager for tools, SkillEngine for skills — fetched by name at
// recall time, not held as a stale copy (the design rationale: avoid
// re-introducing toolPool_'s data channel error).
//
// One-shot per turn-start: CapabilitySelector::findRelevant is called once
// at the start of ReactAgentWorker::Invoke (under SELECTIVE mode only) to
// seed the active sets. Turn-mid search actions (tool_search/skill_search's
// search) also call findRelevant with empty sessionContext — the model's
// search query is already context-aware (§5.4.1 条 7: turn-mid search query
// carries context the model already has).
//
// LLM backend: reuses the main modelConfig (§5.4.1 条 9: V2 does not
// introduce a separate recallModelConfig). Future evolution (§9 v3) may swap
// to an embedding backend; the interface stays the same.
//
// Failure modes (§5.4.1 条 6 降级判定表): on JSON parse failure, empty
// response, or LLM call exception, returns an empty CapabilitySelection
// (both vectors empty). The caller (Invoke entry) treats empty result as a
// 降级 trigger and seeds the active set with the full pool (退化 progressive).
struct CapabilitySelection {
    std::vector<std::string> tools;   // tool names to seed into activeSet
    std::vector<std::string> skills;  // skill names to seed into skillActiveSet
};

class CapabilitySelector {
public:
    // skillEngine may be nullptr when no skills are configured; in that case
    // skills will be an empty vector in the result.
    CapabilitySelector(AgentConfig config, SkillEngine* skillEngine = nullptr);

    // Recall relevant capabilities for the given query. sessionContext is the
    // windowed conversation history (from ContextEngine::GetContextWindow());
    // turn-mid search callers may pass an empty vector since the model's
    // search query already carries context.
    //
    // Returns CapabilitySelection with .tools and .skills populated. On any
    // failure (parse error, LLM exception, empty response), returns empty
    // CapabilitySelection (caller treats as 降级 trigger).
    CapabilitySelection findRelevant(const std::string& rawQuery,
                                      const std::vector<Message>& sessionContext);

private:
    AgentConfig config_;
    SkillEngine* skillEngine_;

    // Build the recall prompt text from the available tool/skill catalogs and
    // the conversation context. Exposed as a separate method so tests can
    // verify the prompt content without invoking the LLM.
    std::string BuildRecallPrompt(const std::string& rawQuery,
                                  const std::vector<Message>& sessionContext) const;

    // Parse the LLM response text into CapabilitySelection. Returns empty
    // selection on any parse failure (caller treats as 降级). Exposed as a
    // separate method so tests can verify the parser directly.
    static CapabilitySelection ParseRecallResponse(const std::string& response);
};

} // namespace jiuwen
