#pragma once

#include <string>
#include <string_view>

namespace hs {

// ASCII-only case folding for protocol text (header names, tokens, file
// extensions). Not std::tolower: that depends on the C locale, and passing it a
// negative char is undefined behaviour. HTTP's case-insensitive parts are
// ASCII, so nothing else is needed.
inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = to_lower_ascii(c);
    return out;
}

}  // namespace hs
