/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

/**
 * @file composite.hpp
 * @brief Convenience header that includes all commonly needed composite framework headers
 *
 * This single header provides everything needed for basic component development:
 * - Component base class
 * - Buffer types (mutable_buffer, immutable_buffer)
 * - Port types (input_port, output_port)
 * - Property system (included via component.hpp)
 *
 * Usage:
 * @code
 * #include <composite/composite.hpp>
 *
 * class MyComponent : public composite::component {
 *     // ... implementation
 * };
 * @endcode
 */

// Core component interface
#include "composite/core/component.hpp"

// Buffer types
#include "composite/buffers/buffer.hpp"

// Port types
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
