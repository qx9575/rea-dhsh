#pragma once

#include "network/DecodedMessage.h"

#include <filesystem>
#include <vector>

namespace dhsh {

class ProtocolReplay {
public:
    static ProtocolReplay load(const std::filesystem::path& path);

    const std::vector<DecodedMessage>& messages() const noexcept;

private:
    ProtocolReplay() = default;

    std::vector<DecodedMessage> messages_;
};

} // namespace dhsh
