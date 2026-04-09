#include "tools/tool_selector.h"
#include "resource_manager.h"
#include <algorithm>

ToolSelector::ToolSelector(SearchConfig config) : m_config(std::move(config)) {}

void ToolSelector::AddToolToPool(const std::string& toolName) {
    if (std::find(m_toolPool.begin(), m_toolPool.end(), toolName) == m_toolPool.end()) {
        m_toolPool.push_back(toolName);
    }
}

void ToolSelector::RemoveToolFromPool(const std::string& toolName) {
    m_toolPool.erase(std::remove(m_toolPool.begin(), m_toolPool.end(), toolName), m_toolPool.end());
}

std::vector<std::string> ToolSelector::GetToolPool() const { return m_toolPool; }

std::string ToolSelector::SelectTool(const std::string& query, const std::vector<std::string>& availableTools) {
    auto results = SelectTopTools(query, availableTools, 1);
    return results.empty() ? "" : results[0];
}

std::vector<std::string> ToolSelector::SelectTopTools(const std::string& query, const std::vector<std::string>& availableTools, int maxCount) {
    auto scores = ScoreTools(query, availableTools);
    std::sort(scores.begin(), scores.end(), [](const ToolMatchResult& a, const ToolMatchResult& b) {
        return a.score > b.score;
    });
    std::vector<std::string> result;
    for (size_t i = 0; i < scores.size() && static_cast<int>(i) < maxCount; ++i) {
        if (scores[i].score > 0) {
            result.push_back(scores[i].toolName);
        }
    }
    return result;
}

std::vector<ToolMatchResult> ToolSelector::RankTools(const std::string& query, const std::vector<std::string>& availableTools) {
    return ScoreTools(query, availableTools);
}

void ToolSelector::SetSearchConfig(const SearchConfig& config) { m_config = config; }

std::vector<ToolMatchResult> ToolSelector::ScoreTools(const std::string& query, const std::vector<std::string>& availableTools) {
    std::vector<ToolMatchResult> results;
    for (const auto& toolName : availableTools) {
        double keywordScore = CalculateKeywordScore(query, toolName, "Tool: " + toolName);
        double embeddingScore = CalculateEmbeddingScore(query, toolName);
        double finalScore = (keywordScore * m_config.keywordWeight) + (embeddingScore * m_config.embeddingWeight);
        results.push_back({toolName, finalScore, "Combined score"});
    }
    return results;
}

double ToolSelector::CalculateKeywordScore(const std::string& query, const std::string& toolName, const std::string& toolDesc) {
    // TODO: 实现 BM25 算法
    (void)query; (void)toolName; (void)toolDesc;
    return 1.0;
}

double ToolSelector::CalculateEmbeddingScore(const std::string& query, const std::string& toolName) {
    // TODO: 实现基于 Embedding 的向量相似度计算
    (void)query; (void)toolName;
    return 1.0;
}
