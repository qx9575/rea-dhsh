#include "ui/MessageBus.h"

#include <stdexcept>
#include <utility>

namespace dhsh {

std::string MessageBus::key(const std::string& control, const std::string& type) {
    return control + '\n' + type;
}

void MessageBus::subscribe(std::string control, std::string type, Handler handler) {
    if (control.empty() || type.empty() || !handler) {
        throw std::invalid_argument("message subscription requires control, type and handler");
    }
    handlers_[key(control, type)].push_back(std::move(handler));
}

bool MessageBus::dispatch(const UiMessage& message) {
    const std::size_t pending = dispatching_ ? dispatched_ + queue_.size() + 1 : 1;
    if (pending > max_messages_per_dispatch_) {
        diagnostics_.push_back("Message dispatch budget exceeded: file="
            + message.source_file + " control=" + message.control
            + " message=" + message.type + " code=" + message.code);
        return false;
    }
    queue_.push_back(message);
    if (dispatching_) {
        return true;
    }

    dispatching_ = true;
    dispatched_ = 0;
    bool result = true;
    try {
        while (!queue_.empty()) {
            UiMessage current = std::move(queue_.front());
            queue_.pop_front();
            ++dispatched_;
            const auto found = handlers_.find(key(current.control, current.type));
            if (found == handlers_.end() || found->second.empty()) {
                diagnostics_.push_back("Unsupported API: file=" + current.source_file
                    + " control=" + current.control + " message=" + current.type
                    + " code=" + current.code);
                result = false;
                continue;
            }
            const auto handlers = found->second;
            for (const Handler& handler : handlers) {
                handler(current);
            }
        }
    } catch (...) {
        queue_.clear();
        dispatching_ = false;
        dispatched_ = 0;
        throw;
    }
    dispatching_ = false;
    dispatched_ = 0;
    return result;
}

} // namespace dhsh
