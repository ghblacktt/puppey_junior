#pragma once
#include <cstdlib>
#include <fstream>
#include <string>

inline void load_dotenv(const std::string& path = ".env") {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        // trim
        while (!k.empty() && std::isspace((unsigned char)k.back())) k.pop_back();
        while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
        while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
            v = v.substr(1, v.size() - 2);
        if (::setenv(k.c_str(), v.c_str(), /*overwrite=*/0) != 0) { /* ignore */ }
    }
}