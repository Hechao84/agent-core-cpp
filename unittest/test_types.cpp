#include "test_runner.h"
#include "types.h"
#include <memory>
#include <vector>
#include <string>

// ========================================
// ConfigNode Tests
// ========================================

TEST(types, SetAndGetInt) {
    ConfigNode node;
    node.Set("count", 42);
    const int* val = node.GetPtr<int>("count");
    TestRunner::AssertTrue(val != nullptr, "GetPtr should return valid pointer");
    TestRunner::AssertEq(*val, 42);
}

TEST(types, SetAndGetFloat) {
    ConfigNode node;
    node.Set("temperature", 0.7f);
    const float* val = node.GetPtr<float>("temperature");
    TestRunner::AssertTrue(val != nullptr);
    TestRunner::AssertEq(*val, 0.7f);
}

TEST(types, SetAndGetBool) {
    ConfigNode node;
    node.Set("stream", true);
    const bool* val = node.GetPtr<bool>("stream");
    TestRunner::AssertTrue(val != nullptr);
    TestRunner::AssertEq(*val, true);
}

TEST(types, SetAndGetString) {
    ConfigNode node;
    node.Set("name", std::string("test_model"));
    const std::string* val = node.GetPtr<std::string>("name");
    TestRunner::AssertTrue(val != nullptr);
    TestRunner::AssertEq(*val, std::string("test_model"));
}

TEST(types, SetAndGetVector) {
    ConfigNode node;
    node.Set("tags", std::vector<std::string>{"ai", "ml", "nlp"});
    const std::vector<std::string>* val = node.GetPtr<std::vector<std::string>>("tags");
    TestRunner::AssertTrue(val != nullptr);
    TestRunner::AssertEq(val->size(), size_t(3));
    TestRunner::AssertEq((*val)[0], std::string("ai"));
}

TEST(types, GetValueWithDefault) {
    ConfigNode node;
    int val = node.GetValue<int>("missing", 999);
    TestRunner::AssertEq(val, 999);
    node.Set("existing", 42);
    TestRunner::AssertEq(node.GetValue<int>("existing", 0), 42);
}

TEST(types, GetPtrReturnsNullForMissingKey) {
    ConfigNode node;
    const int* val = node.GetPtr<int>("nonexistent");
    TestRunner::AssertTrue(val == nullptr);
}

TEST(types, GetPtrReturnsNullForTypeMismatch) {
    ConfigNode node;
    node.Set("name", std::string("test"));
    const int* val = node.GetPtr<int>("name");
    TestRunner::AssertTrue(val == nullptr, "Should return nullptr for type mismatch");
}

TEST(types, SetNestedTwoLevel) {
    ConfigNode node;
    node.SetNested("model.temperature", 0.5f);
    auto subNode = node.GetPtr<std::shared_ptr<ConfigNode>>("model");
    TestRunner::AssertTrue(subNode != nullptr);
    const float* val = (*subNode)->GetPtr<float>("temperature");
    TestRunner::AssertTrue(val != nullptr);
    TestRunner::AssertEq(*val, 0.5f);
}

TEST(types, SetNestedThreeLevel) {
    ConfigNode node;
    node.SetNested("model.params.temperature", 0.7f);
    auto modelNode = node.GetPtr<std::shared_ptr<ConfigNode>>("model");
    TestRunner::AssertTrue(modelNode != nullptr);
    auto paramsNode = (*modelNode)->GetPtr<std::shared_ptr<ConfigNode>>("params");
    TestRunner::AssertTrue(paramsNode != nullptr);
    const float* val = (*paramsNode)->GetPtr<float>("temperature");
    TestRunner::AssertTrue(val != nullptr);
    TestRunner::AssertEq(*val, 0.7f);
}

TEST(types, SetNestedOverwrites) {
    ConfigNode node;
    node.SetNested("model.temperature", 0.5f);
    node.SetNested("model.temperature", 0.8f);
    auto subNode = node.GetPtr<std::shared_ptr<ConfigNode>>("model");
    const float* val = (*subNode)->GetPtr<float>("temperature");
    TestRunner::AssertEq(*val, 0.8f);
}

TEST(types, ModelConfigDefaults) {
    ModelConfig cfg;
    TestRunner::AssertTrue(cfg.formatType == ModelFormatType::OPENAI);
    TestRunner::AssertTrue(cfg.baseUrl.empty());
    TestRunner::AssertTrue(cfg.apiKey.empty());
}

TEST(types, ContextConfigDefaults) {
    ContextConfig cfg;
    TestRunner::AssertEq(cfg.maxContextTokens, 4096);
    TestRunner::AssertTrue(cfg.storageType == ContextConfig::StorageType::MARKDOWN_FILE);
    TestRunner::AssertTrue(cfg.sessionId.empty());
}
