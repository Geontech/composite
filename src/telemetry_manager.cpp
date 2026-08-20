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

#include "composite/metrics/metrics.hpp"
#include "composite/telemetry/manager.hpp"
#include "telemetry_boundary_format.hpp"

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
#include <format>
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
template <typename T>
struct callback_context : callback_context_base {
    T* metric;
    std::vector<std::pair<std::string, std::string>> labels;

    callback_context(T* m, std::vector<std::pair<std::string, std::string>> l) : metric(m), labels(std::move(l)) {}
};

/**
 * @brief Bucket callback context for histogram buckets
 */
struct bucket_context : callback_context_base {
    metrics::histogram* hist;
    std::size_t bucket_idx;
    std::vector<std::pair<std::string, std::string>> labels;

    bucket_context(metrics::histogram* h, std::size_t idx, std::vector<std::pair<std::string, std::string>> l)
        : hist(h), bucket_idx(idx), labels(std::move(l)) {}
};

/**
 * @brief Tracks an OTel instrument and its associated callback context
 *
 * Note: Member order matters! context must be destroyed AFTER instrument
 * to ensure the callback isn't invoked with a dangling pointer.
 * C++ destroys members in reverse order of declaration.
 */
/// Which series an instrument carries. A histogram is exported as several Prometheus-style
/// instruments, each of which groups independently.
enum class instrument_role : std::uint8_t { value, hist_count, hist_sum, hist_bucket };

/// One exported time series: a native metric plus the attributes that identify it.
struct series_entry {
    void* metric{nullptr};                                       ///< native metric; type fixed by the group
    std::size_t bucket_idx{0};                                   ///< hist_bucket only
    std::vector<std::pair<std::string, std::string>> attributes; ///< what Observe() is called with
    metrics::labels_t raw_labels;                                ///< for deregistration matching
};

/**
 * @brief One OTel observable instrument and EVERY series exported through it.
 *
 * The OTLP SDK keys instruments by NAME. Creating a second instrument with the same name — as
 * this used to do, once per label set — does not add a series: the later registration supersedes
 * the earlier one, so N components publishing the same metric collapsed to whichever registered
 * last. Histograms were worse still, creating one `_bucket` instrument per bucket, so all but one
 * bucket vanished.
 *
 * So: one instrument per name, one callback, and a list of series that the callback iterates,
 * calling Observe(value, attributes) once per series.
 *
 * `mtx` guards `series` because the callback runs on the SDK's export thread while components may
 * be registering or deregistering. It is deliberately NOT the manager's instrument_mutex: the
 * callback must never take that, or an export could deadlock against a registration.
 */
struct instrument_group {
    std::string otel_name;   ///< the name handed to the SDK (may carry a histogram suffix)
    std::string base_name;   ///< the native metric name, for deregistration
    std::string unit;        ///< as handed to the SDK; later series reusing the name inherit it
    std::string description; ///< likewise
    metrics::metric_type type{};
    instrument_role role{instrument_role::value};
    std::mutex mtx;
    std::vector<series_entry> series;

    /// The callback registered on `instrument`, kept so the destructor can retire it.
    opentelemetry::metrics::ObservableCallbackPtr callback{nullptr};

    /// DECLARED LAST so it is DESTROYED FIRST — defence in depth, not the actual guarantee.
    ///
    /// What really makes teardown safe is that the SDK's ObservableRegistry::Observe() holds its
    /// callbacks_m_ mutex across the entire callback loop, and RemoveCallback (below) takes the
    /// same mutex — so ~instrument_group blocks until any in-flight export has finished. If an SDK
    /// upgrade ever moves to a per-callback or lock-free callback list, that guarantee is gone and
    /// this declaration order is all that is left, which is NOT sufficient on its own. Treat a
    /// change to that SDK internal as a breaking change for this file.
    opentelemetry::nostd::shared_ptr<otel_metrics::ObservableInstrument> instrument;

