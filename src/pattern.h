// Coffeeclaw - pattern scan (IDA style) and RIP-relative resolve
#pragma once

#include "common.h"
#include <cstring>

namespace claw {

// Parses "48 8B 05 ?? ?? ?? ??" (also accepts "?" and lowercase hex).
inline bool ParsePattern(const char* text, std::vector<int>& out) {
    out.clear();
    std::string cur;
    for (const char* p = text; ; ++p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\0') {
            if (!cur.empty()) {
                if (cur == "?" || cur == "??") out.push_back(-1);
                else {
                    unsigned v = 0;
                    if (std::sscanf(cur.c_str(), "%x", &v) != 1 || v > 0xFF) return false;
                    out.push_back((int)v);
                }
                cur.clear();
            }
            if (c == '\0') break;
        } else {
            cur.push_back(c);
        }
    }
    return !out.empty();
}

// Returns absolute address of the first match inside [base, base+size), or 0.
inline uintptr_t ScanPattern(const uint8_t* data, size_t size, const std::vector<int>& pat) {
    if (!data || pat.empty() || size < pat.size()) return 0;
    const size_t last = size - pat.size();
    for (size_t i = 0; i <= last; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pat.size(); ++j) {
            if (pat[j] >= 0 && data[i + j] != (uint8_t)pat[j]) { ok = false; break; }
        }
        if (ok) return i;
    }
    return 0;
}

// Reads the int32 displacement at (matchOffset + dispOffset) inside the image
// and resolves the RIP-relative target: target = match + matchLen + disp
inline uintptr_t ResolveRip(uintptr_t match, size_t matchOffset, const uint8_t* image,
                            size_t imageSize, int dispOffset, int matchLen) {
    if (!image || matchOffset + (size_t)dispOffset + 4 > imageSize) return 0;
    int32_t disp = 0;
    std::memcpy(&disp, image + matchOffset + (size_t)dispOffset, sizeof(disp));
    return match + (uintptr_t)matchLen + (intptr_t)disp;
}

} // namespace claw
