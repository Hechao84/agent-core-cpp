#include "context_engine.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

// Base implementation providing common functionality
class ContextEngineBase : public ContextEngine {
protected:
    ContextConfig m_config;
    std::vector<Message> m_messages;
    
    // Estimate token count (rough approximation: 1 token ~ 4 chars for English)
    static int EstimateTokens(const std::string& text) {
        return static_cast<int>(text.length()) / 4;
    }
    
public:
    bool Initialize(const ContextConfig& config) override {
        m_config = config;
        return true;
    }
    
    bool AppendMessage(const Message& message) override {
        m_messages.push_back(message);
        return PersistMessage(message);
    }
    
    std::vector<Message> GetContextWindow() const override {
        if (m_messages.empty()) return {};
        
        std::vector<Message> window;
        int totalTokens = 0;
        
        // Start from the end and work backwards to get the most recent messages
        for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
            int msgTokens = EstimateTokens(it->content) + EstimateTokens(it->role);
            if (totalTokens + msgTokens > m_config.maxContextTokens && !window.empty()) {
                break;
            }
            window.insert(window.begin(), *it);
            totalTokens += msgTokens;
        }
        
        return window;
    }
    
    std::vector<Message> GetAllMessages() const override {
        return m_messages;
    }
    
    void Clear() override {
        m_messages.clear();
        ClearPersistentStorage();
    }
    
    int GetCurrentTokenCount() const override {
        int total = 0;
        for (const auto& msg : m_messages) {
            total += EstimateTokens(msg.content) + EstimateTokens(msg.role);
        }
        return total;
    }
    
    std::string GetSessionId() const override {
        return m_config.sessionId;
    }
    
protected:
    virtual bool PersistMessage(const Message& message) = 0;
    virtual bool LoadAllMessages() = 0;
    virtual void ClearPersistentStorage() = 0;
};

// Markdown file-based implementation
class FileContextEngine : public ContextEngineBase {
private:
    std::string m_filePath;
    
    std::string FormatMessageAsMarkdown(const Message& msg) const {
        std::string result = "\n## " + msg.role + "\n\n";
        result += msg.content + "\n";
        return result;
    }
    
    Message ParseMarkdownToMessage(const std::string& block) const {
        Message msg;
        size_t headerPos = block.find("## ");
        if (headerPos == std::string::npos) return msg;
        
        size_t newlinePos = block.find('\n', headerPos);
        if (newlinePos == std::string::npos) return msg;
        
        msg.role = block.substr(headerPos + 3, newlinePos - headerPos - 3);
        
        size_t contentStart = block.find_first_not_of("\n\r ", newlinePos);
        if (contentStart != std::string::npos) {
            msg.content = block.substr(contentStart);
        }
        
        return msg;
    }
    
protected:
    bool Initialize(const ContextConfig& config) override {
        ContextEngineBase::Initialize(config);
        
        fs::path dirPath(config.storagePath);
        if (!fs::exists(dirPath)) {
            fs::create_directories(dirPath);
        }
        
        m_filePath = (dirPath / (config.sessionId + ".md")).string();
        
        // Load existing messages if file exists
        if (fs::exists(m_filePath)) {
            LoadAllMessages();
        }
        
        return true;
    }
    
    bool PersistMessage(const Message& message) override {
        try {
            std::ofstream file(m_filePath, std::ios::app);
            if (!file.is_open()) return false;
            
            file << FormatMessageAsMarkdown(message);
            file.close();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to persist message: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool LoadAllMessages() override {
        try {
            std::ifstream file(m_filePath);
            if (!file.is_open()) return false;
            
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            
            // Parse markdown blocks
            size_t pos = 0;
            while ((pos = content.find("## ", pos)) != std::string::npos) {
                size_t nextPos = content.find("## ", pos + 3);
                std::string block = content.substr(pos, nextPos == std::string::npos ? std::string::npos : nextPos - pos);
                
                Message msg = ParseMarkdownToMessage(block);
                if (!msg.role.empty()) {
                    m_messages.push_back(msg);
                }
                
                if (nextPos == std::string::npos) break;
                pos = nextPos;
            }
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Failed to load messages: " << e.what() << std::endl;
            return false;
        }
    }
    
    void ClearPersistentStorage() override {
        try {
            if (fs::exists(m_filePath)) {
                fs::remove(m_filePath);
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to clear persistent storage: " << e.what() << std::endl;
        }
    }
};

// Factory implementation
std::unique_ptr<ContextEngine> ContextEngine::Create(const ContextConfig& config) {
    switch (config.storageType) {
        case ContextStorageType::FILE_MD:
            return std::make_unique<FileContextEngine>();
        case ContextStorageType::DATABASE:
            // TODO: Implement database-backed context engine
            std::cerr << "Database storage not yet implemented, falling back to file storage" << std::endl;
            return std::make_unique<FileContextEngine>();
        default:
            return std::make_unique<FileContextEngine>();
    }
}
