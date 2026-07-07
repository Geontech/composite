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

#ifdef COMPOSITE_USE_OPENTELEMETRY

#include "composite/telemetry/manager.hpp"
#include "composite/metrics/metrics.hpp"

#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/resource/resource.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace composite::telemetry {

namespace otel_metrics = opentelemetry::metrics;
namespace otlp = opentelemetry::exporter::otlp;
namespace sdk_metrics = opentelemetry::sdk::metrics;
namespace resource = opentelemetry::sdk::resource;

/**
 * @brief Safely convert uint64_t to int64_t for OTel export
 *
 * OTel uses int64_t for counters. If the native counter exceeds INT64_MAX,
 * we clamp to INT64_MAX to avoid overflow to negative values.
 */
inline auto to_otel_int64(uint64_t value) -> int64_t {
    constexpr auto max_val = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(std::clamp(value, uint64_t{0}, max_val));
}

/**
 * @brief Base class for callback context to enable polymorphic cleanup
 */
struct callback_context_base {
    virtual ~callback_context_base() = default;
};

/**
 * @brief Typed callback context for metric callbacks
 */
template<typename T>
struct callback_context : callback_context_base {
    T* metric;
    std::vector<std::pair<std::string, std::string>> labels;

    callback_context(T* m, std::vector<std::pair<std::string, std::string>> l)
        : metric(m), labels(std::move(l)) {}
};

/**
 * @brief Bucket callback context for histogram buckets
 */
struct bucket_context : callback_context_base {
    metrics::histogram* hist;
    std::size_t bucket_idx;
    std::vector<std::pair<std::string, std::string>> labels;

    bucket_context(metrics::histogram* h, std::size_t idx,
                   std::vector<std::pair<std::string, std::string>> l)
        : hist(h), bucket_idx(idx), labels(std::move(l)) {}
};

/**
 * @brief Tracks an OTel instrument and its associated callback context
 *
 * Note: Member order matters! context must be destroyed AFTER instrument
 * to ensure the callback isn't invoked with a dangling pointer.
 * C++ destroys members in reverse order of declaration.
 */
struct instrument_entry {
    std::unique_ptr<callback_context_base> context;  // Destroyed last
    opentelemetry::nostd::shared_ptr<otel_metrics::ObservableInstrument> instrument;  // Destroyed first
    std::string metric_name;  // Native metric name for lookup
    metrics::labels_t metric_labels;  // Labels for distinguishing metrics with same name
};

/**
 * @brief Implementation details for telemetry manager
 */
struct manager::impl {
    std::atomic<bool> initialized{false};
    std::mutex init_mutex;  // Guards initialization to prevent races
    telemetry::config config;

    // OTel meter for creating instruments
    opentelemetry::nostd::shared_ptr<otel_metrics::Meter> meter;

    // Observer registration IDs from the metrics registry
    std::optional<std::size_t> registration_observer_id;
    std::optional<std::size_t> deregistration_observer_id;

    // Mutex for thread-safe instrument creation/removal
    std::mutex instrument_mutex;

    // Tracked instruments with their contexts (for proper cleanup)
    std::vector<instrument_entry> instruments;
};

auto manager::instance() -> manager& {
    static manager instance;
    return instance;
}

manager::manager() : m_impl(std::make_unique<impl>()) {}

manager::~manager() {
    if (m_impl && m_impl->initialized.load()) {
        shutdown();
    }
}

auto manager::is_initialized() const -> bool {
    return m_impl && m_impl->initialized.load();
}

/**
 * @brief Normalize protocol string to lowercase
 */
