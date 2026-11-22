#include "ui/Widget.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
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

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size()
        && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
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
    if (normalized == "LISTVIEWEX") {
        return WidgetType::list_view_ex;
    }
    if (normalized == "RICHEDIT") {
        return WidgetType::rich_edit;
    }
    if (normalized == "COMBOBOX") {
        return WidgetType::combo_box;
    }
    if (normalized == "SCROLL") {
        return WidgetType::scroll;
    }
    return WidgetType::unsupported;
}

// CLIENTRECT is advisory layout metadata; a malformed value disables the
// client area instead of failing widget construction.
std::optional<LogicalRect> client_rect_property(const ScpNode& node) {
    const std::string* value = node.property("CLIENTRECT");
    if (value == nullptr) {
        return std::nullopt;
    }
    LogicalRect rect;
    std::size_t position = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const std::size_t comma = value->find(',', position);
        const std::string part = trim(std::string_view(*value).substr(position,
            comma == std::string::npos ? std::string_view::npos : comma - position));
        if (part.empty()) {
            return std::nullopt;
        }
        char* end = nullptr;
        const long long component = std::strtoll(part.c_str(), &end, 10);
        if (end == nullptr || *end != '\0'
            || component < std::numeric_limits<std::int32_t>::min()
            || component > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        const auto narrow = static_cast<std::int32_t>(component);
        switch (index) {
        case 0: rect.x = narrow; break;
        case 1: rect.y = narrow; break;
        case 2: rect.width = narrow; break;
        default: rect.height = narrow; break;
        }
        if (index + 1 < 4) {
            if (comma == std::string::npos) {
                return std::nullopt;
            }
            position = comma + 1;
        }
    }
    if (rect.width < 0 || rect.height < 0) {
        return std::nullopt;
    }
    return rect;
}

const ScpNode& require_node(const ScpNode* node) {
    if (node == nullptr) {
        throw std::invalid_argument("Widget requires an SCP node");
    }
    return *node;
}

} // namespace

Widget::Widget(const ScpNode* node, std::string source_file,
    LogicalRect bounds, bool visible)
    : Widget(require_node(node), std::move(source_file), bounds, visible) {
}

Widget::Widget(const ScpNode& node, std::string source_file,
    LogicalRect bounds, bool visible)
    : node_(&node), source_file_(std::move(source_file)),
      bounds_(bounds), client_rect_(client_rect_property(node)),
      type_(widget_type(node_->type)),
      visible_(visible && !boolean_property(*node_, "HIDE", false)),
      enabled_(boolean_property(*node_, "ENABLE", true)) {
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
            || type_ == WidgetType::edit || type_ == WidgetType::list_view_ex
            || type_ == WidgetType::rich_edit || type_ == WidgetType::combo_box
            || node_->property("MESSAGE") != nullptr);
}

void Widget::set_image(Image image) {
    image_ = std::move(image);
}

std::optional<COLORREF> scp_color_property(
    const ScpNode& node, std::string_view name) {
    const std::string* value = node.property(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    unsigned red = 255;
    unsigned green = 255;
    unsigned blue = 255;
    if (std::sscanf(value->c_str(), "%u,%u,%u", &red, &green, &blue) != 3) {
        return std::nullopt;
    }
    return RGB(std::min(red, 255U), std::min(green, 255U), std::min(blue, 255U));
}

std::size_t utf8_advance(std::string_view text, std::size_t offset,
    long long delta) {
    offset = std::min(offset, text.size());
    while (delta > 0 && offset < text.size()) {
        do {
            ++offset;
        } while (offset < text.size()
            && (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U);
        --delta;
    }
    while (delta < 0 && offset > 0) {
        do {
            --offset;
        } while (offset > 0
            && (static_cast<unsigned char>(text[offset]) & 0xc0U) == 0x80U);
        ++delta;
    }
    return offset;
}

namespace {

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length);
    return result;
}

HFONT make_text_font(int bounds_height) {
    const int font_height = -std::max(8, std::min(bounds_height - 2, 14));
    return CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

} // namespace

Image rasterize_text_image(std::string_view text, const LogicalRect& bounds,
    const LogicalRect& clip, COLORREF color) {
    Image result;
    if (clip.width <= 0 || clip.height <= 0 || text.empty()) {
        return result;
    }
    const std::wstring wide = utf8_to_wide(text);
    if (wide.empty()) {
        return result;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = clip.width;
    info.bmiHeader.biHeight = -clip.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
        return result;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        DeleteObject(bitmap);
        return result;
    }
    const HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HFONT font = make_text_font(bounds.height);
    const HGDIOBJ old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT rect{bounds.x - clip.x, bounds.y - clip.y,
        bounds.x - clip.x + bounds.width, bounds.y - clip.y + bounds.height};
    DrawTextW(dc, wide.data(), static_cast<int>(wide.size()), &rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    GdiFlush();

    result.width = static_cast<std::uint32_t>(clip.width);
    result.height = static_cast<std::uint32_t>(clip.height);
    result.rgba.resize(static_cast<std::size_t>(clip.width) * clip.height * 4);
    const auto* source = static_cast<const std::uint8_t*>(pixels);
    for (std::size_t offset = 0; offset < result.rgba.size(); offset += 4) {
        const unsigned coverage = std::max({source[offset], source[offset + 1],
            source[offset + 2]});
        result.rgba[offset] = std::byte{GetRValue(color)};
        result.rgba[offset + 1] = std::byte{GetGValue(color)};
        result.rgba[offset + 2] = std::byte{GetBValue(color)};
        result.rgba[offset + 3] = std::byte{static_cast<std::uint8_t>(coverage)};
    }
    if (old_font != nullptr) {
        SelectObject(dc, old_font);
    }
    if (font != nullptr) {
        DeleteObject(font);
    }
    SelectObject(dc, old_bitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return result;
}

} // namespace dhsh
