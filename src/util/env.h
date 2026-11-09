// src/util/env.h
#pragma once
#include <cstdlib>
#include <stdexcept>
#include <string>

inline std::string env_required(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        throw std::runtime_error(std::string("missing required env var: ") + name);
    }
    return v;
}

inline std::string env_or(const char* name, std::string fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::move(fallback);
}