#include "ui/Widget.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace dhsh {
namespace {

std::string upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    return result;
}

bool boolean_property(const ScpNode& node, std::string_view name, bool fallback) {
    const std::string* value = node.property(name);
    if (value == nullptr) {
        return fallback;
    }
    const std::string normalized = upper(scp_unquote(*value));
    if (normalized == "1" || normalized == "TRUE" || normalized == "YES") {
        return true;
    }
    if (normalized == "0" || normalized == "FALSE" || normalized == "NO") {
        return false;
    }
    return fallback;
}

WidgetType widget_type(std::string_view type) {
    const std::string normalized = upper(type);
    if (normalized == "WIN") {
        return WidgetType::window;
    }
    if (normalized == "BUTTON") {
        return WidgetType::button;
    }
    if (normalized == "STATIC") {
        return WidgetType::static_text;
    }
    if (normalized == "EDIT") {
        return WidgetType::edit;
    }
    return WidgetType::unsupported;
}

} // namespace

Widget::Widget(const ScpNode* node, std::string source_file,
    LogicalRect bounds, bool visible)
    : node_(node), source_file_(std::move(source_file)),
      bounds_(bounds), type_(widget_type(node_->type)),
      visible_(visible && !boolean_property(*node_, "HIDE", false)),
      enabled_(boolean_property(*node_, "ENABLE", true)) {
    if (node_ == nullptr) {
        throw std::invalid_argument("Widget requires an SCP node");
    }
    if (const std::string* name = node_->property("NAME")) {
        name_ = scp_unquote(*name);
    } else {
        name_ = "<unnamed:" + std::to_string(node_->line) + ">";
    }
    if (const std::string* text = node_->property("TEXT")) {
        text_ = scp_unquote(*text);
    }
}

const Image* Widget::image() const noexcept {
    return image_.has_value() ? &*image_ : nullptr;
}

bool Widget::intercepts_input() const noexcept {
    return visible_ && supported()
        && (image_.has_value() || !text_.empty() || type_ == WidgetType::button
            || type_ == WidgetType::edit || node_->property("MESSAGE") != nullptr);
}

void Widget::set_image(Image image) {
    image_ = std::move(image);
}

} // namespace dhsh
