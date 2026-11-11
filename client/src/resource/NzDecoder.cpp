#include "resource/NzDecoder.h"

#include "resource/ResourceError.h"

#include <array>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace dhsh {
namespace {

using Bytes = std::vector<std::byte>;
using Payload = std::span<const std::byte>;

constexpr std::size_t header_size = 12;
constexpr std::uint16_t lzw_end = 0x100;
constexpr std::uint32_t lzw_first_dynamic = 0x101;
constexpr std::uint32_t lzw_max_code = 0xffff;

unsigned byte_at(Payload data, std::size_t offset) {
    return std::to_integer<unsigned>(data[offset]);
}

std::uint16_t read_u16(Payload data, std::size_t offset) {
    return static_cast<std::uint16_t>(byte_at(data, offset)
        | (byte_at(data, offset + 1) << 8));
}

std::uint32_t read_u32(Payload data, std::size_t offset) {
    return static_cast<std::uint32_t>(byte_at(data, offset))
        | (static_cast<std::uint32_t>(byte_at(data, offset + 1)) << 8)
        | (static_cast<std::uint32_t>(byte_at(data, offset + 2)) << 16)
        | (static_cast<std::uint32_t>(byte_at(data, offset + 3)) << 24);
}

Bytes decode_lzw(Payload payload, std::uint32_t expected_size) {
    if (payload.empty()) {
        if (expected_size == 0) {
            return {};
        }
        throw FormatError("empty LZW payload");
    }
    if (payload.size() % 2 != 0) {
        throw FormatError("odd LZW payload size");
    }

    std::size_t input_pos = 0;
    const auto next_code = [&] {
        const std::uint16_t code = read_u16(payload, input_pos);
        input_pos += 2;
        return code;
    };

    const std::uint16_t first = next_code();
    if (first == lzw_end) {
        if (input_pos != payload.size()) {
            throw FormatError("trailing data after LZW end code");
        }
        if (expected_size != 0) {
            throw FormatError("empty LZW output has nonzero size1");
        }
        return {};
    }
    if (first > 0xff) {
        throw FormatError("first LZW code is not a literal");
    }
    if (expected_size == 0) {
        throw FormatError("LZW output exceeds zero size1");
    }
    if (expected_size > 0x7fffffffU) {
        throw FormatError("unsupported LZW output size");
    }

    std::array<std::uint16_t, 0x10000> prefix{};
    std::array<std::byte, 0x10000> suffix{};
    std::array<std::byte, 0x10000> first_of{};
    std::array<std::uint32_t, 0x10000> lengths{};
    for (std::uint32_t code = 0; code < 0x100; ++code) {
        suffix[code] = static_cast<std::byte>(code);
        first_of[code] = static_cast<std::byte>(code);
        lengths[code] = 1;
    }

    Bytes output;
    output.push_back(static_cast<std::byte>(first));
    std::uint16_t previous = first;
    std::uint32_t next = lzw_first_dynamic;

    while (input_pos < payload.size()) {
        const std::uint16_t current = next_code();
        if (current == lzw_end) {
            if (input_pos != payload.size()) {
                throw FormatError("trailing data after LZW end code");
            }
            if (output.size() != expected_size) {
                throw FormatError("LZW output size does not equal size1");
            }
            return output;
        }
        if (current > next) {
            throw FormatError("LZW code is beyond the next dictionary entry");
        }

        const bool deferred_entry = current == next;
        const std::uint16_t base = deferred_entry ? previous : current;
        const std::uint64_t length = static_cast<std::uint64_t>(lengths[base])
            + (deferred_entry ? 1U : 0U);
        if (length > expected_size - output.size()) {
            throw FormatError("LZW output exceeds size1");
        }

        const std::size_t old_size = output.size();
        output.resize(old_size + static_cast<std::size_t>(length));
        std::size_t output_pos = output.size();
        if (deferred_entry) {
            output[--output_pos] = first_of[previous];
        }
        std::uint16_t code = base;
        while (code >= 0x100) {
            output[--output_pos] = suffix[code];
            code = prefix[code];
        }
        output[--output_pos] = static_cast<std::byte>(code);
        if (output_pos != old_size) {
            throw FormatError("invalid LZW dictionary chain");
        }

        const std::byte current_first = deferred_entry ? first_of[previous] : first_of[current];
        if (next <= lzw_max_code) {
            prefix[next] = previous;
            suffix[next] = current_first;
            first_of[next] = first_of[previous];
            lengths[next] = lengths[previous] + 1;
            ++next;
        }
        previous = current;
    }

    throw FormatError("missing LZW end code");
}

Bytes decode_lzo(Payload payload, std::uint32_t expected_size) {
    if (payload.empty()) {
        throw FormatError("empty LZO payload");
    }

    Bytes output;
    std::size_t input_pos = 0;
    const auto read_byte = [&]() -> unsigned {
        if (input_pos >= payload.size()) {
            throw FormatError("truncated LZO input");
        }
        return byte_at(payload, input_pos++);
    };
    const auto literals = [&](std::uint64_t count) {
        if (count > payload.size() - input_pos) {
            throw FormatError("truncated LZO literal run");
        }
        if (count > expected_size - output.size()) {
            throw FormatError("LZO output exceeds size1");
        }
        for (std::uint64_t index = 0; index < count; ++index) {
            output.push_back(payload[input_pos++]);
        }
    };
    const auto match = [&](std::int64_t match_pos, std::uint64_t count) {
        if (match_pos < 0 || static_cast<std::uint64_t>(match_pos) >= output.size()) {
            throw FormatError("LZO match is outside decoded output");
        }
        if (count > expected_size - output.size()) {
            throw FormatError("LZO output exceeds size1");
        }
        std::size_t source = static_cast<std::size_t>(match_pos);
        for (std::uint64_t index = 0; index < count; ++index) {
            output.push_back(output[source++]);
        }
    };
    const auto extended_count = [&](std::uint64_t value, unsigned base) {
        while (true) {
            const unsigned next = read_byte();
            if (next != 0) {
                return value + base + next;
            }
            value += 255;
        }
    };

    enum class State { main, first, match, done };
    State state;
    std::uint64_t token = read_byte();
    if (token > 17) {
        token -= 17;
        if (token < 4) {
            literals(token);
            token = read_byte();
            state = State::match;
        } else {
            literals(token);
            state = State::first;
        }
    } else {
        input_pos = 0;
        state = State::main;
    }

    while (true) {
        if (state == State::main) {
            token = read_byte();
            if (token >= 16) {
                state = State::match;
                continue;
            }
            if (token == 0) {
                token = extended_count(token, 15);
            }
            literals(token + 3);
            state = State::first;
            continue;
        }

        if (state == State::first) {
            token = read_byte();
            if (token >= 16) {
                state = State::match;
                continue;
            }
            const unsigned next = read_byte();
            const auto match_pos = static_cast<std::int64_t>(output.size())
                - (1 + 0x0800) - static_cast<std::int64_t>(token >> 2)
                - (static_cast<std::int64_t>(next) << 2);
            match(match_pos, 3);
            state = State::done;
            continue;
        }

        if (state == State::match) {
            std::int64_t match_pos;
            std::uint64_t count;
            if (token >= 64) {
                const unsigned next = read_byte();
                match_pos = static_cast<std::int64_t>(output.size()) - 1
                    - static_cast<std::int64_t>((token >> 2) & 7)
                    - (static_cast<std::int64_t>(next) << 3);
                count = (token >> 5) + 1;
            } else if (token >= 32) {
                token &= 31;
                if (token == 0) {
                    token = extended_count(token, 31);
                }
                const unsigned value = read_byte() | (read_byte() << 8);
                match_pos = static_cast<std::int64_t>(output.size()) - 1 - (value >> 2);
                count = token + 2;
            } else if (token >= 16) {
                match_pos = static_cast<std::int64_t>(output.size())
                    - static_cast<std::int64_t>((token & 8) << 11);
                token &= 7;
                if (token == 0) {
                    token = extended_count(token, 7);
                }
                const unsigned value = read_byte() | (read_byte() << 8);
                match_pos -= value >> 2;
                if (match_pos == static_cast<std::int64_t>(output.size())) {
                    if (input_pos != payload.size()) {
                        throw FormatError("trailing data after LZO end marker");
                    }
                    if (output.size() != expected_size) {
                        throw FormatError("LZO output size does not equal size1");
                    }
                    return output;
                }
                match_pos -= 0x4000;
                count = token + 2;
            } else {
                const unsigned next = read_byte();
                match_pos = static_cast<std::int64_t>(output.size()) - 1
                    - static_cast<std::int64_t>(token >> 2)
                    - (static_cast<std::int64_t>(next) << 2);
                count = token + 2;
            }
            match(match_pos, count);
            state = State::done;
            continue;
        }

        if (input_pos < 2) {
            throw FormatError("invalid LZO match state");
        }
        token = byte_at(payload, input_pos - 2) & 3;
        if (token != 0) {
            literals(token);
            token = read_byte();
            state = State::match;
        } else {
            state = State::main;
        }
    }
}

} // namespace

std::vector<std::byte> NzDecoder::decode(std::span<const std::byte> data) {
    if (data.size() < header_size || byte_at(data, 0) != 'N' || byte_at(data, 1) != 'Z') {
        throw FormatError("invalid NZ header");
    }

    const std::uint16_t kind = read_u16(data, 2);
    const std::uint32_t size1 = read_u32(data, 4);
    const std::uint32_t size2 = read_u32(data, 8);
    const Payload payload = data.subspan(header_size);
    if (payload.size() != size2) {
        throw FormatError("NZ payload size does not equal size2");
    }

    try {
        if (kind == 0) {
            if (size1 != size2) {
                throw FormatError("stored NZ size1 does not equal size2");
            }
            return Bytes(payload.begin(), payload.end());
        }
        if (kind == 0x0a) {
            return decode_lzw(payload, size1);
        }
        if (kind == 0x0b) {
            return decode_lzo(payload, size1);
        }
        throw FormatError("unsupported NZ kind");
    } catch (const FormatError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw FormatError("cannot allocate NZ decoded output");
    } catch (const std::length_error&) {
        throw FormatError("NZ decoded output is too large");
    }
}

} // namespace dhsh
