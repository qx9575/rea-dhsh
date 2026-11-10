#include "script/ScpParser.h"

#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace dhsh {
namespace {

std::string trim(std::string_view value) {
    const auto whitespace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t begin = 0;
    while (begin < value.size() && whitespace(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && whitespace(value[end - 1])) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return result;
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

std::optional<std::string> wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
            static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> decode_codepage(
    std::span<const std::byte> data, UINT codepage, DWORD flags) {
    if (data.empty()) {
        return std::string();
    }
    const auto* text = reinterpret_cast<const char*>(data.data());
    const int wide_size = MultiByteToWideChar(codepage, flags, text,
        static_cast<int>(data.size()), nullptr, 0);
    if (wide_size <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    if (MultiByteToWideChar(codepage, flags, text, static_cast<int>(data.size()),
            wide.data(), wide_size) != wide_size) {
        return std::nullopt;
    }
    return wide_to_utf8(wide);
}

std::pair<std::string, std::string> decode_scp(std::span<const std::byte> data) {
    if (data.size() >= 3 && data[0] == std::byte{0xef}
        && data[1] == std::byte{0xbb} && data[2] == std::byte{0xbf}) {
        data = data.subspan(3);
        if (auto text = decode_codepage(data, CP_UTF8, MB_ERR_INVALID_CHARS)) {
            return {std::move(*text), "utf-8-sig"};
        }
    } else if (auto text = decode_codepage(data, CP_UTF8, MB_ERR_INVALID_CHARS)) {
        return {std::move(*text), "utf-8"};
    }
    if (auto text = decode_codepage(data, 54936, 0)) {
        return {std::move(*text), "gb18030"};
    }
    if (auto text = decode_codepage(data, 936, 0)) {
        return {std::move(*text), "gbk"};
    }
    std::wstring wide;
    wide.reserve(data.size());
    for (const std::byte value : data) {
        wide.push_back(static_cast<wchar_t>(std::to_integer<unsigned char>(value)));
    }
    return {wide_to_utf8(wide).value_or(std::string()), "latin1"};
}

std::string strip_comment(std::string_view line) {
    bool quoted = false;
    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        if (line[index] == '"') {
            quoted = !quoted;
        } else if (!quoted && line[index] == '/' && line[index + 1] == '/') {
            return std::string(line.substr(0, index));
        }
    }
    return std::string(line);
}

bool odd_quotes(std::string_view value) {
    std::size_t count = 0;
    bool escaped = false;
    for (const char c : value) {
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            ++count;
        }
    }
    return count % 2 != 0;
}

bool block_start(std::string_view line, std::string& kind, std::string& tail) {
    const std::string normalized = upper(line);
    for (const std::string_view candidate : {std::string_view("WINDOW"),
             std::string_view("FIXEDPART")}) {
        if (normalized.starts_with(candidate)
            && (normalized.size() == candidate.size()
                || std::isspace(static_cast<unsigned char>(normalized[candidate.size()])))) {
            kind = std::string(candidate);
            tail = trim(line.substr(candidate.size()));
            return true;
        }
    }
    return false;
}

std::string block_type(std::string_view tail) {
    static const std::regex type_pattern(
        R"(\bTYPE\s*=\s*([^\s]+))", std::regex_constants::icase);
    std::match_results<std::string_view::const_iterator> match;
    if (std::regex_search(tail.begin(), tail.end(), match, type_pattern)) {
        return match[1].str();
    }
    return {};
}

bool key_value(std::string_view line, std::string& key, std::string& value) {
    if (line.empty() || !(std::isalpha(static_cast<unsigned char>(line[0]))
            || line[0] == '_')) {
        return false;
    }
    std::size_t position = 1;
    while (position < line.size()
        && (std::isalnum(static_cast<unsigned char>(line[position]))
            || line[position] == '_')) {
        ++position;
    }
    key = upper(line.substr(0, position));
    while (position < line.size()
        && std::isspace(static_cast<unsigned char>(line[position]))) {
        ++position;
    }
    if (position < line.size() && line[position] == '=') {
        ++position;
    }
    value = trim(line.substr(position));
    return true;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    if (!text.empty() && text.back() == '\n') {
        lines.emplace_back();
    }
    return lines;
}

std::string normalize_scp_path(std::string_view path) {
    std::string normalized;
    std::size_t start = 0;
    if (path.empty() || path.front() == '/' || path.front() == '\\'
        || path.find(':') != std::string_view::npos) {
        throw FormatError("unsafe SCP path: " + std::string(path));
    }
    while (start <= path.size()) {
        const std::size_t end = path.find_first_of("/\\", start);
        const std::string component(path.substr(start,
            end == std::string_view::npos ? path.size() - start : end - start));
        if (component == "..") {
            throw FormatError("unsafe SCP path: " + std::string(path));
        }
        if (!component.empty() && component != ".") {
            if (!normalized.empty()) {
                normalized.push_back('/');
            }
            normalized += component;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (normalized.empty()) {
        throw FormatError("empty SCP path");
    }
    return normalized;
}

std::string path_key(std::string_view path) {
    std::string key = lower(path);
    std::replace(key.begin(), key.end(), '\\', '/');
    return key;
}

std::string include_path(std::string_view source, std::string_view include) {
    const std::string normalized = normalize_scp_path(include);
    if (normalized.starts_with("config/") || normalized.starts_with("ui/")
        || normalized.starts_with("plugins/")) {
        return normalized;
    }
    const std::size_t slash = source.find_last_of('/');
    return slash == std::string_view::npos
        ? normalized : std::string(source.substr(0, slash + 1)) + normalized;
}

bool scp_include(std::string_view path) {
    return path.size() >= 4 && lower(path.substr(path.size() - 4)) == ".scp";
}

std::size_t node_count(const ScpDocument& document) {
    std::size_t count = 0;
    std::vector<const ScpNode*> pending;
    for (const ScpNode& node : document.windows) {
        pending.push_back(&node);
    }
    while (!pending.empty()) {
        const ScpNode* node = pending.back();
        pending.pop_back();
        ++count;
        for (const ScpNode& child : node->children) {
            pending.push_back(&child);
        }
    }
    return count;
}

} // namespace

const std::string* ScpNode::property(std::string_view name) const {
    const std::string expected = upper(name);
    for (auto it = properties.rbegin(); it != properties.rend(); ++it) {
        if (it->first == expected) {
            return &it->second;
        }
    }
    return nullptr;
}

std::string scp_unquote(std::string_view value) {
    std::string result = trim(value);
    if (result.size() >= 2 && result.front() == '"' && result.back() == '"') {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

ScpDocument ScpParser::parse(std::span<const std::byte> data,
    std::string source_file, ScpParseOptions options) {
    if (data.size() > options.max_input_bytes) {
        throw FormatError("SCP input budget exceeded: file=" + source_file);
    }
    std::size_t parsed_nodes = 0;
    auto [text, encoding] = decode_scp(data);
    ScpDocument result;
    result.source_file = std::move(source_file);
    result.encoding = std::move(encoding);

    std::vector<ScpNode*> stack;
    std::vector<bool> conditions;
    bool active = true;
    bool in_script = false;
    bool in_block_comment = false;
    std::string script;
    std::vector<std::pair<std::string, std::string>>* pending_target = nullptr;
    std::size_t pending_index = 0;

    const auto warning = [&](std::size_t line, const std::string& text) {
        result.warnings.push_back(result.source_file + ":" + std::to_string(line)
            + ": " + text);
    };
    const auto current_properties = [&]() -> std::vector<std::pair<std::string, std::string>>& {
        return stack.empty() ? result.properties : stack.back()->properties;
    };

    const std::vector<std::string> lines = split_lines(text);
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const std::string& raw = lines[line_index];
        const std::size_t line_number = line_index + 1;
        const std::string stripped = trim(raw);
        if (in_script) {
            if (upper(stripped) == "#ENDSCRIPT") {
                result.scripts.push_back(script);
                script.clear();
                in_script = false;
            } else {
                if (!script.empty()) {
                    script.push_back('\n');
                }
                script += raw;
            }
            continue;
        }
        if (in_block_comment) {
            if (raw.find("*/") != std::string::npos) {
                in_block_comment = false;
            }
            continue;
        }
        if (stripped.starts_with("/*")) {
            if (stripped.find("*/") == std::string::npos) {
                in_block_comment = true;
            }
            continue;
        }
        if (pending_target != nullptr) {
            std::string& value = pending_target->at(pending_index).second;
            value += "\n" + stripped;
            if (!odd_quotes(value)) {
                pending_target = nullptr;
            }
            continue;
        }

        const std::string line = trim(strip_comment(raw));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '#') {
            const std::string directive = trim(std::string_view(line).substr(1));
            const std::size_t split = directive.find_first_of(" \t");
            const std::string name = "#" + upper(directive.substr(0, split));
            const std::string argument = split == std::string::npos
                ? std::string() : trim(std::string_view(directive).substr(split + 1));
            if (name == "#SCRIPT") {
                in_script = true;
                script.clear();
                continue;
            }
            if (name == "#ENDSCRIPT") {
                warning(line_number, "#endscript without #script");
                continue;
            }
            if (name == "#IFDEF" || name == "#IFNDEF") {
                bool condition = upper(argument) == "WINWIDTH800";
                if (name == "#IFNDEF") {
                    condition = !condition;
                }
                conditions.push_back(condition);
                active = active && condition;
                continue;
            }
            if (name == "#ELSE") {
                if (conditions.empty()) {
                    warning(line_number, "#else without #if");
                } else {
                    conditions.back() = !conditions.back();
                    active = std::all_of(conditions.begin(), conditions.end(),
                        [](bool value) { return value; });
                }
                continue;
            }
            if (name == "#ENDIF") {
                if (conditions.empty()) {
                    warning(line_number, "#endif without #if");
                } else {
                    conditions.pop_back();
                    active = std::all_of(conditions.begin(), conditions.end(),
                        [](bool value) { return value; });
                }
                continue;
            }
            if (name == "#INCLUDE") {
                if (active) {
                    const std::string include = trim(argument);
                    if (include.size() >= 2 && include.front() == '"'
                        && include.back() == '"') {
                        result.includes.push_back(include.substr(1, include.size() - 2));
                    } else {
                        warning(line_number, "invalid #include");
                    }
                }
                continue;
            }
            if (name != "#DEFINE" && name != "#UNDEF" && name != "#PRAGMA") {
                warning(line_number, "unknown directive " + line);
            }
            continue;
        }
        if (!active) {
            continue;
        }
        if (upper(line) == "END") {
            if (stack.empty()) {
                warning(line_number, "END without block");
            } else {
                stack.pop_back();
            }
            continue;
        }

        std::string kind;
        std::string tail;
        if (block_start(line, kind, tail)) {
            if (stack.size() >= options.max_depth) {
                throw FormatError("SCP depth budget exceeded: file=" + result.source_file
                    + ":" + std::to_string(line_number));
            }
            if (++parsed_nodes > options.max_nodes) {
                throw FormatError("SCP node budget exceeded: file=" + result.source_file
                    + ":" + std::to_string(line_number));
            }
            ScpNode node;
            node.kind = std::move(kind);
            node.type = block_type(tail);
            node.source_file = result.source_file;
            node.line = line_number;
            if (stack.empty()) {
                result.windows.push_back(std::move(node));
                stack.push_back(&result.windows.back());
            } else {
                stack.back()->children.push_back(std::move(node));
                stack.push_back(&stack.back()->children.back());
            }
            continue;
        }
        if (line.front() == '"') {
            auto& properties = current_properties();
            if (properties.empty()) {
                result.free_text.push_back(line);
            } else {
                properties.back().second += " " + line;
            }
            continue;
        }

        std::string key;
        std::string value;
        if (key_value(line, key, value)) {
            auto& properties = current_properties();
            properties.emplace_back(std::move(key), std::move(value));
            if (odd_quotes(properties.back().second)) {
                pending_target = &properties;
                pending_index = properties.size() - 1;
            }
        } else {
            result.free_text.push_back(line);
        }
    }

    if (in_script) {
        warning(lines.size(), "unclosed #script block");
        result.scripts.push_back(script);
    }
    if (!conditions.empty()) {
        result.warnings.push_back(result.source_file + ": unclosed #if blocks: "
            + std::to_string(conditions.size()));
    }
    if (!stack.empty()) {
        result.warnings.push_back(result.source_file + ": unclosed blocks: "
            + std::to_string(stack.size()));
    }
    return result;
}

ScpDocument ScpParser::load(ResourceProvider& resources, std::string source_file,
    ScpParseOptions options, ScpLoadDiagnostics* diagnostics) {
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    if (diagnostics != nullptr) {
        *diagnostics = {};
    }
    const std::string root = normalize_scp_path(source_file);
    ScpDocument result;
    std::size_t total_nodes = 0;
    std::size_t files_loaded = 0;
    std::size_t total_decoded_bytes = 0;
    std::vector<std::string> active;
    std::unordered_set<std::string> loaded;
    std::function<void(const std::string&, std::size_t)> load_file =
        [&](const std::string& requested, std::size_t depth) {
            const std::string path = normalize_scp_path(requested);
            const std::string key = path_key(path);
            if (depth > options.max_include_depth) {
                throw FormatError("SCP include depth budget exceeded: file=" + path);
            }
            if (std::find(active.begin(), active.end(), key) != active.end()) {
                throw FormatError("SCP include cycle: file=" + path);
            }
            if (loaded.contains(key)) {
                return;
            }
            if (files_loaded >= options.max_files) {
                throw FormatError("SCP include file budget exceeded: file=" + path);
            }
            ++files_loaded;
            active.push_back(key);
            auto stage_start = Clock::now();
            std::vector<std::byte> stored = resources.bytes(path);
            if (diagnostics != nullptr) {
                diagnostics->resource_read_ms += elapsed(stage_start);
            }
            if (stored.size() >= 2 && stored[0] == std::byte{'N'}
                && stored[1] == std::byte{'Z'}) {
                stage_start = Clock::now();
                stored = NzDecoder::decode(stored);
                if (diagnostics != nullptr) {
                    diagnostics->nz_decode_ms += elapsed(stage_start);
                }
            }
            if (stored.size() > options.max_total_decoded_bytes
                - std::min(options.max_total_decoded_bytes, total_decoded_bytes)) {
                throw FormatError("SCP include decoded-byte budget exceeded: file=" + path);
            }
            total_decoded_bytes += stored.size();
            ScpParseOptions file_options = options;
            file_options.max_nodes = options.max_nodes
                - std::min(options.max_nodes, total_nodes);
            stage_start = Clock::now();
            ScpDocument document = parse(stored, path, file_options);
            if (diagnostics != nullptr) {
                diagnostics->parse_ms += elapsed(stage_start);
            }
            const std::size_t count = node_count(document);
            if (count > options.max_nodes - std::min(options.max_nodes, total_nodes)) {
                throw FormatError("SCP include node budget exceeded: file=" + path);
            }
            total_nodes += count;
            const bool is_root = result.source_file.empty();
            if (is_root) {
                result = std::move(document);
            }
            const std::vector<std::string> includes = is_root
                ? result.includes : document.includes;
            for (const std::string& include : includes) {
                const std::string child = include_path(path, include);
                if (!scp_include(child)) {
                    continue;
                }
                if (diagnostics != nullptr) {
                    ++diagnostics->includes;
                }
                std::vector<std::string> candidates{child};
                if (path_key(child).starts_with("config/")) {
                    candidates.push_back(child.substr(7));
                }
                stage_start = Clock::now();
                const auto existing = std::find_if(candidates.begin(), candidates.end(),
                    [&](const std::string& candidate) { return resources.exists(candidate); });
                if (diagnostics != nullptr) {
                    diagnostics->include_resolve_ms += elapsed(stage_start);
                }
                load_file(existing == candidates.end() ? child : *existing, depth + 1);
            }
            if (!is_root) {
                result.warnings.insert(result.warnings.end(), document.warnings.begin(),
                    document.warnings.end());
                for (ScpNode& node : document.windows) {
                    result.windows.push_back(std::move(node));
                }
            }
            active.pop_back();
            loaded.insert(key);
        };
    load_file(root, 0);
    if (diagnostics != nullptr) {
        diagnostics->files = files_loaded;
    }
    return result;
}

} // namespace dhsh
