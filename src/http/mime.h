#pragma once

#include <string_view>

namespace hs {

// Content type for a file path, chosen by the extension after the last dot of
// the last path segment, case-insensitively. Unknown or missing extensions give
// "application/octet-stream". Charset is not part of the result; StaticFiles
// adds it for text types.
std::string_view mime_type(std::string_view path);

}  // namespace hs
