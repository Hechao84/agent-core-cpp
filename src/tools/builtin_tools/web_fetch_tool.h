#pragma once
#include "tool.h"
#include <string>

class WebFetcherTool : public Tool {
public:
    WebFetcherTool();
    std::string Invoke(const std::string& input) override;
private:
    std::string FetchUrl(const std::string& url);
};
