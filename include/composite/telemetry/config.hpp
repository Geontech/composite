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

#include <chrono>
#include <string>

namespace composite::telemetry {

/**
 * @brief OTLP exporter configuration
 */
struct exporter_config {
    /// OTLP HTTP endpoint (OTEL_EXPORTER_OTLP_ENDPOINT)
    std::string endpoint{"http://localhost:4318"};
    /// OTLP exporter protocol (OTEL_EXPORTER_OTLP_PROTOCOL or OTEL_EXPORTER_OTLP_METRICS_PROTOCOL)
    std::string protocol{"http/protobuf"};
    /// Export timeout in milliseconds (OTEL_EXPORTER_OTLP_TIMEOUT)
    std::chrono::milliseconds timeout{10000};
    /// Additional headers as comma-separated key=value pairs (OTEL_EXPORTER_OTLP_HEADERS)
    std::string headers{};
};

/**
 * @brief OpenTelemetry metrics export configuration
 *
 * This configures the OTLP exporter that bridges native composite metrics
 * to OpenTelemetry collectors. The native metrics::registry always collects
 * metrics; this configuration controls whether they are exported via OTLP.
 */
struct config {
    /// Enable OTLP export (native metrics are always collected regardless)
    bool enabled{false};
    /// Service name for telemetry data (OTEL_SERVICE_NAME)
    std::string service_name{"composite"};
    /// Service version (uses composite version if empty)
    std::string service_version{};
    /// Export interval - how often to push metrics to collector
    std::chrono::milliseconds export_interval{10000};
    /// OTLP exporter settings
    exporter_config exporter;
};

/**
 * @brief Standard OpenTelemetry environment variable names
 *
 * These follow the OpenTelemetry specification for environment variables.
 * JSON config values take precedence; environment variables provide defaults
 * when config values are left at their defaults.
 */
namespace env {
    inline constexpr auto SERVICE_NAME = "OTEL_SERVICE_NAME";
    inline constexpr auto EXPORTER_ENDPOINT = "OTEL_EXPORTER_OTLP_ENDPOINT";
    inline constexpr auto EXPORTER_PROTOCOL = "OTEL_EXPORTER_OTLP_PROTOCOL";
    inline constexpr auto EXPORTER_METRICS_PROTOCOL = "OTEL_EXPORTER_OTLP_METRICS_PROTOCOL";
    inline constexpr auto EXPORTER_TIMEOUT = "OTEL_EXPORTER_OTLP_TIMEOUT";
    inline constexpr auto EXPORTER_HEADERS = "OTEL_EXPORTER_OTLP_HEADERS";
    inline constexpr auto METRIC_EXPORT_INTERVAL = "OTEL_METRIC_EXPORT_INTERVAL";
} // namespace env

} // namespace composite::telemetry
