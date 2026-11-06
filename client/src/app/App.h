#pragma once

namespace dhsh {

class App {
public:
    int logical_width() const noexcept;
    int logical_height() const noexcept;
    int run(int argc, char** argv);
};

} // namespace dhsh
