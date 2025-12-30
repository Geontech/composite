/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
 
#pragma once

namespace composite {

class lifecycle {
public:
    virtual ~lifecycle() = default;
    virtual auto initialize() -> void = 0;
    virtual auto start() -> void = 0;
    virtual auto stop() -> void = 0;

}; // class lifecycle

} // namespace composite