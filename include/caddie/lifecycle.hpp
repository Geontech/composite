#pragma once

namespace caddie {

class lifecycle {
public:
    virtual ~lifecycle() = default;
    virtual auto initialize() -> void = 0;
    virtual auto start() -> void = 0;
    virtual auto stop() -> void = 0;

}; // class lifecycle

} // namespace caddie