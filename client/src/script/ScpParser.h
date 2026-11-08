#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dhsh {

struct ScpNode {
    std::string kind;
    std::string type;
    std::size_t line = 0;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<ScpNode> children;

    const std::string* property(std::string_view name) const;
};

struct ScpDocument {
    std::string source_file;
    std::string encoding;
    std::vector<std::string> includes;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<ScpNode> windows;
    std::vector<std::string> warnings;
    std::vector<std::string> scripts;
    std::vector<std::string> free_text;
};

class ScpParser {
public:
    static ScpDocument parse(
        std::span<const std::byte> data, std::string source_file = "<memory>");
};

std::string scp_unquote(std::string_view value);

} // namespace dhsh
