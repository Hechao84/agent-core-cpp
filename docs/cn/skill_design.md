# 技能系统设计文档

## 1. 模块概述

技能系统（`include/skill.h`、`src/skills/skill_engine.h`、`src/skills/skill_engine.cpp`）实现了基于文件的技能发现与渐进式披露机制，使 Agent 能够按需加载专业知识指令，而不在每次推理时都注入全部技能内容。

## 2. Skill 公共结构

### 2.1 Skill 定义

```cpp
struct Skill {
    std::string id;          // 目录名（也是唯一键）
    std::string name;        // 从 YAML frontmatter 提取的名称
    std::string description; // 从 YAML frontmatter 提取的描述
    std::string body;        // SKILL.md 的完整正文（frontmatter 之后）
    std::string directory;   // 技能目录的完整路径
};
```

`Skill` 是稳定的公共 POD（Plain Old Data）结构，通过 `Agent::ListSkills` 和 `Agent::GetSkill` 暴露给应用层（如 HTTP API、Web UI）。

## 3. SKILL.md 格式

### 3.1 文件结构

每个技能是一个目录，包含 `SKILL.md` 文件：

```
my_skills/
 └── skill-creator/
     ├── SKILL.md
     └── scripts/
         ├── init_skill.py
         ├── package_skill.py
         └── quick_validate.py
```

### 3.2 SKILL.md 格式

SKILL.md 使用 YAML frontmatter + Markdown body 格式：

```markdown
---
name: Skill Creator
description: Create, validate, and package new skills for the agent framework
---

# Skill Creator

You are a skill creation assistant. When the user asks you to create a new skill:

1. Ask for the skill name and description
2. Create the directory structure:
   ...
3. Write the SKILL.md file with proper frontmatter
   ...
```

- **YAML frontmatter**：包含 `name` 和 `description` 字段，用于发现阶段的元数据
- **Markdown body**：完整的技能指令，仅在需要时加载

### 3.3 解析流程

```
ParseSkillDir(dirPath, folderName)
  │  1. 读取 SKILL.md 文件内容
  │  2. 分离 frontmatter 和 body:
  │     ├── 找到 "---" 分隔符
  │     ├── 提取 YAML 部分 → 解析 name 和 description
  │     └── 提取 body 部分（frontmatter 之后的全部内容）
  │  3. 构建 Skill 结构:
  │     ├── id = folderName
  │     ├── name = ExtractFrontmatterField(content, "name")
  │     ├── description = ExtractFrontmatterField(content, "description")
  │     ├── body = frontmatter 之后的内容
  │     └── directory = dirPath
  │  4. 返回 Skill
```

### 3.4 ExtractFrontmatterField

```cpp
std::string ExtractFrontmatterField(const std::string& content, const std::string& key);
```

简化的 YAML 解析器，仅支持 `key: value` 格式的单行字段。不处理嵌套结构、列表或多行值。

## 4. SkillEngine 加载与渐进式披露

### 4.1 设计意图

`SkillEngine` 实现了**渐进式披露（Progressive Disclosure）**策略：

- **Level 1（Metadata）**：技能名称和描述，始终注入系统提示
- **Level 2（FullBody）**：完整技能指令，仅通过 `skill_search` 工具按需加载

这种设计避免在每次推理时注入所有技能的完整指令，节省 token 并降低模型混淆的风险。

### 4.2 类结构

```
SkillEngine
  │  ├── rootDir_ (string)                ← 技能根目录路径
  │  ├── skills_ (map<string, Skill>)     ← 已加载的技能注册表
  │  ├── lastMtime_ (string)              ← 上次加载时的目录 mtime
  │  ├── mutex_ (mutex)                   ← 保护并发访问
  │  │
  │  ├── SetRootDir(path)                 ← 设置技能根目录
  │  ├── Load(forceReload)                ← 从磁盘加载技能
  │  ├── GetSkillCatalog()                ← Level 1: 元数据目录
  │  ├── GetSkillInstructions(name)       ← Level 2: 完整指令
  │  ├── SearchSkills(query)              ← 搜索技能
  │  ├── GetSkillIds()                    ← 获取所有技能 ID
  │  ├── GetAllSkills()                   ← 获取所有技能
  │  ├── GetSkill(id)                     ← 获取单个技能
  │  ├── GetRootDir()                     ← 获取根目录路径
  │  │
  │  ├── ParseSkillDir(dir, folder)       ← 解析单个技能目录
  │  └── ExtractFrontmatterField()        ← 提取 YAML 字段
```

### 4.3 Load 流程

```
Load(forceReload = false)
  │  1. 检查是否需要重载:
  │     ├── 若 forceReload → 强制重载
  │     ├── 若 rootDir_ 的 mtime != lastMtime_ → 需要重载
  │     └── 否则 → 跳过
  │
  │  2. 遍历 rootDir_ 下的子目录:
  │     ├── 对每个包含 SKILL.md 的子目录:
  │     │   ├── ParseSkillDir(path, folderName) → Skill
  │     │   └── skills_[skill.id] = skill
  │     └── 忽略不含 SKILL.md 的子目录
  │
  │  3. 更新 lastMtime_
  │  4. 返回 true
```

### 4.4 热重载（mtime 检测）

`SkillEngine` 通过检测技能目录的文件修改时间（mtime）来决定是否需要重载：

