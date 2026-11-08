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
    const auto found = handlers_.find(key(message.control, message.type));
    if (found == handlers_.end() || found->second.empty()) {
        diagnostics_.push_back("Unsupported API: file=" + message.source_file
            + " control=" + message.control + " message=" + message.type
            + " code=" + message.code);
        return false;
    }
    const auto handlers = found->second;
    for (const Handler& handler : handlers) {
        handler(message);
    }
    return true;
}

} // namespace dhsh