    instrument_group() = default;
    instrument_group(const instrument_group&) = delete;
    auto operator=(const instrument_group&) -> instrument_group& = delete;
    instrument_group(instrument_group&&) = delete;
    auto operator=(instrument_group&&) -> instrument_group& = delete;

    ~instrument_group() {
        // Retire the callback EXPLICITLY rather than relying on the shared_ptr going away: the
        // meter may hold its own reference to the instrument, in which case dropping ours does
        // not stop callbacks. RemoveCallback is what actually detaches this group as state.
        if (instrument && callback != nullptr) {
            instrument->RemoveCallback(callback, this);
        }
    }
};

/**
 * @brief Implementation details for telemetry manager
 */
struct manager::impl {
    std::atomic<bool> initialized{false};
    std::mutex init_mutex; // Guards initialization to prevent races
    telemetry::config config;

    // OTel meter for creating instruments
    opentelemetry::nostd::shared_ptr<otel_metrics::Meter> meter;

    /// OUR provider, kept so shutdown() can flush it. Reaching for the global provider instead
    /// would flush whatever happens to be installed at the time, which is not necessarily ours.
    opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider> provider;

    // Observer registration IDs from the metrics registry
    std::optional<std::size_t> registration_observer_id;
    std::optional<std::size_t> deregistration_observer_id;

    // Mutex for thread-safe instrument creation/removal
    std::mutex instrument_mutex;

