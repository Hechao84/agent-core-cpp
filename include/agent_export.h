// agent_export.h
#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef BUILDING_AGENT_FRAMEWORK
        #define AGENT_API __declspec(dllexport)
    #else
        #define AGENT_API __declspec(dllimport)
    #endif
#else
    #define AGENT_API __attribute__((visibility("default")))
#endif

// Export macro for memory plugin shared libraries. A plugin compiles against
// the framework headers and exports the RegisterMemoryPlugin entry point that
// ResourceManager::LoadMemoryPlugins() looks up after loading the library.
#if defined(_WIN32) || defined(__CYGWIN__)
    #define AGENT_PLUGIN_API __declspec(dllexport)
#else
    #define AGENT_PLUGIN_API __attribute__((visibility("default")))
#endif
