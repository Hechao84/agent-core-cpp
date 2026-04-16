#pragma once

#include <string>
#include "include/tool.h"

class WebFetcherTool : public Tool {
public:
    WebFetcherTool();
    std::string Invoke(const std::string& input) override;
private:
    std::string FetchUrl(const std::string& url);
};
