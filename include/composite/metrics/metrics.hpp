/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * composite is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#pragma once

/**
 * @file metrics.hpp
 * @brief Convenience header for the composite metrics system
 *
 * This header provides everything needed for metric collection:
 * - Metric types: counter, updown_counter, gauge, histogram
 * - Metric registry for creating and querying metrics
 *
 * The metrics system is designed for minimal overhead:
 * - Metric recording uses relaxed atomic operations
 * - Cache-line alignment prevents false sharing
 * - No allocations in the hot path
 *
 * Usage:
 * @code
 * #include <composite/metrics/metrics.hpp>
 *
 * // At initialization (not hot path)
 * auto& packets = composite::metrics::registry::instance()
 *     .create_counter("my_component.packets", "Packets processed", "1",
 *                     {{"component_id", id()}});
 *
 * // In process() loop (hot path)
 * packets.inc();  // Single atomic increment
 * @endcode
 */

#include "registry.hpp"
#include "types.hpp"
