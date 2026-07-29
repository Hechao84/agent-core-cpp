#pragma once

#include <string>
#include "include/tool.h"

namespace jiuwen {

class WebFetcherTool : public Tool {
public:
    WebFetcherTool();
    std::string Invoke(const std::string& input) override;
    // Parse the tool input into the URL to fetch. Accepts either a bare URL
    // or a JSON object {"url": "..."}. Extracted as a separate method so it
    // can be unit-tested without going to the network.
    static std::string ExtractUrl(const std::string& input);
private:
    std::string FetchUrl(const std::string& url);
};

} // namespace jiuwen
