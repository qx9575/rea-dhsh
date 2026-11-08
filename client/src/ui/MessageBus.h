#pragma once

#include <cstddef>
#include <deque>
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

    explicit MessageBus(std::size_t max_messages_per_dispatch = 256)
        : max_messages_per_dispatch_(max_messages_per_dispatch) {}

    void subscribe(std::string control, std::string type, Handler handler);
    bool dispatch(const UiMessage& message);

    const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    static std::string key(const std::string& control, const std::string& type);

    std::unordered_map<std::string, std::vector<Handler>> handlers_;
    std::vector<std::string> diagnostics_;
    std::deque<UiMessage> queue_;
    std::size_t max_messages_per_dispatch_ = 256;
    std::size_t dispatched_ = 0;
    bool dispatching_ = false;
};

} // namespace dhsh