static auto normalize_protocol(std::string protocol) -> std::string {
    std::transform(protocol.begin(), protocol.end(), protocol.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return protocol;
}

/**
 * @brief Fix endpoint when gRPC protocol is requested but only HTTP is supported
 *
 * Handles:
 * - Port 4317 -> 4318 conversion
 * - Missing port -> insert :4318 between host and path
 * - Other ports -> warn but leave as-is
 *
 * URL format: scheme://host[:port][/path]
 */
static auto fix_grpc_endpoint(std::string& endpoint) -> void {
    // Find scheme end (e.g., "http://")
    auto scheme_end = endpoint.find("://");
    auto host_start = (scheme_end != std::string::npos) ? scheme_end + 3 : 0;

    // Find path start (first '/' after host)
    auto path_start = endpoint.find('/', host_start);

    // Find port in the host portion (between host_start and path_start or end)
    auto host_end = (path_start != std::string::npos) ? path_start : endpoint.size();
    auto host_portion = endpoint.substr(host_start, host_end - host_start);
    auto port_pos = host_portion.rfind(':');

    if (port_pos != std::string::npos) {
        // Port exists - check if it's 4317
        auto absolute_port_pos = host_start + port_pos;
        if (endpoint.compare(absolute_port_pos, 5, ":4317") == 0) {
            endpoint.replace(absolute_port_pos, 5, ":4318");
            spdlog::info("telemetry: converted gRPC port 4317 to HTTP port 4318");
        } else {
            spdlog::warn("telemetry: endpoint has non-standard port; HTTP export may fail");
        }
    } else {
        // No port - insert :4318 between host and path
        auto insert_pos = (path_start != std::string::npos) ? path_start : endpoint.size();
        // Remove trailing slash if that's where we're inserting
        if (insert_pos == endpoint.size() && !endpoint.empty() && endpoint.back() == '/') {
            endpoint.pop_back();
            insert_pos = endpoint.size();
        }
        endpoint.insert(insert_pos, ":4318");
        spdlog::info("telemetry: added HTTP port 4318 to endpoint");
    }
}

/**
 * @brief Apply protocol configuration (from JSON or env var)
 *
 * Handles gRPC -> HTTP fallback with endpoint fixup.
 * Updates cfg.exporter.protocol to reflect the actual protocol being used.
 */
static auto apply_protocol_config(telemetry::config& cfg, const std::string& requested_protocol) -> void {
    auto protocol = normalize_protocol(requested_protocol);

    if (protocol == "grpc") {
        spdlog::warn(
            "telemetry: OTLP protocol 'grpc' requested but only HTTP is supported; "
            "forcing http/protobuf");
        fix_grpc_endpoint(cfg.exporter.endpoint);
        cfg.exporter.protocol = "http/protobuf";
    } else if (protocol == "http/json") {
        cfg.exporter.protocol = "http/json";
    } else {
        // Default to http/protobuf for any other value
        cfg.exporter.protocol = "http/protobuf";
    }
}

/**
 * @brief Apply environment variable defaults to config
 *
 * Environment variables provide fallback defaults. JSON config values
 * take precedence - ENV vars only apply when config uses default values.
 * This follows OTel SDK specification where config file > env vars > defaults.
 */
static auto apply_env_defaults(telemetry::config& cfg) -> void {
    // Default values for comparison
    constexpr auto default_service_name = "composite";
    constexpr auto default_endpoint = "http://localhost:4318";
    constexpr auto default_protocol = "http/protobuf";
    constexpr auto default_timeout = std::chrono::milliseconds{10000};
    constexpr auto default_export_interval = std::chrono::milliseconds{10000};

    // Only apply ENV if config has default value (not explicitly set)
    if (cfg.service_name == default_service_name) {
        if (const char* val = std::getenv(env::SERVICE_NAME); val != nullptr) {
            cfg.service_name = val;
        }
    }

    if (cfg.exporter.endpoint == default_endpoint) {
        if (const char* val = std::getenv(env::EXPORTER_ENDPOINT); val != nullptr) {
            cfg.exporter.endpoint = val;
        }
    }

    // Protocol: apply normalization and gRPC fallback regardless of source
    // But only use ENV protocol if config has default
    std::string protocol_to_apply = cfg.exporter.protocol;
    if (cfg.exporter.protocol == default_protocol) {
        const char* protocol_env = std::getenv(env::EXPORTER_METRICS_PROTOCOL);
        if (protocol_env == nullptr) {
            protocol_env = std::getenv(env::EXPORTER_PROTOCOL);
        }
        if (protocol_env != nullptr) {
            protocol_to_apply = protocol_env;
        }
    }
    apply_protocol_config(cfg, protocol_to_apply);

    if (cfg.exporter.timeout == default_timeout) {
        if (const char* val = std::getenv(env::EXPORTER_TIMEOUT); val != nullptr) {
            try {
                cfg.exporter.timeout = std::chrono::milliseconds{std::stoul(val)};
            } catch (...) {
                spdlog::warn("Invalid {} value: {}", env::EXPORTER_TIMEOUT, val);
            }
        }
    }

    if (cfg.exporter.headers.empty()) {
        if (const char* val = std::getenv(env::EXPORTER_HEADERS); val != nullptr) {
            cfg.exporter.headers = val;
        }
    }

    if (cfg.export_interval == default_export_interval) {
        if (const char* val = std::getenv(env::METRIC_EXPORT_INTERVAL); val != nullptr) {
            try {
                cfg.export_interval = std::chrono::milliseconds{std::stoul(val)};
            } catch (...) {
                spdlog::warn("Invalid {} value: {}", env::METRIC_EXPORT_INTERVAL, val);
            }
        }
    }
}

static auto ensure_http_metrics_path(std::string endpoint) -> std::string {
    constexpr std::string_view path = "/v1/metrics";
    if (endpoint.size() >= path.size() &&
        endpoint.compare(endpoint.size() - path.size(), path.size(), path) == 0) {
        return endpoint;
    }
    if (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    return endpoint + std::string(path);
}

/**
 * @brief Convert native labels to OTel KeyValueIterable
 */
static auto labels_to_kv(const metrics::labels_t& labels)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> kv;
    kv.reserve(labels.size());
    for (const auto& [k, v] : labels) {
        kv.emplace_back(k, v);
    }
    return kv;
}

/**
 * @brief Parse header string in "key=value,key2=value2" format
 */
static auto parse_headers(const std::string& header_str)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> headers;
    if (header_str.empty()) {
        return headers;
    }

    std::size_t pos = 0;
    while (pos < header_str.size()) {
        auto comma = header_str.find(',', pos);
        auto segment = (comma != std::string::npos)
            ? header_str.substr(pos, comma - pos)
            : header_str.substr(pos);

        auto eq = segment.find('=');
        if (eq != std::string::npos && eq > 0) {
            auto key = segment.substr(0, eq);
            auto value = segment.substr(eq + 1);
            // Trim whitespace
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(0, 1);
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(0, 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            if (!key.empty()) {
                headers.emplace_back(std::move(key), std::move(value));
            }
        }

        pos = (comma != std::string::npos) ? comma + 1 : header_str.size();
    }
    return headers;
}

auto manager::initialize(const telemetry::config& cfg) -> bool {
    // Use mutex + compare_exchange to prevent race conditions on double-init
    auto lock = std::lock_guard{m_impl->init_mutex};

    bool expected = false;
    if (!m_impl->initialized.compare_exchange_strong(expected, true)) {
        spdlog::warn("telemetry: already initialized");
        return true;
    }

    if (!cfg.enabled) {
        spdlog::debug("telemetry: OTLP export disabled by configuration");
        m_impl->initialized.store(false);  // Not really initialized
        return true;
    }

    // Copy config and apply environment variable defaults (config takes precedence)
    m_impl->config = cfg;
    apply_env_defaults(m_impl->config);

    spdlog::info("telemetry: initializing OTLP export (service: {}, endpoint: {}, protocol: {})",
        m_impl->config.service_name, m_impl->config.exporter.endpoint, m_impl->config.exporter.protocol);

    try {
        // Configure OTLP HTTP metric exporter
        otlp::OtlpHttpMetricExporterOptions exporter_opts;
        exporter_opts.url = ensure_http_metrics_path(m_impl->config.exporter.endpoint);
        exporter_opts.timeout = m_impl->config.exporter.timeout;

        // Set content type based on protocol
        if (m_impl->config.exporter.protocol == "http/json") {
            exporter_opts.content_type = otlp::HttpRequestContentType::kJson;
        } else {
            // Default to binary/protobuf for http/protobuf and any other value
            exporter_opts.content_type = otlp::HttpRequestContentType::kBinary;
        }

        // Apply custom headers from config
        auto headers = parse_headers(m_impl->config.exporter.headers);
        for (const auto& [key, value] : headers) {
            exporter_opts.http_headers.insert({key, value});
            spdlog::debug("telemetry: added header '{}'", key);
        }

        auto exporter = otlp::OtlpHttpMetricExporterFactory::Create(exporter_opts);

        // Configure periodic reader
        sdk_metrics::PeriodicExportingMetricReaderOptions reader_opts;
        reader_opts.export_interval_millis = m_impl->config.export_interval;
        reader_opts.export_timeout_millis = m_impl->config.exporter.timeout;

        auto reader = sdk_metrics::PeriodicExportingMetricReaderFactory::Create(
            std::move(exporter), reader_opts);

        // Create meter provider with resource attributes
        // Using OTel semantic convention attribute names directly
        auto resource_attrs = resource::ResourceAttributes{
            {"service.name", m_impl->config.service_name},
        };
        if (!m_impl->config.service_version.empty()) {
            resource_attrs["service.version"] = m_impl->config.service_version;
        }
        auto res = resource::Resource::Create(resource_attrs);

        auto provider = sdk_metrics::MeterProviderFactory::Create(
            std::make_unique<sdk_metrics::ViewRegistry>(), res);

        auto* sdk_provider = static_cast<sdk_metrics::MeterProvider*>(provider.get());
        sdk_provider->AddMetricReader(std::move(reader));

        // Set as global provider (convert unique_ptr to nostd::shared_ptr)
        opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider> shared_provider(
            provider.release());
        otel_metrics::Provider::SetMeterProvider(shared_provider);

        // Get meter for our instrumentation
        m_impl->meter = otel_metrics::Provider::GetMeterProvider()->GetMeter(
            "composite",
            m_impl->config.service_version.empty() ? "0.0.0" : m_impl->config.service_version);

        // Register as an observer on the native metrics registry
        // This callback is invoked for existing metrics and any future metrics
        auto& registry = metrics::registry::instance();

        m_impl->registration_observer_id = registry.add_observer(
            [this](const metrics::metric_metadata& meta, void* metric_ptr) {
                create_otel_instrument(meta, metric_ptr);
            },
            true  // notify for existing metrics
        );

        // Register deregistration observer to clean up OTel instruments when metrics are removed
        m_impl->deregistration_observer_id = registry.add_deregistration_observer(
            [this](const metrics::metric_metadata& meta) {
                remove_otel_instrument(meta);
            }
        );

        spdlog::info("telemetry: OTLP export initialized (interval: {}ms)",
            m_impl->config.export_interval.count());
        return true;

    } catch (const std::exception& e) {
        spdlog::error("telemetry: failed to initialize OTLP export: {}", e.what());
        m_impl->initialized.store(false);
        return false;
    }
}

auto manager::shutdown() -> void {
    auto lock = std::lock_guard{m_impl->init_mutex};

    if (!m_impl->initialized.load()) {
        return;
    }

    spdlog::debug("telemetry: shutting down OTLP export");

    // Unregister from metrics registry
    auto& registry = metrics::registry::instance();
    if (m_impl->registration_observer_id) {
        registry.remove_observer(*m_impl->registration_observer_id);
        m_impl->registration_observer_id.reset();
    }
    if (m_impl->deregistration_observer_id) {
        registry.remove_deregistration_observer(*m_impl->deregistration_observer_id);
        m_impl->deregistration_observer_id.reset();
    }

    // Clear instruments and their contexts (prevents memory leaks)
    {
        auto inst_lock = std::lock_guard{m_impl->instrument_mutex};
        m_impl->instruments.clear();  // unique_ptr contexts are automatically deleted
    }

    // Reset meter. Assignment, not .reset(): nostd::shared_ptr only grew reset() in newer
    // opentelemetry-cpp releases, and this must compile against distro packages (1.19).
    m_impl->meter = opentelemetry::nostd::shared_ptr<otel_metrics::Meter>{};

    // Replace global provider with noop to flush pending exports
    opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider> noop_provider(
        new otel_metrics::NoopMeterProvider());
    otel_metrics::Provider::SetMeterProvider(noop_provider);

    m_impl->initialized.store(false);
    spdlog::info("telemetry: OTLP export shutdown complete");
}

/**
 * @brief Check if two label sets are equal (order-independent)
 */
static auto labels_match(const metrics::labels_t& a, const metrics::labels_t& b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    auto a_sorted = a;
    auto b_sorted = b;
    auto by_key = [](const metrics::label_pair& left, const metrics::label_pair& right) {
        if (left.first != right.first) {
            return left.first < right.first;
        }
        return left.second < right.second;
    };
    std::sort(a_sorted.begin(), a_sorted.end(), by_key);
    std::sort(b_sorted.begin(), b_sorted.end(), by_key);
    return a_sorted == b_sorted;
}

/**
 * @brief Remove OTel instruments for a metric that was deregistered
 *
 * This is called by the deregistration observer to clean up OTel instruments
 * when a native metric is removed, preventing use-after-free.
 *
 * Matches by both name AND labels to avoid removing metrics with the same
 * name but different label sets.
 */
auto manager::remove_otel_instrument(const metrics::metric_metadata& meta) -> void {
    auto lock = std::lock_guard{m_impl->instrument_mutex};

    // Remove all instruments matching this metric name AND labels
    // (histograms have multiple: _count, _sum, _bucket, but all share the same base name/labels)
    auto it = std::remove_if(m_impl->instruments.begin(), m_impl->instruments.end(),
        [&meta](const instrument_entry& entry) {
            return entry.metric_name == meta.name && labels_match(entry.metric_labels, meta.labels);
        });

    if (it != m_impl->instruments.end()) {
        auto count = std::distance(it, m_impl->instruments.end());
        m_impl->instruments.erase(it, m_impl->instruments.end());
        spdlog::debug("telemetry: removed {} OTel instrument(s) for '{}'", count, meta.name);
    }
}

auto manager::create_otel_instrument(
    const metrics::metric_metadata& meta,
    void* metric_ptr
) -> void {
    if (!m_impl->meter) {
        return;
    }

    auto lock = std::lock_guard{m_impl->instrument_mutex};

    try {
        // Prepare labels as key-value pairs
        auto labels = labels_to_kv(meta.labels);

        switch (meta.type) {
            case metrics::metric_type::counter: {
            auto* native = static_cast<metrics::counter<uint64_t>*>(metric_ptr);

            // Create context with proper ownership
            auto ctx = std::make_unique<callback_context<metrics::counter<uint64_t>>>(native, labels);
            auto* ctx_ptr = ctx.get();  // Raw pointer for callback

            // Create instrument
            auto instrument = m_impl->meter->CreateInt64ObservableCounter(
                meta.name, meta.description, meta.unit);

            // Register callback with raw pointer (context owned by instrument_entry)
            instrument->AddCallback(
                [](opentelemetry::metrics::ObserverResult result, void* state) {
                    auto* c = static_cast<callback_context<metrics::counter<uint64_t>>*>(state);
                    auto value = to_otel_int64(c->metric->value());
                    if (auto* obs = opentelemetry::nostd::get_if<
                            opentelemetry::nostd::shared_ptr<
                                opentelemetry::metrics::ObserverResultT<int64_t>>>(&result)) {
                        (*obs)->Observe(value, c->labels);
                    }
                },
                ctx_ptr);

            m_impl->instruments.push_back({std::move(ctx), std::move(instrument), meta.name, meta.labels});
            spdlog::debug("telemetry: created OTel counter '{}'", meta.name);
            break;
        }

        case metrics::metric_type::updown_counter: {
            auto* native = static_cast<metrics::updown_counter<int64_t>*>(metric_ptr);

            auto ctx = std::make_unique<callback_context<metrics::updown_counter<int64_t>>>(native, labels);
            auto* ctx_ptr = ctx.get();

            auto instrument = m_impl->meter->CreateInt64ObservableUpDownCounter(
                meta.name, meta.description, meta.unit);

            instrument->AddCallback(
                [](opentelemetry::metrics::ObserverResult result, void* state) {
                    auto* c = static_cast<callback_context<metrics::updown_counter<int64_t>>*>(state);
                    if (auto* obs = opentelemetry::nostd::get_if<
                            opentelemetry::nostd::shared_ptr<
                                opentelemetry::metrics::ObserverResultT<int64_t>>>(&result)) {
                        (*obs)->Observe(c->metric->value(), c->labels);
                    }
                },
                ctx_ptr);

            m_impl->instruments.push_back({std::move(ctx), std::move(instrument), meta.name, meta.labels});
            spdlog::debug("telemetry: created OTel updown_counter '{}'", meta.name);
            break;
        }

        case metrics::metric_type::gauge: {
            auto* native = static_cast<metrics::gauge<double>*>(metric_ptr);

            auto ctx = std::make_unique<callback_context<metrics::gauge<double>>>(native, labels);
            auto* ctx_ptr = ctx.get();

            auto instrument = m_impl->meter->CreateDoubleObservableGauge(
                meta.name, meta.description, meta.unit);

            instrument->AddCallback(
                [](opentelemetry::metrics::ObserverResult result, void* state) {
                    auto* c = static_cast<callback_context<metrics::gauge<double>>*>(state);
                    if (auto* obs = opentelemetry::nostd::get_if<
                            opentelemetry::nostd::shared_ptr<
                                opentelemetry::metrics::ObserverResultT<double>>>(&result)) {
                        (*obs)->Observe(c->metric->value(), c->labels);
                    }
                },
                ctx_ptr);

            m_impl->instruments.push_back({std::move(ctx), std::move(instrument), meta.name, meta.labels});
            spdlog::debug("telemetry: created OTel gauge '{}'", meta.name);
            break;
        }

        case metrics::metric_type::histogram: {
            // OTel doesn't have observable histograms - we export as multiple metrics
            // following Prometheus conventions: count, sum, and bucket metrics
            auto* native = static_cast<metrics::histogram*>(metric_ptr);
            const auto& boundaries = native->boundaries();

            // Export count
            {
                auto ctx = std::make_unique<callback_context<metrics::histogram>>(native, labels);
                auto* ctx_ptr = ctx.get();

                auto instrument = m_impl->meter->CreateInt64ObservableCounter(
                    meta.name + "_count", meta.description + " (count)", "1");

                instrument->AddCallback(
                    [](opentelemetry::metrics::ObserverResult result, void* state) {
                        auto* c = static_cast<callback_context<metrics::histogram>*>(state);
                        if (auto* obs = opentelemetry::nostd::get_if<
                                opentelemetry::nostd::shared_ptr<
                                    opentelemetry::metrics::ObserverResultT<int64_t>>>(&result)) {
                            (*obs)->Observe(static_cast<int64_t>(c->metric->count()), c->labels);
                        }
                    },
                    ctx_ptr);

                m_impl->instruments.push_back({std::move(ctx), std::move(instrument), meta.name, meta.labels});
            }

            // Export sum
            {
                auto ctx = std::make_unique<callback_context<metrics::histogram>>(native, labels);
                auto* ctx_ptr = ctx.get();

                auto instrument = m_impl->meter->CreateDoubleObservableGauge(
                    meta.name + "_sum", meta.description + " (sum)", meta.unit);

                instrument->AddCallback(
                    [](opentelemetry::metrics::ObserverResult result, void* state) {
                        auto* c = static_cast<callback_context<metrics::histogram>*>(state);
                        if (auto* obs = opentelemetry::nostd::get_if<
                                opentelemetry::nostd::shared_ptr<
                                    opentelemetry::metrics::ObserverResultT<double>>>(&result)) {
                            (*obs)->Observe(c->metric->sum(), c->labels);
                        }
                    },
                    ctx_ptr);

                m_impl->instruments.push_back({std::move(ctx), std::move(instrument), meta.name, meta.labels});
            }

            // Export each bucket as a cumulative counter with "le" label
            for (std::size_t i = 0; i <= boundaries.size(); ++i) {
                std::string le_value = (i < boundaries.size())
                    ? std::to_string(boundaries[i])
                    : "+Inf";

                auto bucket_labels = labels;
                bucket_labels.emplace_back("le", le_value);

                auto ctx = std::make_unique<bucket_context>(native, i, bucket_labels);
                auto* ctx_ptr = ctx.get();

                auto instrument = m_impl->meter->CreateInt64ObservableCounter(
                    meta.name + "_bucket", meta.description + " (bucket)", "1");

                instrument->AddCallback(
                    [](opentelemetry::metrics::ObserverResult result, void* state) {
                        auto* c = static_cast<bucket_context*>(state);
                        auto counts = c->hist->bucket_counts();
                        int64_t cumulative = 0;
                        for (std::size_t j = 0; j <= c->bucket_idx && j < counts.size(); ++j) {
                            cumulative += static_cast<int64_t>(counts[j]);
                        }
                        if (auto* obs = opentelemetry::nostd::get_if<
                                opentelemetry::nostd::shared_ptr<
                                    opentelemetry::metrics::ObserverResultT<int64_t>>>(&result)) {
                            (*obs)->Observe(cumulative, c->labels);
                        }
                    },
                    ctx_ptr);

                m_impl->instruments.push_back({std::move(ctx), std::move(instrument), meta.name, meta.labels});
            }

            spdlog::debug("telemetry: created OTel histogram metrics for '{}' ({} buckets)",
                meta.name, boundaries.size() + 1);
            break;
        }
        }
    } catch (const std::exception& e) {
        spdlog::error("telemetry: failed to create OTel instrument for '{}': {}",
            meta.name, e.what());
    } catch (...) {
        spdlog::error("telemetry: failed to create OTel instrument for '{}': unknown error",
            meta.name);
    }
}

} // namespace composite::telemetry

#endif // COMPOSITE_USE_OPENTELEMETRY
