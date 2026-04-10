#pragma once
#include <string>
#include <vector>

// Represents a single Skill loaded from a directory
struct Skill {
    std::string id; // Directory name
    std::string name; // Skill Name (extracted from header or folder name)
    std::string description;
    std::string instructions; // Full markdown content
};
