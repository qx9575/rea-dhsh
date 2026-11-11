#include "ui/ScpWindow.h"

#include "formats/A16.h"
#include "formats/Apg.h"
#include "platform/WicImage.h"
#include "resource/NzDecoder.h"
#include "resource/ResourceError.h"
#include "script/FsLuaHost.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dhsh {
namespace {

using ProfileClock = std::chrono::steady_clock;

double profile_ms(ProfileClock::time_point start) {
    return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size()
        && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin
        && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

bool is_hidden(const ScpNode& node) {
    const std::string* value = node.property("HIDE");
    if (value == nullptr) {
        return false;
    }
    const std::string normalized = lower(scp_unquote(*value));
    return normalized == "1" || normalized == "true" || normalized == "yes";
}

std::int32_t checked_coordinate(long long value) {
    if (value < std::numeric_limits<std::int32_t>::min()
        || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::out_of_range("SCP RECT coordinate is out of range");
    }
    return static_cast<std::int32_t>(value);
}

LogicalRect parse_rect(const ScpNode& node) {
    const std::string* property = node.property("RECT");
    if (property == nullptr) {
        return {};
    }
    std::array<long long, 4> values{};
    std::size_t position = 0;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::size_t comma = property->find(',', position);
        const std::string part = trim(std::string_view(*property).substr(position,
            comma == std::string::npos ? std::string_view::npos : comma - position));
        std::size_t consumed = 0;
        values[index] = std::stoll(part, &consumed);
        if (consumed != part.size()) {
            throw std::invalid_argument("invalid SCP RECT component");
        }
        if (index + 1 < values.size()) {
            if (comma == std::string::npos) {
                throw std::invalid_argument("SCP RECT requires four components");
            }
            position = comma + 1;
        } else if (comma != std::string::npos) {
            throw std::invalid_argument("SCP RECT has too many components");
        }
    }
    if (values[2] < 0 || values[3] < 0) {
        throw std::invalid_argument("SCP RECT dimensions cannot be negative");
    }
    return {checked_coordinate(values[0]), checked_coordinate(values[1]),
        checked_coordinate(values[2]), checked_coordinate(values[3])};
}

std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(data[offset]))
        | static_cast<std::uint16_t>(std::to_integer<unsigned>(data[offset + 1])) << 8;
}

Image decode_tga(std::span<const std::byte> data) {
    if (data.size() < 18) {
        throw std::invalid_argument("TGA header is truncated");
    }
    const std::uint8_t id_length = std::to_integer<std::uint8_t>(data[0]);
    const std::uint8_t color_map = std::to_integer<std::uint8_t>(data[1]);
    const std::uint8_t kind = std::to_integer<std::uint8_t>(data[2]);
    const std::uint16_t width = read_u16(data, 12);
    const std::uint16_t height = read_u16(data, 14);
    const std::uint8_t depth = std::to_integer<std::uint8_t>(data[16]);
    const std::uint8_t descriptor = std::to_integer<std::uint8_t>(data[17]);
    if (color_map != 0 || (kind != 2 && kind != 10)
        || (depth != 24 && depth != 32) || width == 0 || height == 0) {
        throw std::invalid_argument("only uncompressed or RLE true-color TGA is supported");
    }
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (pixel_count > 64U * 1024U * 1024U / 4U) {
        throw std::invalid_argument("TGA dimensions exceed the image budget");
    }
    const std::size_t bytes_per_pixel = depth / 8;
    std::size_t position = 18U + id_length;
    std::vector<std::array<std::uint8_t, 4>> pixels;
    pixels.reserve(pixel_count);
    const auto read_pixel = [&]() {
        if (position + bytes_per_pixel > data.size()) {
            throw std::invalid_argument("TGA pixel data is truncated");
        }
        std::array<std::uint8_t, 4> pixel{
            std::to_integer<std::uint8_t>(data[position + 2]),
            std::to_integer<std::uint8_t>(data[position + 1]),
            std::to_integer<std::uint8_t>(data[position]),
            depth == 32 ? std::to_integer<std::uint8_t>(data[position + 3])
                        : static_cast<std::uint8_t>(255),
        };
        position += bytes_per_pixel;
        return pixel;
    };
    if (kind == 2) {
        while (pixels.size() < pixel_count) {
            pixels.push_back(read_pixel());
        }
    } else {
        while (pixels.size() < pixel_count) {
            if (position >= data.size()) {
                throw std::invalid_argument("TGA RLE data is truncated");
            }
            const std::uint8_t control = std::to_integer<std::uint8_t>(data[position++]);
            const std::size_t count = static_cast<std::size_t>(control & 0x7fU) + 1U;
            if (count > pixel_count - pixels.size()) {
                throw std::invalid_argument("TGA RLE packet exceeds the image");
            }
            if ((control & 0x80U) != 0) {
                const auto pixel = read_pixel();
                pixels.insert(pixels.end(), count, pixel);
            } else {
                for (std::size_t index = 0; index < count; ++index) {
                    pixels.push_back(read_pixel());
                }
            }
        }
    }

    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(pixel_count * 4);
    const bool top_origin = (descriptor & 0x20U) != 0;
    const bool right_origin = (descriptor & 0x10U) != 0;
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t source_y = top_origin ? y : height - 1U - y;
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t source_x = right_origin ? width - 1U - x : x;
            const auto& pixel = pixels[source_y * width + source_x];
            const std::size_t destination = (y * width + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                image.rgba[destination + channel] = std::byte{pixel[channel]};
            }
        }
    }
    return image;
}

