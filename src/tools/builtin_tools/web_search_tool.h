#pragma once

#include <string>
#include <vector>
#include "include/tool.h"

namespace jiuwen {

struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

class WebSearchTool : public Tool {
public:
    WebSearchTool();
    std::string Invoke(const std::string& input) override;
private:
    // Returns ok=true with results on success; ok=false with errMsg on failure
    // (including anti-bot challenge pages). Engine name records which backend
    // produced the rows.
    struct EngineOutcome {
        bool ok{false};
        std::string errMsg;
        std::vector<WebSearchResult> rows;
        std::string engine;
    };
    EngineOutcome SearchDuckDuckGo(const std::string& query, int maxResults, int timeoutSec);
    EngineOutcome SearchBing(const std::string& query, int maxResults, int timeoutSec);

    std::string DecodeDDGRedirect(const std::string& href);
    std::string StripTags(const std::string& s);
};

} // namespace jiuwen
