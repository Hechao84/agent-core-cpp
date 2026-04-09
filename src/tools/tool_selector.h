#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>

struct ToolMatchResult {
    std::string toolName;
    double score;
    std::string reason;
};

struct SearchConfig {
    double keywordWeight{0.5};
    double embeddingWeight{0.5};
    std::string embeddingModelName;
};

class ToolSelector {
    public:
        ToolSelector(SearchConfig config = {});
        void AddToolToPool(const std::string& toolName);
        void RemoveToolFromPool(const std::string& toolName);
        std::vector<std::string> GetToolPool() const;
        std::string SelectTool(const std::string& query, const std::vector<std::string>& availableTools);
        std::vector<std::string> SelectTopTools(const std::string& query, const std::vector<std::string>& availableTools, int maxCount = 3);
        std::vector<ToolMatchResult> RankTools(const std::string& query, const std::vector<std::string>& availableTools);
        void SetSearchConfig(const SearchConfig& config);
    private:
        SearchConfig m_config;
        std::vector<std::string> m_toolPool;
        std::function<std::string(const std::string&, const std::vector<std::string>&)> m_selectionStrategy;
        
        std::vector<ToolMatchResult> ScoreTools(const std::string& query, const std::vector<std::string>& availableTools);
        double CalculateKeywordScore(const std::string& query, const std::string& toolName, const std::string& toolDesc);
        double CalculateEmbeddingScore(const std::string& query, const std::string& toolName);
};
