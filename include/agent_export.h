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