    // Tracked instruments with their contexts (for proper cleanup)
    /// unique_ptr so a group's address is stable: it is handed to AddCallback as the callback
    /// state and must outlive every export while the instrument exists.
    std::vector<std::unique_ptr<instrument_group>> instruments;
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
        spdlog::warn("telemetry: OTLP protocol 'grpc' requested but only HTTP is supported; "
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
    if (endpoint.size() >= path.size() && endpoint.compare(endpoint.size() - path.size(), path.size(), path) == 0) {
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
static auto labels_to_kv(const metrics::labels_t& labels) -> std::vector<std::pair<std::string, std::string>> {
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
static auto parse_headers(const std::string& header_str) -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> headers;
    if (header_str.empty()) {
        return headers;
    }

    std::size_t pos = 0;
    while (pos < header_str.size()) {
        auto comma = header_str.find(',', pos);
        auto segment = (comma != std::string::npos) ? header_str.substr(pos, comma - pos) : header_str.substr(pos);

        auto eq = segment.find('=');
        if (eq != std::string::npos && eq > 0) {
            auto key = segment.substr(0, eq);
            auto value = segment.substr(eq + 1);
            // Trim whitespace
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front())))
                key.erase(0, 1);
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
                key.pop_back();
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                value.erase(0, 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.pop_back();
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
        m_impl->initialized.store(false); // Not really initialized
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

        auto reader = sdk_metrics::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), reader_opts);

        // Create meter provider with resource attributes
        // Using OTel semantic convention attribute names directly
        auto resource_attrs = resource::ResourceAttributes{
            {"service.name", m_impl->config.service_name},
        };
        if (!m_impl->config.service_version.empty()) {
            resource_attrs["service.version"] = m_impl->config.service_version;
        }
        auto res = resource::Resource::Create(resource_attrs);

        auto provider = sdk_metrics::MeterProviderFactory::Create(std::make_unique<sdk_metrics::ViewRegistry>(), res);

        auto* sdk_provider = static_cast<sdk_metrics::MeterProvider*>(provider.get());

        // Set as global provider (convert unique_ptr to nostd::shared_ptr)
        opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider> shared_provider(provider.release());
        m_impl->provider = shared_provider;
        otel_metrics::Provider::SetMeterProvider(shared_provider);

        // Get meter for our instrumentation
        m_impl->meter = otel_metrics::Provider::GetMeterProvider()->GetMeter(
            "composite", m_impl->config.service_version.empty() ? "0.0.0" : m_impl->config.service_version);

        // READER LAST. AddMetricReader starts the periodic export thread, and that thread walks the
        // provider's meters — so attaching it before GetMeter() means the collector is running
        // against a provider whose meter is still being constructed. TSan reported exactly that
        // race (Meter::Meter on this thread vs ObservableRegistry::Observe on the export thread)
        // once the export interval was short enough for a collect to land during start-up.
        //
        // Same shape as the shutdown ordering: do not start the thing that reads until the thing it
        // reads exists, and do not retire the thing that protects a pointer until nothing can
        // dereference it. `sdk_provider` stays valid — shared_provider owns it and is held in
        // m_impl->provider.
        sdk_provider->AddMetricReader(std::move(reader));

        // Register as an observer on the native metrics registry
        // This callback is invoked for existing metrics and any future metrics
        auto& registry = metrics::registry::instance();

        // DEREGISTRATION FIRST, then registration. add_observer(notify_existing) enumerates and
        // notifies under the registry's observer mutex, so a removal racing that enumeration is
        // held until the callbacks have run — its metric stays alive meanwhile, so we register a
        // valid pointer. But that removal's deregistration notice fires the moment the mutex is
        // released, and if the deregistration observer were installed only afterwards, the notice
        // would find no observer and the series for a now-destroyed metric would be stranded.
        // Subscribing in this order means the retraction always has somewhere to land.
        // Unsubscribe again if anything below throws. add_observer(notify_existing) allocates a
        // snapshot of the whole registry and runs user callbacks, so it is not throw-free. Leaving
        // the deregistration observer behind on that path would be permanent: shutdown() and
        // ~manager both early-out on `initialized`, which is false by then, so nothing would ever
        // remove a subscription that still holds `this` — and a retry would overwrite the id and
        // orphan the first one on top of that.
        struct subscription_guard {
            manager::impl* impl;
            metrics::registry* reg;
            bool committed{false};
            ~subscription_guard() {
                if (committed) {
                    return;
                }
                if (impl->registration_observer_id) {
                    reg->remove_observer(*impl->registration_observer_id);
                    impl->registration_observer_id.reset();
                }
                if (impl->deregistration_observer_id) {
                    reg->remove_deregistration_observer(*impl->deregistration_observer_id);
                    impl->deregistration_observer_id.reset();
                }
            }
        } subs{m_impl.get(), &registry};

        m_impl->deregistration_observer_id = registry.add_deregistration_observer(
            [this](const metrics::metric_metadata& meta, void* ptr) { remove_otel_instrument(meta, ptr); });

        m_impl->registration_observer_id =
            registry.add_observer([this](const metrics::metric_metadata& meta,
                                         void* metric_ptr) { create_otel_instrument(meta, metric_ptr); },
                                  true // notify for existing metrics
            );
        subs.committed = true;

        spdlog::info("telemetry: OTLP export initialized (interval: {}ms)", m_impl->config.export_interval.count());
        return true;

    } catch (const std::exception& e) {
        spdlog::error("telemetry: failed to initialize OTLP export: {}", e.what());
        {
            const auto inst_lock = std::lock_guard{m_impl->instrument_mutex};
            m_impl->instruments.clear();
        }
        // UNDO THE PUBLICATION, not just our own handles. SetMeterProvider() has already installed
        // this provider globally by the time the reader is attached and the observers registered, so
        // dropping only m_impl's pointers left the GLOBAL provider owning a half-initialized SDK —
        // and, if AddMetricReader() succeeded before something later threw, owning a live periodic
        // export thread that keeps collecting for the rest of the process. is_initialized() would
        // report false while that thread ran on, and shutdown() early-returns on exactly that flag,
        // so nothing would ever have torn it down.
        //
        // Order matters: swap the global back to a noop FIRST so nothing new resolves to the
        // failed provider, then shut the SDK provider down (which joins its reader threads), then
        // release our reference.
        try {
            opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider> noop_provider(
                new otel_metrics::NoopMeterProvider());
            otel_metrics::Provider::SetMeterProvider(noop_provider);
            if (m_impl->provider) {
                static_cast<sdk_metrics::MeterProvider*>(m_impl->provider.get())->Shutdown();
            }
        } catch (const std::exception& inner) {
            spdlog::error("telemetry: failed to unwind a partially initialized provider: {}", inner.what());
        } catch (...) {
            spdlog::error("telemetry: failed to unwind a partially initialized provider");
        }
        m_impl->meter = opentelemetry::nostd::shared_ptr<otel_metrics::Meter>{};
        m_impl->provider = opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider>{};
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

    // ORDER MATTERS, and it is the reverse of the intuitive one. The deregistration observer is
    // what keeps a series from outliving its native metric, so it must be the LAST thing removed —
    // it has to stay installed for as long as any observable callback can still fire. Removing
    // both observers up front left a window in which a concurrent remove_counter() destroyed its
    // metric with nobody listening, while an export callback still held that raw pointer and
    // dereferenced it on the very next collect (including the ForceFlush below).
    //
    //   1. registration observer off  — no NEW series can appear from here on
    //   2. deregistration observer STAYS — retractions still land while callbacks can run
    //   3. flush                      — the final collect, callbacks still live
    //   4. clear instruments          — RemoveCallback retires each callback synchronously
    //   5. deregistration observer off — nothing can dereference a metric any more
    auto& registry = metrics::registry::instance();
    if (m_impl->registration_observer_id) {
        registry.remove_observer(*m_impl->registration_observer_id);
        m_impl->registration_observer_id.reset();
    }

    // Flush BEFORE clearing. Clearing retires every observable callback (~instrument_group calls
    // RemoveCallback), so a collect after that point observes nothing — the whole last export
    // interval, which is exactly the data that explains why a component exited, would be discarded
    // on every clean shutdown.
    if (m_impl->provider) {
        try {
            static_cast<sdk_metrics::MeterProvider*>(m_impl->provider.get())->ForceFlush();
        } catch (const std::exception& e) {
            spdlog::warn("telemetry: final flush failed: {}", e.what());
        }
    }

    // Clear instruments and their contexts (prevents memory leaks). ~instrument_group's
    // RemoveCallback serializes against the SDK's callback loop, so once this returns no callback
    // is running or can start.
    {
        auto inst_lock = std::lock_guard{m_impl->instrument_mutex};
        m_impl->instruments.clear(); // unique_ptr contexts are automatically deleted
    }

    // Only NOW is it safe to stop listening for retractions: nothing is left that could hold a
    // native metric pointer. remove_deregistration_observer() waits out any retraction already
    // executing, so this also cannot cut one short.
    if (m_impl->deregistration_observer_id) {
        registry.remove_deregistration_observer(*m_impl->deregistration_observer_id);
        m_impl->deregistration_observer_id.reset();
    }

    // Reset meter. Assignment, not .reset(): nostd::shared_ptr only grew reset() in newer
    // opentelemetry-cpp releases, and this must compile against distro packages (1.19).
    m_impl->meter = opentelemetry::nostd::shared_ptr<otel_metrics::Meter>{};
    m_impl->provider = opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider>{};

    // Replace the global provider with a noop so nothing keeps exporting after us
    opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider> noop_provider(new otel_metrics::NoopMeterProvider());
    otel_metrics::Provider::SetMeterProvider(noop_provider);

    m_impl->initialized.store(false);
    spdlog::info("telemetry: OTLP export shutdown complete");
}

namespace {

/// Observe every series in a group. One of these per (type, role); each is the single callback
/// registered on that group's instrument, and each iterates the group's series list rather than
/// closing over one metric.
template <typename Native, typename Value, typename Read>
void observe_all(opentelemetry::metrics::ObserverResult& result, void* state, Read read) {
    auto* group = static_cast<instrument_group*>(state);
    auto* observer =
        opentelemetry::nostd::get_if<opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<Value>>>(
            &result);
    if (observer == nullptr) {
        return;
    }
    const std::lock_guard lock{group->mtx};
    for (const auto& series : group->series) {
        (*observer)->Observe(read(static_cast<Native*>(series.metric), series), series.attributes);
    }
}

void observe_counter(opentelemetry::metrics::ObserverResult result, void* state) {
    observe_all<metrics::counter<uint64_t>, int64_t>(
        result, state, [](auto* m, const series_entry&) { return to_otel_int64(m->value()); });
}

void observe_updown(opentelemetry::metrics::ObserverResult result, void* state) {
    observe_all<metrics::updown_counter<int64_t>, int64_t>(result, state,
                                                           [](auto* m, const series_entry&) { return m->value(); });
}

void observe_gauge(opentelemetry::metrics::ObserverResult result, void* state) {
    observe_all<metrics::gauge<double>, double>(result, state, [](auto* m, const series_entry&) { return m->value(); });
}

void observe_hist_count(opentelemetry::metrics::ObserverResult result, void* state) {
    observe_all<metrics::histogram, int64_t>(
        result, state, [](auto* m, const series_entry&) { return static_cast<int64_t>(m->count()); });
}

void observe_hist_sum(opentelemetry::metrics::ObserverResult result, void* state) {
    observe_all<metrics::histogram, double>(result, state, [](auto* m, const series_entry&) { return m->sum(); });
}

/// Cumulative bucket values for ONE histogram, computed from ONE snapshot.
///
/// Re-reading bucket_counts() per series (the obvious shape, and what observe_all() would give)
/// takes an independent sample of a concurrently-mutating array for every bucket, so bucket `le=10`
/// can be sampled later than `le=5` and come out SMALLER. Cumulative buckets that decrease are not
/// a rounding artefact: a consumer differences adjacent buckets to get populations, so it computes
/// a negative count and histogram_quantile() returns garbage. One snapshot also turns an O(B^2)
/// summation with B allocations into O(B) with one.
void observe_hist_bucket(opentelemetry::metrics::ObserverResult result, void* state) {
    auto* group = static_cast<instrument_group*>(state);
    auto* observer = opentelemetry::nostd::get_if<
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<int64_t>>>(&result);
    if (observer == nullptr) {
        return;
    }
    const std::lock_guard lock{group->mtx};
    const void* cached = nullptr;
    std::vector<int64_t> cumulative; // prefix sums of one snapshot
    for (const auto& series : group->series) {
        if (series.metric != cached) {
            // create_otel_instrument() appends a histogram's buckets consecutively, so this
            // re-snapshots once per histogram rather than once per bucket.
            cached = series.metric;
            const auto data = static_cast<metrics::histogram*>(series.metric)->snapshot();
            cumulative.assign(data.bucket_counts.size(), 0);
            int64_t running = 0;
            for (std::size_t i = 0; i < data.bucket_counts.size(); ++i) {
                running += static_cast<int64_t>(data.bucket_counts[i]);
                cumulative[i] = running;
            }
        }
        const auto value = series.bucket_idx < cumulative.size() ? cumulative[series.bucket_idx] : 0;
        (*observer)->Observe(value, series.attributes);
    }
}

} // namespace

auto manager::group_for(const std::string& otel_name, const std::string& base_name, metrics::metric_type type,
                        int role_raw, const std::string& description, const std::string& unit) -> void* {
    const auto role = static_cast<instrument_role>(role_raw);
    for (const auto& group : m_impl->instruments) {
        if (group->otel_name != otel_name) {
            continue;
        }
        // Same OTel name — reuse ONLY if the whole schema matches. Matching on the name alone is
        // a type-confusion bug: the registry's duplicate check is per-collection, so
        // counter("shared", {a}) and gauge("shared", {b}) both exist happily, and appending the
        // gauge to the counter's group would have the counter callback static_cast a
        // gauge<double>* to counter<uint64_t>*. Histogram suffixes collide the same way — a
        // native counter named "x_count" against a histogram "x" that exports "x_count".
        if (group->type == type && group->role == role && group->base_name == base_name) {
            // UNIT MISMATCH IS A REFUSAL, not a warning. Unit is part of an instrument's identity
            // in the OTel spec, but the registry's uniqueness rule is per name+labels, so two
            // components can legitimately register the same name with different units. Every
            // series of one instrument carries the FIRST registration's unit, so appending here
            // would export the second component's milliseconds labelled as seconds — silently
            // wrong data, which is worse than an absent series. Same treatment as the type
            // collision below: refuse, and say exactly what to do about it.
            if (group->unit != unit) {
                spdlog::error("telemetry: unit collision on '{}' — existing series from '{}' are "
                              "exported with unit '{}'; refusing to add a series with unit '{}'. "
                              "One instrument carries one unit: rename the metric, or make the "
                              "units agree.",
                              otel_name, group->base_name, group->unit, unit);
                return nullptr; // add_series() is a no-op on nullptr
            }
            // Description is cosmetic — it does not change what a value MEANS — so a mismatch is
            // reported and the first one wins rather than dropping the series.
            if (group->description != description) {
                spdlog::warn("telemetry: '{}' reused with a different description (keeping '{}', "
                             "ignoring '{}').",
                             otel_name, group->description, description);
            }
            return group.get(); // one instrument per NAME — reuse it and append a series
        }
        spdlog::error("telemetry: metric name collision on '{}' — existing series come from '{}' "
                      "(type {}, role {}); refusing to add '{}' (type {}, role {}). Rename one of "
                      "them: exporting both through one instrument would be a type-confused cast.",
                      otel_name, group->base_name, static_cast<int>(group->type), static_cast<int>(group->role),
                      base_name, static_cast<int>(type), static_cast<int>(role));
        return nullptr; // add_series() is a no-op on nullptr
    }

    auto group = std::make_unique<instrument_group>();
    group->otel_name = otel_name;
    group->base_name = base_name;
    group->type = type;
    group->role = role;
    group->unit = unit;
    group->description = description;

    opentelemetry::metrics::ObservableCallbackPtr callback{nullptr};
    switch (role) {
    case instrument_role::hist_count:
        group->instrument = m_impl->meter->CreateInt64ObservableCounter(otel_name, description, "1");
        callback = &observe_hist_count;
        break;
    case instrument_role::hist_sum:
        // A monotonic Counter, matching `_count` and `_bucket`, so the whole histogram family
        // shares one aggregation temporality and a collector can reconstruct it.
        //
        // This is only sound because histogram::record() REJECTS negative, NaN and infinite
        // observations (metrics/types.hpp) — a v0.5 API guarantee. Without it the sum could
        // decrease, and a decreasing OTLP counter is read as a counter RESET: the collector adds
        // the whole new value and invents an enormous spurious rate. If that invariant is ever
        // relaxed, this must become an UpDownCounter (a Sum with is_monotonic=false) in the same
        // commit.
        group->instrument = m_impl->meter->CreateDoubleObservableCounter(otel_name, description, unit);
        callback = &observe_hist_sum;
        break;
    case instrument_role::hist_bucket:
        group->instrument = m_impl->meter->CreateInt64ObservableCounter(otel_name, description, "1");
        callback = &observe_hist_bucket;
        break;
    case instrument_role::value:
        switch (type) {
        case metrics::metric_type::counter:
            group->instrument = m_impl->meter->CreateInt64ObservableCounter(otel_name, description, unit);
            callback = &observe_counter;
            break;
        case metrics::metric_type::updown_counter:
            group->instrument = m_impl->meter->CreateInt64ObservableUpDownCounter(otel_name, description, unit);
            callback = &observe_updown;
            break;
        case metrics::metric_type::gauge:
            group->instrument = m_impl->meter->CreateDoubleObservableGauge(otel_name, description, unit);
            callback = &observe_gauge;
            break;
        case metrics::metric_type::histogram:
            break; // handled via the three roles above
        }
        break;
    }

    if (!group->instrument || callback == nullptr) {
        return nullptr;
    }
    // The group's address is the callback state, which is why groups are held by unique_ptr.
    group->callback = callback;
    group->instrument->AddCallback(callback, group.get());
    auto* raw = group.get();
    m_impl->instruments.push_back(std::move(group));
    return raw;
}

auto manager::add_series(void* group_ptr, void* metric, const metrics::labels_t& raw_labels,
                         const std::vector<std::pair<std::string, std::string>>& attributes, std::size_t bucket_idx)
    -> void {
    if (group_ptr == nullptr) {
        return;
    }
    auto* group = static_cast<instrument_group*>(group_ptr);
    const std::lock_guard lock{group->mtx};
    group->series.push_back({metric, bucket_idx, attributes, raw_labels});
}

/**
 * @brief Remove OTel instruments for a metric that was deregistered.
 *
 * Drops only the SERIES belonging to this (name, labels) pair — other components publishing the
 * same metric name keep exporting. An instrument is destroyed only once its last series goes,
 * which is also what stops its callback firing against freed metrics.
 */
auto manager::remove_otel_instrument(const metrics::metric_metadata& meta, void* metric_ptr) -> void {
    if (!m_impl->meter) {
        return;
    }
    const auto lock = std::lock_guard{m_impl->instrument_mutex};

    std::size_t dropped = 0;
    for (auto& group : m_impl->instruments) {
        // Match on the POINTER, which is what a series actually is. Name+labels does not identify
        // a metric: removing "x" and creating "x" again is legal, and the retraction for the
        // original then arrives while the replacement is live — cancelling the replacement's
        // series and leaving the dead original's in place. Pointer identity also makes the
        // type-collision case fall out for free (a refused metric is a different object) and
        // sweeps all three of a histogram's roles, which share one native metric.
        const std::lock_guard series_lock{group->mtx};
        auto it = std::remove_if(group->series.begin(), group->series.end(),
                                 [metric_ptr](const series_entry& s) { return s.metric == metric_ptr; });
        dropped += static_cast<std::size_t>(std::distance(it, group->series.end()));
        group->series.erase(it, group->series.end());
    }

    prune_empty_groups();

    if (dropped != 0) {
        spdlog::debug("telemetry: removed {} OTel series for '{}'", dropped, meta.name);
    }
}

auto manager::exported_series_counts() const -> std::pair<std::size_t, std::size_t> {
    const auto lock = std::lock_guard{m_impl->instrument_mutex};
    std::size_t series = 0;
    for (const auto& group : m_impl->instruments) {
        const std::lock_guard series_lock{group->mtx};
        series += group->series.size();
    }
    return {m_impl->instruments.size(), series};
}

/// Retire instruments that no longer carry any series. Call with instrument_mutex held.
auto manager::prune_empty_groups() -> void {
    auto empty = std::remove_if(m_impl->instruments.begin(), m_impl->instruments.end(),
                                [](const std::unique_ptr<instrument_group>& g) { return g->series.empty(); });
    m_impl->instruments.erase(empty, m_impl->instruments.end());
}

auto manager::create_otel_instrument(const metrics::metric_metadata& meta, void* metric_ptr) -> void {
    if (!m_impl->meter) {
        return;
    }

    const auto lock = std::lock_guard{m_impl->instrument_mutex};

    try {
        const auto labels = labels_to_kv(meta.labels);

        if (meta.type == metrics::metric_type::histogram) {
            auto* native = static_cast<metrics::histogram*>(metric_ptr);
            const auto& boundaries = native->boundaries();

            // ALL THREE OR NONE. A histogram is exported as three instruments, and group_for()
            // refuses per role — so an existing native counter called "x_count", or a second
            // histogram "x" with a different unit, used to knock out one role while the other two
            // registered anyway. The result is a malformed family (buckets and a sum with no
            // count, say) whose exact shape depends on registration order, and no collector can
            // reconstruct a histogram from it. Preflight all three, then commit.
            void* count_group =
                group_for(meta.name + "_count", meta.name, meta.type, static_cast<int>(instrument_role::hist_count),
                          meta.description + " (count)", "1");
            void* sum_group =
                group_for(meta.name + "_sum", meta.name, meta.type, static_cast<int>(instrument_role::hist_sum),
                          meta.description + " (sum)", meta.unit);
            // One instrument named "<name>_bucket" carrying a series per bucket, distinguished by
            // the "le" attribute. Previously each bucket created its own same-named instrument, so
            // all but one bucket were silently dropped by the SDK.
            void* bucket_group =
                group_for(meta.name + "_bucket", meta.name, meta.type, static_cast<int>(instrument_role::hist_bucket),
                          meta.description + " (bucket)", "1");
            if (count_group == nullptr || sum_group == nullptr || bucket_group == nullptr) {
                spdlog::error("telemetry: refusing to export histogram '{}' — one of its three instruments "
                              "({}_count / {}_sum / {}_bucket) collided with an existing instrument (see the "
                              "error above). Exporting a partial family is worse than exporting none: a "
                              "collector cannot reconstruct a histogram from buckets with no count. Rename "
                              "the metric, or the instrument it collides with.",
                              meta.name, meta.name, meta.name, meta.name);
                prune_empty_groups(); // drop whichever of the three we just created but never filled
                return;
            }

            add_series(count_group, metric_ptr, meta.labels, labels, 0);
            add_series(sum_group, metric_ptr, meta.labels, labels, 0);
            for (std::size_t i = 0; i <= boundaries.size(); ++i) {
                auto bucket_labels = labels;
                // SHORTEST ROUND-TRIP representation, via to_chars. to_string is fixed 6-decimal
                // and {:g} defaults to 6 significant digits — both render distinct nearby bounds
                // identically (1e-7 and 2e-7 both become "0.000000"; 1.0000000000000002 and 1.0
                // both become "1"). Two series with identical attribute sets in one group is
                // exactly the collapse this grouping rewrite exists to prevent: ObserverResultT
                // keys measurements by attribute set, so one bucket would silently overwrite the
                // other. to_chars emits the fewest digits that recover the exact double, so two
                // different doubles can never produce the same label — and ordinary bounds still
                // read as "1", "5", "10" rather than 17-digit noise.
                bucket_labels.emplace_back("le",
                                           (i < boundaries.size()) ? detail::format_boundary(boundaries[i]) : "+Inf");
                add_series(bucket_group, metric_ptr, meta.labels, bucket_labels, i);
            }

            spdlog::debug("telemetry: registered OTel histogram series for '{}' ({} buckets)", meta.name,
                          boundaries.size() + 1);
            return;
        }

        add_series(group_for(meta.name, meta.name, meta.type, static_cast<int>(instrument_role::value),
                             meta.description, meta.unit),
                   metric_ptr, meta.labels, labels, 0);
        spdlog::debug("telemetry: registered OTel series for '{}'", meta.name);
    } catch (const std::exception& e) {
        spdlog::error("telemetry: failed to create OTel instrument for '{}': {}", meta.name, e.what());
    } catch (...) {
        spdlog::error("telemetry: failed to create OTel instrument for '{}': unknown error", meta.name);
    }
}

} // namespace composite::telemetry

#endif // COMPOSITE_USE_OPENTELEMETRY
