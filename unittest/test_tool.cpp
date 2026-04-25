

#include <string>
#include <vector>
#include "include/tool.h"
#include "test_runner.h"

using namespace jiuwen;

class DummyTool : public Tool {
public:
    DummyTool() : Tool("dummy", "A dummy tool for testing",
        {{"input", "Input text", "string", true}, {"count", "Repeat count", "integer", false}}){};

    std::string Invoke(const std::string& input) override
    {
        return "echo: " + input;
    }
};

TEST(tool, ConstructorStoresName)
{
    DummyTool tool;
    TestRunner::AssertEq(tool.GetName(), std::string("dummy"));
}

TEST(tool, ConstructorStoresDescription)
{
    DummyTool tool;
    TestRunner::AssertEq(tool.GetDescription(), std::string("A dummy tool for testing"));
}

TEST(tool, ConstructorStoresParams)
{
    DummyTool tool;
    auto params = tool.GetParams();
    TestRunner::AssertEq(params.size(), size_t(2));
    TestRunner::AssertEq(params[0].name, std::string("input"));
    TestRunner::AssertEq(params[0].type, std::string("string"));
    TestRunner::AssertEq(params[0].required, true);
    TestRunner::AssertEq(params[1].name, std::string("count"));
    TestRunner::AssertEq(params[1].required, false);
}

TEST(tool, GetSchema)
{
    DummyTool tool;
    std::string schema = tool.GetSchema();
    TestRunner::AssertContains(schema, "Tool: dummy");
    TestRunner::AssertContains(schema, "Description: A dummy tool for testing");
    TestRunner::AssertContains(schema, "- input (string) [required]: Input text");
    TestRunner::AssertContains(schema, "- count (integer): Repeat count");
}

TEST(tool, Invoke)
{
    DummyTool tool;
    std::string result = tool.Invoke("hello");
    TestRunner::AssertEq(result, std::string("echo: hello"));
}

TEST(tool, EmptyParamsTool)
{
    class NoParamTool : public Tool {
    public:
        NoParamTool() : Tool("noparams", "No params", {}){} 
        std::string Invoke(const std::string&) override 
        { 
            return "ok"; 
        }
    };
    NoParamTool tool;
    std::string schema = tool.GetSchema();
    TestRunner::AssertContains(schema, "Tool: noparams");
    TestRunner::AssertContains(schema, "Description: No params");
    TestRunner::AssertFalse(schema.find("- ") != std::string::npos, "Should have no param lines");
}