- `Load(false)` → 仅在 mtime 变化时重载
- `Load(true)` → 强制重载（用于显式刷新命令）

mtime 检测是轻量级的，不需要每次都重新读取所有文件内容。

## 5. SkillLevel 披露层级

### 5.1 SkillLevel 定义

```cpp
enum class SkillLevel {
    Metadata,    // name + description only (always in prompt)
    FullBody     // name + description + full SKILL.md instructions
};
```

### 5.2 Level 1: Metadata

`GetSkillCatalog()` 返回紧凑的元数据目录，用于系统提示中的 `{$skills}` 占位符：

```
- Skill Creator: Create, validate, and package new skills
- Data Analyst: Analyze datasets and generate reports
- Code Reviewer: Review code for quality and security issues
```

每行格式：`- <name>: <description>`

这种紧凑格式占用极少的 token，但告知模型有哪些技能可用。

### 5.3 Level 2: FullBody

`GetSkillInstructions(skillName)` 返回特定技能的完整指令：

```
# Skill Creator

You are a skill creation assistant. When the user asks you to create a new skill:

1. Ask for the skill name and description
2. Create the directory structure...
```

通过 `skill_search` 工具按需加载：

```
模型输出: tool_calls: [{"name": "skill_search", "arguments": {"query": "create skill"}}]
  │
  ▼
SkillSearchTool::Invoke(input)
  │  ├── skillEngine_->SearchSkills(query) → 匹配的技能名称列表
  │  ├── 对每个匹配的技能:
  │  │   ├── skillEngine_->GetSkillInstructions(name) → 完整指令
  │  │   └── 拼接为结果文本
  │  └── 返回结果
```

### 5.4 渐进式披露的优势

| 方面 | 全量注入 | 渐进式披露 |
|------|---------|-----------|
| Token 消耗 | 高（所有技能的完整指令） | 低（只有元数据目录） |
| 模型选择准确性 | 低（信息过载） | 高（先发现，再深入） |
| 可扩展性 | 受 token 限制 | 无限制（按需加载） |
| 响应延迟 | 无差异 | 需要额外一轮工具调用 |

## 6. 与 Agent 的集成

### 6.1 Agent 拥有 SkillEngine

```cpp
// Agent 构造
skillEngine_ = make_shared<SkillEngine>(config_.skillDirectory);
skillEngine_->Load();
```

### 6.2 注入到 AgentWorker 和 SkillSearchTool

```
Agent 构造
  │  ├── skillEngine_ = make_shared<SkillEngine>(config_.skillDirectory)
  │  ├── skillEngine_->Load()
  │  ├── worker_->SetSkillEngine(skillEngine_)
  │  └── SkillSearchTool::SetSkillEngine(skillEngine_.get())
```

- `AgentWorker` 使用 `skillEngine_` 的 `GetSkillCatalog()` 生成 `{$skills}` 占位符内容
- `SkillSearchTool` 使用 `skillEngine_` 的 `SearchSkills()` 和 `GetSkillInstructions()` 执行技能搜索

### 6.3 SkillSearchTool 的静态指针

`SkillSearchTool` 使用静态指针而非 `ToolBuildContext` 注入 `SkillEngine`：

```cpp
// SkillSearchTool 内部
static SkillEngine* skillEnginePtr_ = nullptr;

void SkillSearchTool::SetSkillEngine(SkillEngine* engine) {
    skillEnginePtr_ = engine;
}
```

这是因为 `SkillEngine` 是 Agent 级别资源（所有会话共享），而非会话级别资源。

### 6.4 Agent 公共 API

```cpp
// Agent 提供技能查询 API（暴露给应用层）
std::vector<Skill> Agent::ListSkills() const;      // 返回所有技能
Skill Agent::GetSkill(const std::string& id) const; // 返回指定技能
std::string Agent::GetSkillRootDir() const;          // 返回技能根目录
```

这些方法直接转发到 `skillEngine_`，供 HTTP API 和 Web UI 使用。

## 7. 技能搜索

### 7.1 SearchSkills

```cpp
std::vector<std::string> SearchSkills(const std::string& query) const;
```

搜索逻辑：
- 在所有技能的 `name` 和 `description` 中查找包含 `query` 子串的匹配
- 返回匹配技能的 ID 列表
- 搜索是简单的子串匹配，不使用 embedding 或语义搜索

### 7.2 未来扩展

可考虑更高级的搜索策略：
- 基于关键词权重的排序
- 基于 embedding 的语义搜索
- 搜索技能 body 内容而非仅元数据

## 8. 数据目录结构

### 8.1 技能目录布局

```
data/skills/                 ← 默认技能目录 (或 my_skills/)
 ├── skill-creator/
 │   ├── SKILL.md
 │   └── scripts/
 │       ├── init_skill.py
 │       └── package_skill.py
 │       └── quick_validate.py
 ├── data-analyst/
 │   ├── SKILL.md
 │   └── templates/
 │       └── report_template.md
 └── code-reviewer/
     ├── SKILL.md
     └── checklists/
         ├── security.md
         └── quality.md
```

### 8.2 配置

```cpp
// AgentConfig 中的技能目录配置
config.skillDirectory = "./my_skills";  // 或 "./data/skills"
```

技能目录可以是绝对路径或相对路径（相对于工作目录）。
