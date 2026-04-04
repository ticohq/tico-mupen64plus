/// @file TicoUtils.h
/// @brief String utility functions for filename processing
#pragma once
#include <string>
#include <vector>

namespace TicoUtils {

    /// @brief Trim leading and trailing whitespace
    static inline std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (std::string::npos == first) {
            return str;
        }
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    /// @brief Extract a display-friendly title from a ROM filename
    /// @details Removes file extension and strips content within () and []
    static inline std::string GetCleanTitle(const std::string& filename) {
        // First remove extension
        std::string title = filename;
        size_t lastDot = title.find_last_of(".");
        if (lastDot != std::string::npos) {
            title = title.substr(0, lastDot);
        }

        std::string result = "";
        result.reserve(title.length());

        int parenDepth = 0;
        int bracketDepth = 0;
        bool lastWasSpace = false;

        for (char c : title) {
            if (c == '(') {
                parenDepth++;
                continue;
            }
            if (c == ')') {
                if (parenDepth > 0) parenDepth--;
                continue;
            }
            if (c == '[') {
                bracketDepth++;
                continue;
            }
            if (c == ']') {
                if (bracketDepth > 0) bracketDepth--;
                continue;
            }

            if (parenDepth == 0 && bracketDepth == 0) {
                if (c == ' ' || c == '_') {
                     if (!result.empty() && !lastWasSpace) {
                         result += ' ';
                         lastWasSpace = true;
                     }
                } else {
                    result += c;
                    lastWasSpace = false;
                }
            }
        }

        return Trim(result);
    }

}
