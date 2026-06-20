
#include <iostream>
#include <string>
#include "include/resource_manager.h"

using namespace jiuwen;

int main(int argc, char** argv)
{
    std::string query = argc > 1 ? argv[1] : "openai gpt-5 release";
    auto tool = ResourceManager::GetInstance().CreateTool("web_search");
    std::string out = tool->Invoke("{\"query\": \"" + query + "\", \"max_results\": 3}");
    std::cout << "==== output ====\n" << out << "\n==== end ====" << std::endl;
    return 0;
}
