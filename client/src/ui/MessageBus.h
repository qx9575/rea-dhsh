#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dhsh {

struct UiMessage {
    std::string control;
    std::string type;
    int value = 0;
    std::string code;
    std::string source_file;
};

class MessageBus {
public:
    using Handler = std::function<void(const UiMessage&)>;

    void subscribe(std::string control, std::string type, Handler handler);
    bool dispatch(const UiMessage& message);

    const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    static std::string key(const std::string& control, const std::string& type);

    std::unordered_map<std::string, std::vector<Handler>> handlers_;
    std::vector<std::string> diagnostics_;
};

} // namespace dhsh