Image decode_image(std::string_view path, std::span<const std::byte> data) {
    const std::string normalized = lower(path);
    if (normalized.ends_with(".a16")) {
        return A16Decoder::decode(data);
    }
    if (normalized.ends_with(".apg")) {
        ApgAnimation animation = ApgDecoder::decode(data);
        if (animation.frames.empty()) {
            throw std::invalid_argument("APG button has no frames");
        }
        return std::move(animation.frames.front().image);
    }
    if (normalized.ends_with(".tga")) {
        return decode_tga(data);
    }
    return WicImage::decode(data);
}

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

COLORREF text_color(const Widget& widget) {
    const std::string* value = widget.node().property("TEXTCOLOR");
    if (value == nullptr) {
        return RGB(255, 255, 255);
    }
    unsigned red = 255;
    unsigned green = 255;
    unsigned blue = 255;
    if (std::sscanf(value->c_str(), "%u,%u,%u", &red, &green, &blue) == 3) {
        return RGB(std::min(red, 255U), std::min(green, 255U), std::min(blue, 255U));
    }
    return RGB(255, 255, 255);
}

Image rasterize_text(const Widget& widget, const LogicalRect& clip) {
    Image result;
    const LogicalRect& bounds = widget.bounds();
    if (clip.width <= 0 || clip.height <= 0 || widget.text().empty()) {
        return result;
    }
    const std::wstring text = utf8_to_wide(widget.text());
    if (text.empty()) {
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
    const int font_height = -std::max(8, std::min(bounds.height - 2, 14));
    HFONT font = CreateFontW(font_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    const HGDIOBJ old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_color(widget));
    RECT rect{bounds.x - clip.x, bounds.y - clip.y,
        bounds.x - clip.x + bounds.width, bounds.y - clip.y + bounds.height};
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    GdiFlush();

    result.width = static_cast<std::uint32_t>(clip.width);
    result.height = static_cast<std::uint32_t>(clip.height);
    result.rgba.resize(static_cast<std::size_t>(clip.width) * clip.height * 4);
    const auto* source = static_cast<const std::uint8_t*>(pixels);
    const COLORREF color = text_color(widget);
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

struct MessageDeclaration {
    std::string type;
    int value = 0;
    std::string code;
};

std::optional<MessageDeclaration> parse_message(const std::string& value) {
    std::size_t position = 0;
    const auto token = [&]() {
        while (position < value.size()
            && std::isspace(static_cast<unsigned char>(value[position]))) {
            ++position;
        }
        const std::size_t start = position;
        while (position < value.size()
            && !std::isspace(static_cast<unsigned char>(value[position]))) {
            ++position;
        }
        return value.substr(start, position - start);
    };
    MessageDeclaration result;
    result.type = token();
    const std::string number = token();
    if (result.type.empty() || number.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        result.value = std::stoi(number, &consumed);
        if (consumed != number.size()) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (token() != "CODE") {
        return std::nullopt;
    }
    result.code = scp_unquote(trim(std::string_view(value).substr(position)));
    return result.code.empty() ? std::nullopt : std::optional(result);
}

} // namespace

ScpWindow::ScpWindow(ScpDocument document, ResourceProvider& resources,
    MessageBus& messages, ImageDecoder decoder, ScpWindowOptions options,
    ScpWindowProfile* profile)
    : document_(std::move(document)), resources_(resources), messages_(messages),
      decoder_(decoder ? std::move(decoder) : ImageDecoder(decode_image)),
      options_(options), profile_(profile) {
    if (profile_ != nullptr) {
        *profile_ = {};
    }
    auto stage_start = ProfileClock::now();
    for (const ScpNode& node : document_.windows) {
        find_unresolved_layout(node, 1);
    }
    if (profile_ != nullptr) {
        profile_->layout_ms = profile_ms(stage_start);
    }
    for (const ScpNode& node : document_.windows) {
        build_node(node, {}, true, 1, true);
    }
    if (!document_.scripts.empty()) {
        stage_start = ProfileClock::now();
        lua_host_ = std::make_unique<FsLuaHost>(*this, messages_);
        if (profile_ != nullptr) {
            profile_->lua_ms = profile_ms(stage_start);
        }
    }
}

ScpWindow::~ScpWindow() = default;

void ScpWindow::find_unresolved_layout(const ScpNode& node, std::size_t depth) {
    if (depth > options_.max_depth) {
        throw FormatError("SCP window depth budget exceeded: file=" + node.source_file);
    }
    std::unordered_map<std::string, const ScpNode*> positions;
    std::unordered_set<const ScpNode*> duplicates;
    for (const ScpNode& child : node.children) {
        if (lower(child.type) != "button" || child.property("NAME") == nullptr
            || is_hidden(child)) {
            continue;
        }
        try {
            const LogicalRect rect = parse_rect(child);
            const std::string key = std::to_string(rect.x) + ','
                + std::to_string(rect.y) + ',' + std::to_string(rect.width) + ','
                + std::to_string(rect.height);
            const auto [position, inserted] = positions.emplace(key, &child);
            if (!inserted) {
                duplicates.insert(position->second);
                duplicates.insert(&child);
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    unresolved_layout_.insert(duplicates.begin(), duplicates.end());
    for (const ScpNode& child : node.children) {
        find_unresolved_layout(child, depth + 1);
    }
}

void ScpWindow::build_node(const ScpNode& node, LogicalPoint parent,
    bool parent_visible, std::size_t depth, bool layout_valid) {
    const std::string& source = node.source_file.empty()
        ? document_.source_file : node.source_file;
    if (depth > options_.max_depth) {
        throw FormatError("SCP window depth budget exceeded: file=" + source);
    }
    if (widgets_.size() >= options_.max_widgets) {
        throw FormatError("SCP window node budget exceeded: file=" + source);
    }
    LogicalRect bounds;
    try {
        bounds = parse_rect(node);
    } catch (const std::exception& error) {
        layout_valid = false;
        diagnostics_.push_back("Unsupported invalid RECT: file=" + source
            + " control=" + node.type + " line=" + std::to_string(node.line)
            + ": " + error.what());
    }
    const std::int64_t x = static_cast<std::int64_t>(parent.x) + bounds.x;
    const std::int64_t y = static_cast<std::int64_t>(parent.y) + bounds.y;
    bounds.x = x > std::numeric_limits<std::int32_t>::max()
        ? std::numeric_limits<std::int32_t>::max()
        : x < std::numeric_limits<std::int32_t>::min()
            ? std::numeric_limits<std::int32_t>::min() : static_cast<std::int32_t>(x);
    bounds.y = y > std::numeric_limits<std::int32_t>::max()
        ? std::numeric_limits<std::int32_t>::max()
        : y < std::numeric_limits<std::int32_t>::min()
            ? std::numeric_limits<std::int32_t>::min() : static_cast<std::int32_t>(y);
    const auto widget_start = ProfileClock::now();
    widgets_.emplace_back(&node, source, bounds, parent_visible);
    if (profile_ != nullptr) {
        profile_->widget_setup_ms += profile_ms(widget_start);
    }
    Widget& widget = widgets_.back();
    for (const auto& [key, value] : node.properties) {
        if (key == "MESSAGE" && !parse_message(value).has_value()) {
            diagnostics_.push_back("Unsupported MESSAGE: file=" + source
                + " control=" + widget.name() + " line="
                + std::to_string(node.line) + " value=" + value);
        }
    }
    if (!layout_valid) {
        widget.mark_unsupported();
        diagnostics_.push_back("Unsupported invalid layout: file=" + source
            + " control=" + widget.name() + " type=" + node.type);
    } else if (unresolved_layout_.contains(&node)) {
        widget.mark_unsupported();
        diagnostics_.push_back("Unsupported unresolved layout: file=" + source
            + " control=" + widget.name() + " type=" + node.type);
    } else if (!widget.supported()) {
        diagnostics_.push_back("Unsupported control: file=" + source
            + " control=" + widget.name() + " type=" + node.type);
    } else if (widget.visible()) {
        load_image(widget);
    }
    const bool child_parent_visible = widget.visible();
    for (const ScpNode& child : node.children) {
        build_node(child, {bounds.x, bounds.y}, child_parent_visible, depth + 1,
            layout_valid);
    }
}

void ScpWindow::load_image(Widget& widget) {
    std::string path;
    if (const std::string* value = widget.node().property("BMPPATH")) {
        path = scp_unquote(*value) + "/default.tga";
    } else if (const std::string* value = widget.node().property("BMPPATHAPG")) {
        path = scp_unquote(*value);
    } else if (const std::string* value = widget.node().property("BMP")) {
        path = scp_unquote(*value);
    }
    if (path.empty()) {
        return;
    }
    if (profile_ != nullptr) {
        ++profile_->images;
    }
    try {
        std::vector<std::byte> data;
        auto stage_start = ProfileClock::now();
        try {
            data = resources_.bytes(path);
        } catch (const std::exception&) {
            if (path.starts_with("ui/") || path.starts_with("ui\\")) {
                data = resources_.bytes(path.substr(3));
            } else {
                throw;
            }
        }
        if (profile_ != nullptr) {
            profile_->resource_read_ms += profile_ms(stage_start);
        }
        if (data.size() >= 2 && data[0] == std::byte{'N'}
            && data[1] == std::byte{'Z'}) {
            stage_start = ProfileClock::now();
            data = NzDecoder::decode(data);
            if (profile_ != nullptr) {
                profile_->nz_decode_ms += profile_ms(stage_start);
            }
        }
        const std::string normalized = lower(path);
        const bool wic = !normalized.ends_with(".a16") && !normalized.ends_with(".apg")
            && !normalized.ends_with(".tga");
        stage_start = ProfileClock::now();
        Image image = decoder_(path, data);
        if (profile_ != nullptr) {
            (wic ? profile_->wic_decode_ms : profile_->proprietary_decode_ms)
                += profile_ms(stage_start);
        }
        widget.set_image(std::move(image));
    } catch (const std::exception& error) {
        diagnostics_.push_back("Resource error: file=" + widget.source_file()
            + " control=" + widget.name() + " path=" + path + ": " + error.what());
    }
}

const Widget* ScpWindow::find_widget(std::string_view name) const {
    const auto found = std::find_if(widgets_.begin(), widgets_.end(),
        [&](const Widget& widget) { return widget.name() == name; });
    return found == widgets_.end() ? nullptr : &*found;
}

Widget* ScpWindow::find_widget(std::string_view name) {
    const auto found = std::find_if(widgets_.begin(), widgets_.end(),
        [&](const Widget& widget) { return widget.name() == name; });
    return found == widgets_.end() ? nullptr : &*found;
}

std::vector<LogicalRect> ScpWindow::input_regions() const {
    std::vector<LogicalRect> result;
    for (const Widget& widget : widgets_) {
        if (widget.intercepts_input() && widget.bounds().width > 0
            && widget.bounds().height > 0) {
            result.push_back(widget.bounds());
        }
    }
    return result;
}

std::optional<std::size_t> ScpWindow::hit_test(LogicalPoint point) const {
    for (std::size_t index = widgets_.size(); index > 0; --index) {
        const Widget& widget = widgets_[index - 1];
        if (widget.intercepts_input() && widget.bounds().contains(point)) {
            return index - 1;
        }
    }
    return std::nullopt;
}

std::vector<UiMessage> ScpWindow::collect_messages(
    const Widget& widget, std::string_view type) const {
    std::vector<UiMessage> result;
    for (const auto& [key, value] : widget.node().properties) {
        if (key != "MESSAGE") {
            continue;
        }
        const auto declaration = parse_message(value);
        if (!declaration.has_value() || declaration->type != type) {
            continue;
        }
        result.push_back(UiMessage{widget.name(), declaration->type,
            declaration->value, declaration->code, widget.source_file()});
    }
    return result;
}

std::vector<UiMessage> ScpWindow::message_declarations() const {
    std::vector<UiMessage> result;
    for (const Widget& widget : widgets_) {
        for (const auto& [key, value] : widget.node().properties) {
            if (key != "MESSAGE") {
                continue;
            }
            const auto declaration = parse_message(value);
            if (!declaration.has_value()) {
                continue;
            }
            result.push_back(UiMessage{widget.name(), declaration->type,
                declaration->value, declaration->code, widget.source_file()});
        }
    }
    return result;
}

bool ScpWindow::handle_event(const WindowEvent& event) {
    using Type = WindowEvent::Type;
    if (event.type == Type::input_cancelled) {
        pressed_.reset();
        return false;
    }
    if (!event.logical_position.has_value()) {
        return false;
    }
    const auto hit = hit_test(*event.logical_position);
    if (event.type == Type::left_button_down) {
        pressed_.reset();
        if (hit.has_value() && widgets_[*hit].enabled()) {
            pressed_ = hit;
        }
        return hit.has_value();
    }
    if (event.type == Type::left_button_up) {
        const bool consumed = hit.has_value() || pressed_.has_value();
        std::vector<UiMessage> outgoing;
        if (hit.has_value() && pressed_ == hit && widgets_[*hit].enabled()) {
            const Widget& widget = widgets_[*hit];
            if (widget.type() == WidgetType::button) {
                outgoing = collect_messages(widget, "FSM_BTN_MSG");
            }
            std::vector<UiMessage> mouse = collect_messages(widget, "FSM_MOUSE_UP");
            outgoing.insert(outgoing.end(), mouse.begin(), mouse.end());
        }
        pressed_.reset();
        MessageBus& bus = messages_;
        for (const UiMessage& message : outgoing) {
            bus.dispatch(message);
        }
        return consumed;
    }
    return hit.has_value()
        && (event.type == Type::mouse_move || event.type == Type::mouse_wheel);
}

bool ScpWindow::render(SoftwareRenderer& renderer) {
    bool changed = false;
    for (const Widget& widget : widgets_) {
        if (!widget.visible() || !widget.supported()) {
            continue;
        }
        if (const Image* image = widget.image()) {
            changed = renderer.draw(*image, widget.bounds().x, widget.bounds().y)
                || changed;
        }
        if (!widget.text().empty()) {
            const LogicalRect& bounds = widget.bounds();
            const std::int64_t left = std::max<std::int64_t>(0, bounds.x);
            const std::int64_t top = std::max<std::int64_t>(0, bounds.y);
            const std::int64_t right = std::min<std::int64_t>(renderer.width(),
                static_cast<std::int64_t>(bounds.x) + bounds.width);
            const std::int64_t bottom = std::min<std::int64_t>(renderer.height(),
                static_cast<std::int64_t>(bounds.y) + bounds.height);
            if (right <= left || bottom <= top) {
                continue;
            }
            const LogicalRect clip{static_cast<std::int32_t>(left),
                static_cast<std::int32_t>(top), static_cast<std::int32_t>(right - left),
                static_cast<std::int32_t>(bottom - top)};
            const std::size_t pixels = static_cast<std::size_t>(clip.width)
                * static_cast<std::size_t>(clip.height);
            if (pixels > options_.max_text_pixels) {
                diagnostics_.push_back("Text budget exceeded: file=" + widget.source_file()
                    + " control=" + widget.name() + " pixels=" + std::to_string(pixels));
                continue;
            }
            const Image text = rasterize_text(widget, clip);
            if (text.width != 0 && text.height != 0) {
                changed = renderer.draw(text, clip.x, clip.y) || changed;
            }
        }
    }
    return changed;
}

bool handle_scp_event(ScpWindows& windows, const WindowEvent& event) {
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        if (*it != nullptr && (*it)->handle_event(event)) {
            return true;
        }
    }
    return false;
}

std::vector<LogicalRect> scp_input_regions(const ScpWindows& windows) {
    std::vector<LogicalRect> result;
    for (const auto& window : windows) {
        if (window == nullptr) {
            continue;
        }
        std::vector<LogicalRect> regions = window->input_regions();
        result.insert(result.end(), regions.begin(), regions.end());
    }
    return result;
}

} // namespace dhsh
