# composite

**composite** is a lightweight framework for building componentized streaming applications.
Components are independent shared libraries, loaded at runtime and wired into a data-flow graph;
the framework owns the threading, the lock-free data path, the property/configuration plane, and a
REST control plane for runtime inspection and reconfiguration.

## Features

- **Modular architecture** — build applications by composing reusable component `.so`s.
- **Lock-free data path** — bounded single-producer/single-consumer ring ports with event-driven
  (doorbell) wakeups; minimal copies via move-on-last-receiver buffer transfer.
- **Typed configuration** — properties are native, schema-validated JSON bound directly to component
  members, with optional whole-struct `config<T>` reflection.
- **Observable** — a lock-free metrics registry exposed over REST/SSE, with optional OpenTelemetry export.

## Getting Started

### Prerequisites

- [CMake](https://cmake.org/) 3.15 or higher (3.22+ recommended)
- A C++20 compiler (GCC 12+, Clang 15+)
- OpenSSL 3.0+ — only if building with `-DCOMPOSITE_USE_OPENSSL=ON`

### Build and Install

```bash
cmake -B build
cmake --build build [--parallel N]
cmake --install build
```

The install ships a relocatable CMake package and a pkg-config file; a downstream project consumes
it with `find_package(composite)` (or `pkg-config composite`) and links the imported target
`composite::composite`. `find_package(composite)` also works against the **build tree** (point
`CMAKE_PREFIX_PATH` at the build directory) without installing.

### Build Options

- `COMPOSITE_USE_OPENSSL` (default OFF) — compile a TLS REST server (requires OpenSSL 3). When ON,
  `composite-cli` requires client cert/key arguments at launch (see below).
- `COMPOSITE_USE_OPENTELEMETRY` (default OFF) — enable OpenTelemetry OTLP metrics export (requires
  opentelemetry-cpp).
- `COMPOSITE_USE_DPDK` (default OFF) — build DPDK-backed source support.

### Container Images

The repository builds two framework-only Rocky Linux 9 image roles:

- `composite:<version>` — a non-root runtime containing `composite-cli` and `libcomposite`;
- `composite:<version>-devel` — the matching SDK/toolchain for building external component modules.

The runtime and SDK are produced from one multi-stage build and separate CMake `Runtime` and `Development` install components. The official component fleet will be distributed separately by `composite-comps` on top of these exact framework images.

See [docker/README.md](docker/README.md) for local builds, runtime use, GitLab publication variables, tag behavior, and runner requirements.

## Composite CLI Application and Configuration

The framework ships `composite-cli`, which loads a JSON application file, wires the graph, starts the
components, and serves the REST control plane.

```text
Usage: composite-cli [--help] [--version] [--server VAR] [--port VAR] [--log-level VAR] config-file

Positional arguments:
  config-file              application configuration file

Optional arguments:
  -h, --help               shows help message and exits
  -v, --version            prints version information and exits
  -s, --server             REST server bind address           [default: "localhost"]
  -p, --port               REST server port                   [default: 5000]
  -l, --log-level          trace|debug|info|warning|error|critical|off  [default: "info"]
```

Additional arguments appear depending on build options:

- With `-DCOMPOSITE_USE_OPENSSL=ON` (TLS), client authentication is **required**:
  `-c, --client-certificate <file>` (required), `-k, --client-key <file>` (required),
  `-a, --certificate-authority <file>` (optional).
- With `-DCOMPOSITE_USE_DPDK=ON`: `--list-dpdk-ports` enumerates DPDK-capable ports and exits.

> The default (non-TLS) build serves **plain, unauthenticated HTTP** with a wide-open CORS policy.
> Run it on a trusted interface, or build with OpenSSL for mutual-TLS.

### JSON Configuration File

The configuration file defines the components, their properties, and how they are connected.

#### Schema Overview

Top-level keys:

1. **name** (optional, string) — application name; a default is generated if omitted.

2. **properties** (optional, object) — application-level properties applied to all components
   *before* their component-level properties. Property values are **native JSON**, typed to the
   target C++ field — the framework validates and decodes each value against the property's
   registered schema (a `bool` field takes `true`, a numeric field takes `123`/`2.5`, a list field
   takes a JSON array, an enum field takes the enum-name string). They are **not** strings.

```json
{
    "name": "my_streaming_application",
    "properties": {
        "feature_flags": { "enable_x": true }
    }
}
```

3. **components** (required, array) — each object defines a component instance:
   - **id** (required, string) — unique instance identifier; used for connections and passed to the
     component constructor.
   - **library** (required, string) — the shared library to load. A bare name (`"fft"` or
     `"libfft.so"`) is resolved via `LD_LIBRARY_PATH` and standard paths; an absolute path loads
     directly. A name without a `.so` suffix is expanded to `lib<name>.so`.
   - **properties** (optional, object) — component-specific property values (native JSON, as above).
   - **args** (optional, object) — construction-time arguments passed to the factory, chiefly the
     template `"type"` discriminator (e.g. `{ "type": "cf32" }`) — distinct from runtime
     `properties`. The legacy scalar `"create_arg": "cf32"` form is still accepted and mapped to
     `{ "type": "cf32" }`.

```json
{
    "components": [
        {
            "id": "my_component_instance",
            "library": "libmy_component.so",
            "properties": { "threshold": 123, "processing_gain": 2.5 }
        }
    ]
}
```

4. **connections** (required, array) — each object connects one output port to one input port:
   - **output** — `{ "component": "<source id>", "port": "<output port name>" }`
   - **input**  — `{ "component": "<target id>", "port": "<input port name>" }`

```json
{
    "components": [
        { "id": "sensor",    "library": "libsensor_reader.so" },
        { "id": "processor", "library": "libdata_processor.so" },
        { "id": "writer",    "library": "libfile_writer.so" }
    ],
    "connections": [
        { "output": { "component": "sensor",    "port": "raw_data" },
          "input":  { "component": "processor", "port": "data_in"  } },
        { "output": { "component": "processor", "port": "processed_data" },
          "input":  { "component": "writer",    "port": "data_in" } }
    ]
}
```

**Note:** an output port may fan out to multiple input ports, but an input port accepts exactly one
producer (fan-in is rejected at connect time — it is the single-producer precondition of the SPSC ring).

## Component Interface

### Component Loading

Components are dynamically loaded shared libraries:

- **Library path** — the `library` field resolves a bare name (via `LD_LIBRARY_PATH`/standard
  locations) or an absolute path; a missing `.so` suffix becomes `lib<name>.so`.
- **Factory ABI** — each component library exports exactly one factory plus an ABI-version handshake.
  Declare them with the `COMPOSITE_REGISTER_*` macros from `<composite/core/register.hpp>` rather than
  hand-writing the `extern "C"` block:

  ```cpp
  #include <composite/core/register.hpp>

  // Non-templated component with a `MyComp(std::string_view id)` constructor:
  COMPOSITE_REGISTER_SIMPLE(MyComp)

  // Component whose concrete type is chosen at construction (a "type" discriminator):
  COMPOSITE_REGISTER_COMPONENT([](std::string_view id, const composite::create_args& args)
                                   -> std::shared_ptr<composite::component> {
      const auto type = args.type();                 // e.g. "cf32"
      if (type == "cf32") return composite::make_component<my_fft<std::complex<float>>>(id);
      if (type == "cf64") return composite::make_component<my_fft<std::complex<double>>>(id);
      throw std::runtime_error("my_fft: unknown type '" + std::string{type} + "'");
  })
  ```

  Both macros emit the single signature the loader calls —
  `create(std::string_view id, const composite::create_args& args)` — and a
  `composite_abi_version()` symbol. The loader checks the ABI version (currently `1`) *before* calling
  `create()` and refuses a library built against an incompatible framework, so a stale `.so` fails
  cleanly instead of being called through a mismatched signature.

- **Construction args** — pass construction-time arguments in an `"args"` object on the component
  config; the factory reads them via `args.type()` / `args.value<T>("key")`. These are distinct from
  runtime `properties`.

  ```json
  { "id": "fft1", "library": "fft", "args": { "type": "cf32" } }
  ```

- **Component ID** — each instance must have a unique `id`. The factory must thread the provided `id`
  into the constructor (`composite::component(id)`); the loader **rejects** a component whose `id()`
  differs from the configured id. The `dlopen` handle is tied to the returned component's lifetime, so
  the library is unmapped only after the component is destroyed.

### Component Lifecycle

1. **Construction** — ports and properties are registered in the constructor.
2. **Initialize** — `initialize()` runs once (optional override) before the worker loop.
3. **Start** — the component's worker thread starts (if `enabled` is `true`).
4. **Process loop** — `process()` is called repeatedly on the worker thread.
5. **Stop** — the worker is joined and resources are released.

Each component runs in its own `std::jthread`, named after the component id for debugging.

`start()`/`stop()` are **`final`**. One lifecycle path (the `enabled` reconcile used by
`application::start()` and RUNTIME `enabled` writes) never calls the virtual entry points, so an
override would run on a direct `start()` but silently not on a reconcile. Subclasses that own
worker-scoped resources (a receiver thread, a worker pool) hook `on_worker_start()` /
`on_worker_stop()` instead — those run on **every** start/stop path.

**Ownership.** Create heap-allocated components with `composite::make_component<T>(args...)`: the
returned `shared_ptr`'s deleter **stops the component before destruction begins**, while the leaf
vtable and derived members are still intact — closing the "destroyed while its worker still runs"
use-after-free. The deleter survives the upcast to `shared_ptr<component>`, and
`COMPOSITE_REGISTER_SIMPLE` builds through it (custom factory lambdas should too). A
**stack-allocated** component instead declares `component::auto_stop` as its **last** data member.

#### Enabling and disabling at runtime

`enabled` is a framework **spec/status virtual**, not a stored property. It defaults to `true`. Reads
report the *desired* state; the component report also includes an observed `running` flag.

- A **RUNTIME-context** write to `enabled` **is** the start/stop action — it reconciles the worker
  immediately. There is no separate "apply" step.
- An **INITIALIZE-context** write only records the desired state; the application start sequence (or
  `apply_lifecycle_changes()`) reconciles it.
- `enabled` must be written as a JSON boolean; a non-boolean is rejected.

Disabling a component stops its worker and pauses its input ports (sets their depth to 0, so incoming
data is dropped rather than queued); re-enabling restarts the worker and restores the port depths.

```cpp
// Disable a running component (RUNTIME context -> takes effect immediately):
component->set_properties({{"enabled", false}}, composite::properties::config_type::RUNTIME);

// Re-enable it:
component->set_properties({{"enabled", true}}, composite::properties::config_type::RUNTIME);
```

> Over the REST API, `PATCH` writes are always RUNTIME-context, so writing `enabled` there
> starts/stops the component immediately.

### Process Return Values

`process()` returns a `composite::retval` that tells the worker loop what to do next:

| Return | Meaning |
|--------|---------|
| `NORMAL` | Useful work was done. The worker loops, yielding (`sched_yield`) once per `yield_interval` consecutive `NORMAL`s (default 32) so a busy component doesn't monopolize a core. |
| `NOOP` | No work was available (e.g. no input data). The worker arms the input **doorbell** and sleeps until a producer delivers data, a stop/reconfigure is requested, or the `noop_thread_delay` backstop (default 1 ms) elapses. **At end-of-stream** (every input drained *and* producer-closed) a `NOOP` is auto-promoted to `FINISH` so the component self-completes — see below. |
| `AWAIT_OUTPUT` | The component is blocked on a **full downstream output** (it paced with `can_send()`). The worker sleeps until a consumer drains the downstream ring (reverse doorbell), or the backstop elapses. Use this for lossless backpressure. |
| `FINISH` | Graceful shutdown — the worker exits the loop permanently. Synthesized automatically if `process()` throws, or when a `NOOP` coincides with end-of-stream (unless `finish_at_end` is set false). |
| `NO_YIELD` | Work was done; loop again immediately without yielding (does not count toward the `yield_interval` streak). |

The doorbell is a **latency optimization**, not the liveness mechanism: `noop_thread_delay`/`AWAIT_OUTPUT`
always carry a timeout backstop, so a missed fast-wake degrades to that delay — never a hang.

### Completion, end-of-stream, and sources

A component that reaches the end of its work **self-terminates** — the basis for batch and
file-processing pipelines that run to completion and stop on their own:

- **EOS is the default — you don't write it.** When your `process()` returns `NOOP` and every input
  has drained *and* its producer has closed (`inputs_at_end()`), the base auto-promotes that `NOOP`
  to `FINISH`. So a plain consumer that returns `NOOP` on an empty read *already* self-completes when
  the stream ends; no `inputs_at_end()` check is needed. Opt out with the `finish_at_end` property
  (set false) for the rare component that must keep running past its inputs.
- **Flushing held state.** If your component buffers data (a delay line, a framer's partial residue,
  an accumulator), override `on_end_of_stream()` — it's called once on the worker thread just before
  the synthesized `FINISH`/EOS, and may emit final packets via `send_data()`. (Distinct from
  `on_finished(reason)`, which runs later in the completion tail for **resource release / recording**
  — keep *that* one prompt; it runs before a concurrent property write can complete.)
- Returning `FINISH` explicitly (or an unhandled `process()` throw) also ends the worker loop, and
  `on_finished(reason)` is called once (`reason` is `completed` for `FINISH`, `error` for a throw).
- On a `completed` finish the framework sends **end-of-stream (EOS)** on every output port. EOS is
  out-of-band (a flag, not a ring packet), so the drop-on-full ring cannot lose it. A downstream
  input reaches `at_end()` once drained, so the next consumer self-completes in turn — completion
  propagates through the whole graph.
- `is_finished()` / `finished_reason()` (and the `finished` / `finish_reason` fields of the component
  report) expose the terminal state. `wait_until_finished([timeout])` — on both `component` and
  `application` — blocks until the worker(s) exit; the app-level form is the natural join for a batch
  run.

```cpp
composite::application app{"batch"};
// ... add components, connect ...
app.start();
app.wait_until_finished();   // returns once a finite source drives the graph to completion
app.stop();
```

**Sources.** Derive `source_component<OutBuf>` for a component with no data input: implement
`produce()` returning `emit(buffer, ts)`, `idle()` (nothing yet — back off), or `done()` (end of
stream → EOS + FINISH). Backpressure is automatic — when the downstream ring is full the base returns
`AWAIT_OUTPUT` and does not call `produce()`, so a source paces to consumption instead of dropping.
A concrete source **must** stop its worker while still fully alive — create it with
`composite::make_component<T>()` (its deleter stops before destruction), or for a stack instance
declare `component::auto_stop` as its **last** data member (the base cannot do this for you, since
`produce()` is pure).

**Resilience.** Set the `error_restart_max` / `error_restart_backoff_ms` properties to retry a
throwing `process()` with exponential, stop-interruptible backoff instead of finishing on the first
error. **Graceful shutdown:** `application::drain_stop(timeout)` stops the sources and lets EOS drain
the graph to completion, then hard-stops any straggler.

## Ports

Ports transfer time-stamped, contiguous data buffers (and optional per-packet metadata) between
components over a **bounded, lock-free, single-producer/single-consumer ring**.

Each port is an `input_port<BufferType>` or `output_port<BufferType>`, where `BufferType` is one of:
- `mutable_buffer<T>` — exclusive-ownership, move-only buffer
- `immutable_buffer<T>` — shared-ownership, read-only buffer

### Buffer Types

**`mutable_buffer<T>`** — exclusive, **move-only** (the copy constructor is deleted; use `.copy()` for
a deep copy). Supports in-place modification via `operator[]`/iterators/`as_span()`. Can be promoted to
an immutable buffer with `std::move(buf).to_immutable()` (rvalue-qualified — it empties the source).
When backed by a dynamic container it supports `resize`/`reserve`/`capacity`/`shrink_to_fit`/`clear`
(these throw on an empty or non-dynamic buffer); `truncate(n)` logically shrinks (it cannot grow).

**`immutable_buffer<T>`** — read-only, reference-counted (cheap to copy — a refcount bump). Ideal for
fan-out. `share()` returns a zero-copy alias; `slice(offset, count)` / `slice_from(offset)` are
zero-copy views over the same storage (use `immutable_buffer<T>::npos` for "to end").

**`aligned_mem<T>`** — a SIMD-aligned backing store (requires `T` trivially copyable and trivially
destructible). Construct buffers over it with `make_aligned_buffer<T>(alignment, count)` /
`make_aligned_immutable_buffer<T>(alignment, count)`; the alignment must be a power of two and at least
`alignof(std::max_align_t)`.

Factory helpers (`<composite/buffers/buffer.hpp>`): `make_mutable<T>(size)` / `make_mutable<T>({...})`,
`make_immutable<T>(size)` / `make_immutable<T>({...})`, `wrap_mutable`/`wrap_immutable`, and the
aligned variants above.

### Output Port

`output_port<BufferType>` publishes time-stamped buffers (with optional metadata) to one or more
connected input ports.

```cpp
out.send_data(std::move(buffer), ts);                 // no metadata
out.send_data(std::move(buffer), ts, md_ptr);         // shared metadata rides with the packet
out.send_batch(buffers_span, ts, md_ptr);             // amortized publish for a single consumer

// Build the shared instance ONCE per metadata change, not per packet:
composite::metadata md;
md.sample_rate = 1e6;
auto md_ptr = composite::make_metadata(std::move(md));  // shared_ptr<const metadata>
```

- **Single-producer:** `send_data`/`send_batch` must be called from one thread only (the component's
  worker). The send path reads a lock-free cached connection snapshot; concurrent senders on the same
  output are a data race. (Wiring/introspection methods *are* thread-safe — see below.)
- **Minimal copies (move-on-last):** for the common 1:1 case the buffer (and its metadata) is *moved*
  to the receiver — zero copies. On fan-out, earlier receivers get a share/copy and the last receiver
  gets the move. An immutable→mutable hop deep-copies **every frame** (flagged with a
  connect-time warning); a mutable→immutable hop promotes in place.
- **Metadata** travels atomically with its packet as the third argument — there is no separate
  "send metadata" call and no metadata/data race. It is carried as a `composite::metadata_ptr`
  (`shared_ptr<const metadata>`): the producer rebuilds the instance only when a field changes,
  every packet in between attaches the same pointer (a refcount bump, not a map copy), fan-out
  receivers share one instance, and consumers detect "unchanged" by pointer identity instead of
  a deep compare. A convenience overload still accepts a plain `metadata` value and wraps it
  (one allocation per call) — fine for tests, avoid on hot paths.
- **Backpressure:** `can_send()` returns `true` if at least one connected input has capacity. Pair it
  with the `AWAIT_OUTPUT` return value for lossless pacing.

#### Connection Management (thread-safe)

`connect()` is normally driven by the application graph, but ports also support runtime
reconfiguration, all under a mutex and safe to call while data flows:

```cpp
bool was = out.disconnect(&input1);    // disconnect one; false if it wasn't connected
std::size_t n = out.disconnect();      // disconnect all; returns the count
out.is_connected();  out.is_connected_to(&input2);
out.connection_count();  out.connected_ports();   // introspection
```

### Input Port

`input_port<BufferType>` receives packets into a **bounded lock-free SPSC ring**.

- **Default depth is 1024** (rounded up to a power of two); configurable per port and at runtime via
  `depth(std::size_t)`. The ring only grows while empty; raising the depth at runtime adjusts a soft
  limit, it never reallocates a live ring.
- **Drop-on-full at the producer:** when the ring is full, `send_data` drops the packet (it does
  **not** block and the queue is **not** unbounded), increments `packets_dropped`, and fires the
  overflow callback. Use `can_send()`/`AWAIT_OUTPUT` upstream for lossless flow.
- `depth(0)` pauses the port — every packet is dropped (used by `enabled=false`).

#### Receiving data

`try_get()` is the **canonical non-blocking read**. It returns
`std::optional<std::tuple<BufferType, timestamp, metadata_ptr>>`: `std::nullopt` when the
ring is empty, otherwise the packet — even one carrying a genuine zero-length buffer (the
`metadata_ptr` is `nullptr` when the packet carries no metadata):

```cpp
auto pkt = in.try_get();
if (!pkt) {
    return composite::retval::NOOP;   // empty ring — idle on the doorbell (base auto-FINISHes at EOS)
}
auto& [buffer, ts, metadata] = *pkt;
if (metadata.has_value()) { /* use *metadata */ }
```

> `get_data()` also exists and returns the tuple directly, signalling empty with an empty buffer
> (`size() == 0`) — prefer `try_get()`, which distinguishes an empty ring from a real zero-length
> packet. The worker, not the port, handles idle backoff (via the doorbell / `noop_thread_delay`),
> so there is no blocking read; the old no-op `get_data(timeout)` / `get_data(blocking)` overloads
> were removed.

A batch consumer can drain several packets with a single ring advance: `get_batch(std::span<...> out)`
returns the number moved.

#### Statistics, backpressure, overflow

Both port directions expose `stats()` — `packets_transferred()`, `packets_dropped()`,
`bytes_transferred()`, `throughput_mbps()`, `drop_rate()`, `time_since_last_activity()` — and
`reset_stats()`. (Live queue depth is published separately as the `composite.port.queue_depth`
metric gauge, not via `stats()`.) Input ports expose `is_full()` / `available_capacity()`, and
`set_overflow_callback([](std::size_t dropped){ ... })` to be notified of drops. A batch overflow
calls the callback once with the aggregate number rejected, rather than once per packet.

### External Egress

There is no transport machinery on output ports. Publishing data outside the application (to NATS,
ZeroMQ, UDP, a file, etc.) is modeled as an ordinary **sink component** with an input port, wired into
the graph like any other connection. This keeps the hot path free of any per-send transport branch,
makes egress opt-in per graph, and lets an egress backend be developed and tested as a normal
component.

## Properties and Configuration

Properties are **native, schema-validated JSON bound to component members**. A property's value lives
in the member you register; the framework validates and commits writes, runs reactions, and exposes
the value over config and REST. There are three authoring styles, all registered in the constructor
and all coexisting in one component: per-property `add_property`, whole-struct `add_config<T>`, and
keyed-map `add_keyed`.

### Configurability: INITIALIZE vs RUNTIME

Every registration takes a configurability argument (default `INITIALIZE`):

- `composite::properties::config_type::INITIALIZE` (alias `initialize`) — settable only during
  initialization (config-file load / `set_properties(..., INITIALIZE)`). A runtime write to an
  INITIALIZE-only property is rejected (`config_violation`).
- `composite::properties::config_type::RUNTIME` (alias `runtime`) — settable while the component runs.

The REST control plane always writes in **RUNTIME** context, so only `RUNTIME` properties/fields are
wire-writable on a live component.

### (a) Per-property: `add_property`

`add_property(name, member_ref, configurability = INITIALIZE)` binds a member and returns a
`typed_property<T>&` for fluent configuration:

```cpp
class MyComponent : public composite::component {
public:
    explicit MyComponent(std::string_view id) : composite::component(id) {
        using enum composite::properties::config_type;

        add_property("threshold", m_threshold, RUNTIME)
            .units("dB")
            .validate([](const std::int32_t& v) { return v >= 0 && v <= 100; },
                      "threshold must be in [0, 100]")
            .on_change([this](const composite::properties::json& /*diff*/) {
                logger()->info("threshold is now {}", m_threshold);
            });

        add_property("api_key", m_api_key);          // std::optional<std::string>, INITIALIZE-only
    }
private:
    std::int32_t m_threshold{};
    std::optional<std::string> m_api_key{};
};
```

- **`.validate(fn)` / `.validate(fn, "reason")`** — the validator runs on a *candidate* value before
  the live member is touched. Returning `false` rejects the write (nothing mutates); the optional
  reason is surfaced in the error. The member already holds the candidate value inside the validator.
- **`.on_change(fn)`** — a post-commit listener receiving the property's own JSON diff. A throwing
  listener is logged as a warning (success-with-warnings), not turned into an error — the value is
  already live.
- **`.units("...")`** — an informational unit string for schema/introspection.
- `T` may be a scalar, `bool`, `std::string`, an enum (declared with `COMPOSITE_ENUM`),
  `std::optional<T>`, `std::vector<T>`, or a reflected struct. The type is deduced from the member.

### (b) Whole-struct: `config<T>` + `COMPOSITE_FIELDS`

Group related fields into one reflected struct. Each field is projected as a top-level property (the
wire format is unchanged), but the **whole struct is the validate/commit unit**, and you read fields
through a zero-overhead `operator->`.

```cpp
struct AmpConfig {
    double      gain{1.0};
    std::uint32_t fft_size{1024};
    std::string window{"hann"};

    COMPOSITE_FIELDS(AmpConfig,
        (gain,     runtime, range(0.0, 10.0), unit("dB"), doc("output gain")),
        (fft_size, runtime, power_of_two, range(64, 65536)),
        (window,   runtime, one_of("hann", "hamming", "blackman_harris")));
};

class Amp : public composite::component {
public:
    explicit Amp(std::string_view id) : composite::component(id) {
        // Set whole-struct invariants and the reaction BEFORE add_config():
        m_cfg.validate([](const AmpConfig& c) { return c.fft_size >= 64; }, "fft_size too small");
        m_cfg.on_apply([this](const AmpConfig& prev, const composite::changes<AmpConfig>& ch) {
            if (ch.changed(&AmpConfig::fft_size)) { rebuild_window(); }   // runs at worker loop-top
        });
        add_config(m_cfg);                 // baseline INITIALIZE; per-field `runtime` opts in
    }

    auto process() -> composite::retval override {
        const double g = m_cfg->gain;      // zero-overhead live read
        // ...
        return composite::retval::NORMAL;
    }
private:
    composite::config<AmpConfig> m_cfg;
    void rebuild_window();
};
```

Per-field attributes (inside the `COMPOSITE_FIELDS(...)` parentheses): `runtime` (makes that field
RUNTIME-configurable), `range(lo, hi)`, `unit("...")`, `doc("...")`, `power_of_two`,
`one_of("a", "b", ...)`. An attribute on an incompatible field type is a **compile error**.

The reaction `on_apply(prev, changes<T>)` queries what changed: `ch.changed(&T::field)` (the member
pointer is compile-checked) and `ch.new_value(&T::field)` (the committed value, or `nullopt` if
unchanged). **Important:** `on_apply` does **not** run on the writer's thread — it is staged and runs
at the **worker loop-top** (the same thread as `process()`), so derived state is rebuilt safely before
the next `process()`. When there is no live worker (initialization, a stopped component, a source with
no loop), it runs inline. Do not assume the reaction has already executed when a `set_properties` /
PATCH returns on a running component.

> `COMPOSITE_STRUCT(T, ...)` is the simpler reflection macro (no per-field attributes). Use
> `COMPOSITE_FIELDS_EXTERN` in the type's namespace for a struct you can't edit. Apply exactly one
> reflection macro per type.

### (c) Keyed collections: `add_keyed`

`add_keyed(name, std::map<std::string, E>& member, configurability = INITIALIZE)` registers a map of
reflected structs, addressed as nested JSON under the property name. It extends RFC-7396 over the map:
a key set to `null` erases it, a key set to an object upserts/merges, and a top-level `null` clears the
whole map.

### Setting values, merge semantics, and reactions

`set_properties(const json& values, config_type = INITIALIZE, bool allow_unknown = false)` applies a
batch of values. It is **native JSON in** (not strings), **validate-all-then-commit-all** per
component (a rejected value aborts the batch and nothing is committed — there is no post-commit
rollback), and runs under a worker park so `process()` never observes a half-applied state.

Writes follow **RFC-7396 JSON Merge Patch**: a nested object merges, an array or scalar replaces
wholesale, and **`null` resets a field to its registered default** (re-running the member's
initializer — not zero). Unknown fields are rejected with a "did you mean" hint.

A component can react to a completed batch by overriding:

```cpp
auto property_change_handler(const composite::properties::json& diff) -> void override {
    // `diff` is the aggregate {property: sub-diff} of everything that changed this batch.
    if (diff.contains("threshold")) { /* react narrowly */ }
}
```

(The no-argument `property_change_handler()` is deprecated; prefer the `diff` form. For `config<T>`,
prefer `on_apply` over this hook.)

### Runtime Property Control via REST API

`composite-cli` serves a REST control plane (default `localhost:5000`; configure with `--server` /
`--port`). All routes are under `/app`. Values are **native JSON** — `2.5`, `true`, `"hann"` — not
stringified.

**Application & components**

| Method & path | Description |
|---|---|
| `GET /app/healthz` | Liveness — always returns `200`. |
| `GET /app/openapi.json` | OpenAPI 3.1 description of this control plane (for client/UI generation). |
| `GET /app` | Full application graph: `{ name, components, connections }`. |
| `POST /app/start` | Start (reconcile to desired-`enabled`) every component. |
| `POST /app/stop` | Stop every component's worker (the server keeps running). |
| `GET /app/components` | Array of component documents. |
| `POST /app/components` | Create + add a component at runtime (`{library, id, properties?}`). `201` on success; `409` on duplicate id. |
| `GET /app/components/:id` | One component document (`404` if unknown). |
| `DELETE /app/components/:id` | Stop, disconnect, and unload a component (`404` if unknown). |
| `PATCH /app/components/:id` | Set a batch of `{ "properties": { ... } }` on one component (atomic per component). |
| `PATCH /app/components` | Multi-component batch `{ "components": [ { id, properties }, ... ] }`. Per-component atomic but **not** transactional across components — returns **`207 Multi-Status`** if any component fails, with a per-component `results` array. |

**Properties**

| Method & path | Description |
|---|---|
| `GET /app/components/:id/schema` | A single **JSON Schema 2020-12** document for the component's properties. Names are the keys of `properties`; standard keywords (`type`, `default`, `minimum`/`maximum`, `enum`, `description`, nested `properties`/`items`) carry the shape, and composite-specific metadata rides as vendor extensions (`x-composite-unit`, `x-composite-configurability`, `x-composite-powerOfTwo`). `additionalProperties` is `false`; `required` is omitted so a partial `PATCH` body validates. Drives auto-generated config UIs. |
| `GET /app/components/:id/properties` | Full property state. |
| `GET /app/components/:id/properties/:name` | One property value. |
| `PATCH /app/components/:id/properties/:name` | Merge one property (RFC-7396). Body is the raw JSON value, or `{ "value": ... }`. For a struct property a partial object patches only the named fields. `PUT` is **not** offered: this operation is a merge, and aliasing `PUT` to it would contradict PUT-as-replace. Use `DELETE` then `PATCH` for replace semantics. |
| `DELETE /app/components/:id/properties/:name` | Reset to the registered default (RFC-7396 `null`). |

To mutate an element of a list/struct/keyed property, `PATCH` the whole property with a partial JSON
object or array (`null` resets/erases). There are **no** `/items` or `/fields` sub-routes.

**Ports & connections**

| Method & path | Description |
|---|---|
| `GET /app/components/:id/ports` | List ports with connection status. |
| `GET /app/components/:id/ports/:port_name` | One port's details. |
| `POST /app/connections` | Create a connection (`{output:{component,port}, input:{component,port}}`). `201` on success. |
| `DELETE /app/connections` | Remove a specific connection (same body shape as POST). |
| `DELETE /app/components/:id/ports/:port_name/connections` | Disconnect all of a port's connections. |

**Status codes & limits.** `201` on create; `207` on a partially-failed multi-component batch;
`403` for a write to a non-runtime property (`config_violation`); `404` for an unknown property or
component; `400` for a validation/decoding error; `409` for a duplicate component id; `500` otherwise.
Request bodies are capped at 8 MiB; CORS is `*`. Only `RUNTIME` properties are writable over REST.

```bash
# Inspect
curl -s http://localhost:5000/app/healthz
curl -s http://localhost:5000/app/components/my_component/properties

# Set one property (raw value, or {"value": ...} — both accepted) — note: native JSON, not strings
curl -s -X PATCH http://localhost:5000/app/components/my_component/properties/threshold \
  -H 'Content-Type: application/json' -d '75.5'

# Reset a property to its default
curl -s -X DELETE http://localhost:5000/app/components/my_component/properties/threshold

# Update several properties on one component atomically
curl -s -X PATCH http://localhost:5000/app/components/my_component \
  -H 'Content-Type: application/json' \
  -d '{"properties": {"threshold": 75.5, "enabled": true}}'

# Multi-component batch (may return 207 Multi-Status with a per-component results array)
curl -s -X PATCH http://localhost:5000/app/components \
  -H 'Content-Type: application/json' \
  -d '{"components": [{"id":"src","properties":{"rate":500}},
                      {"id":"snk","properties":{"gain":2}}]}'

# Connect two ports (201 Created)
curl -s -X POST http://localhost:5000/app/connections \
  -H 'Content-Type: application/json' \
  -d '{"output": {"component": "src", "port": "data_out"},
       "input":  {"component": "snk", "port": "data_in"}}'
```

A TLS build (`-DCOMPOSITE_USE_OPENSSL=ON`) requires client cert/key at launch and `https://`:

```bash
composite-cli app.json -c client.crt -k client.key -a ca.crt
curl --cert client.crt --key client.key --cacert ca.crt https://localhost:5000/app/healthz
```

## Metrics

The framework includes a lock-free metrics registry for observability, always available and
independent of build options (OpenTelemetry is one optional export path).

### Model: label-only

Metric **names are global and used verbatim** — they are **not** prefixed with the component id.
A component's identity is carried in an automatically-added **`component_id` label**, so several
components can share a metric name (e.g. `packets_sent`) and be told apart by label. A metric's
identity is its name plus its (order-independent) label set. On component destruction, the framework
removes that component's metrics by label.

> The only dotted metric names are the framework's own lifecycle metrics
> (`composite.component.process_calls`, `noop_count`, `process_time`, `state`).

### Metric types

| Type | Use | Operations |
|------|-----|-----------|
| `counter<uint64_t>` | monotonic totals | `inc()`, `add()`, `++`, `+=` |
| `updown_counter<int64_t>` | non-monotonic sums | `inc()`, `dec()`, `add()`, `++`, `--`, `+=`, `-=` |
| `gauge<double>` | point-in-time values | `set()`, `=` |
| `histogram` | distributions | `record()` |

All instances are cache-line aligned (no false sharing) and use relaxed atomics — recording is
allocation-free and safe to call concurrently. (`histogram::reset()` is not safe against concurrent
`record()`.)

### Using metrics in a component

Register at construction (not on the hot path); cache the returned reference; record in `process()`:

```cpp
class MyComponent : public composite::component {
public:
    explicit MyComponent(std::string_view id) : composite::component(id) {
        // Name is used verbatim; a {"component_id", id} label is added automatically.
        m_packets = &create_counter("packets_processed", "Total packets processed", "1");
        m_latency = &create_histogram_pow2("processing_time_ns", "Processing time", "ns", 20);
        // Extra labels may be passed as the last argument: create_counter(..., {{"port","eth0"}});
    }

    auto process() -> composite::retval override {
        const auto start = std::chrono::steady_clock::now();
        // ... process ...
        ++(*m_packets);
        m_latency->record(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        return composite::retval::NORMAL;
    }
private:
    composite::metrics::counter<std::uint64_t>* m_packets{};
    composite::metrics::histogram* m_latency{};
};
```

Convenience methods on `component`: `create_counter`, `create_updown_counter`, `create_gauge`,
`create_histogram(name, desc, unit, boundaries, labels)`, and
`create_histogram_pow2(name, desc, unit, num_buckets, labels)`.

`create_histogram_pow2` pre-computes power-of-two boundaries (1, 2, 4, …, `2^(num_buckets-2)`);
`num_buckets` is in `[2, 64]` (default 20 → top boundary `2^18 = 262144`). Bucket lookup is the same
O(log n) binary search as any histogram — the powers of two are just convenient boundaries.

### Metrics REST API

```bash
# Snapshot of all metrics (optionally filtered)
curl -s "http://localhost:5000/app/metrics"
curl -s "http://localhost:5000/app/metrics?prefix=composite.component"
curl -s "http://localhost:5000/app/metrics?label_key=component_id&label_value=processor1"

# Server-Sent Events stream (interval ms in [100, 60000], default 1000; max 8 concurrent streams)
curl -N "http://localhost:5000/app/metrics/stream?interval=2000"
```

Each metric serializes as `{ name, description, unit, type, labels, value, timestamp }` (timestamps
are ISO-8601 UTC). For a histogram, `value` is `{ count, sum, boundaries, bucket_counts }`. The
envelope is `{ timestamp, metrics: [...], count }`.

### Registry query API

```cpp
auto& registry = composite::metrics::registry::instance();
auto all  = registry.snapshot_all();
auto some = registry.snapshot_by_prefix("composite.component.");
auto byl  = registry.snapshot_by_label("component_id", "processor1");
auto n    = registry.metric_count();
```

### OpenTelemetry Export

Built with `-DCOMPOSITE_USE_OPENTELEMETRY=ON`, metrics can be pushed via OTLP. Configure under a
`telemetry` key in the application JSON:

```json
{
  "telemetry": {
    "enabled": true,
    "service_name": "my_streaming_app",
    "export_interval": 10000,
    "exporter": { "endpoint": "http://otel-collector:4318", "protocol": "http/protobuf" }
  }
}
```

`protocol` is `http/protobuf` (default), `http/json`, or `grpc` (auto-mapped to `http/protobuf`,
port 4317→4318). The standard `OTEL_*` environment variables are honored as fallbacks
(`OTEL_SERVICE_NAME`, `OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_EXPORTER_OTLP_PROTOCOL`,
`OTEL_EXPORTER_OTLP_TIMEOUT`, `OTEL_EXPORTER_OTLP_HEADERS`, `OTEL_METRIC_EXPORT_INTERVAL`); the JSON
configuration takes precedence.

## Implementing a Component

A minimal component subclasses `composite::component`, registers its ports and properties in the
constructor, implements `process()`, and registers a factory. A working, CI-built version of this
lives in [`examples/passthrough_gain/`](examples/passthrough_gain).

```cpp
// component.hpp
#include <composite/core/component.hpp>

class passthrough_gain : public composite::component {
public:
    explicit passthrough_gain(std::string_view id);
    ~passthrough_gain() override = default;
    auto process() -> composite::retval override;

private:
    composite::input_port<composite::mutable_buffer<float>>  m_in{"data_in"};
    composite::output_port<composite::mutable_buffer<float>> m_out{"data_out"};
    float m_gain{1.0F};

    // MUST be the last data member: its destructor stops the worker first, while the
    // members process() touches are still alive (otherwise: destruction-order use-after-free).
    composite::component::auto_stop m_auto_stop{*this};
};
```

```cpp
// component.cpp
#include "component.hpp"
#include <composite/core/register.hpp>

passthrough_gain::passthrough_gain(std::string_view id) : composite::component(id) {
    using enum composite::properties::config_type;
    add_port(&m_in);
    add_port(&m_out);
    add_property("gain", m_gain, RUNTIME)
        .units("factor")
        .validate([](const float& g) { return g >= 0.0F; }, "gain must be >= 0");
}

auto passthrough_gain::process() -> composite::retval {
    using enum composite::retval;

    auto pkt = m_in.try_get();           // canonical read: nullopt == empty ring
    if (!pkt) { return NOOP; }           // idle on the doorbell; base auto-FINISHes at end-of-stream
    auto& [buffer, ts, metadata] = *pkt;

    for (std::size_t i = 0; i < buffer.size(); ++i) { buffer[i] *= m_gain; }

    // Metadata (if any) rides atomically with the packet as a shared immutable instance;
    // forwarding it is a refcount bump.
    m_out.send_data(std::move(buffer), ts, metadata);
    return NORMAL;
}

// Emits the create(id, args) ABI + the composite_abi_version() handshake.
COMPOSITE_REGISTER_SIMPLE(passthrough_gain)
```

**Factory registration:**
- `COMPOSITE_REGISTER_SIMPLE(Class)` — for a component with a `Class(std::string_view id)` constructor
  and no construction-time discriminator.
- `COMPOSITE_REGISTER_COMPONENT(factory)` — when the concrete type is chosen at construction:
  `factory` is any callable `(std::string_view id, const composite::create_args& args) ->
  std::shared_ptr<composite::component>` that inspects `args.type()` / `args.value<T>("key")` and
  returns the right instance (throw on an unknown type — the loader turns it into a clean load
  failure).

Both macros also emit `composite_abi_version()`; never hand-write the `extern "C" create` block.

### Building a component as a loadable module

A component is a CMake `MODULE` library linking the imported `composite::composite` target:

```cmake
cmake_minimum_required(VERSION 3.20)
project(passthrough_gain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(composite REQUIRED)

add_library(passthrough_gain MODULE component.cpp)   # -> libpassthrough_gain.so (dlopen'd at runtime)
target_link_libraries(passthrough_gain PRIVATE composite::composite)

include(GNUInstallDirs)
install(TARGETS passthrough_gain LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
```

### High-throughput components: `pipeline_component`

For one-in/one-out components that benefit from an internal worker pool with **order-preserving**
output, derive from `pipeline_component<InBuf, OutBuf>` instead of `component`. It owns the pool and a
slot ring that re-serializes out-of-order completions back to submission order; `process()` is `final`.
Override three hooks instead:

- `prepare(metadata&)` — main thread, arrival order (e.g. stamp annotations). Metadata is shared
  across packets, so this runs only when it actually needs rebuilding: when the incoming packet
  carries a different metadata instance than the previous one, or after
  `invalidate_prepared_metadata()` (call that from your property handler when `prepare()` stamps
  values derived from config). Packets in between share the prepared result.
- `work(InBuf in, timestamp ts, const metadata& md) -> OutBuf` — runs concurrently on the pool.
- `finalize(OutBuf& out, timestamp ts, const metadata& md) -> bool` — main thread, submission
  order; return `false` to drop the packet. Metadata is read-only here — annotate in `prepare()`.

`num_workers` is auto-registered as a RUNTIME property (range 1..1024); changing it drains the pool and
rebuilds it at the new size. Because only the main thread sends, the single-producer invariant holds.
The `fft` and `psd` components in `composite-comps` are built on it.

Pool workers run **concurrently with property commits** — the park quiesces only the main worker —
so `work()` must not read live config members (torn scalars; a freed string/vector is a
use-after-free). Publish an immutable value through **`composite::snapshot<T>`**
(`<composite/properties/snapshot.hpp>`) from your property handler and `load()` it in `work()`: the
returned `shared_ptr<const T>` keeps that value alive for as long as the worker holds it, however
many times the publisher has since moved on. The same applies to any thread the park does not
quiesce (e.g. a source's receiver threads).

## Versioning & ABI

- The package is versioned with **SemVer**; the installed CMake config declares
  `COMPATIBILITY SameMinorVersion`, so `find_package(composite x.y)` accepts the same `x.y` line.
- The component **ABI version** is independent and currently `1` (`composite::abi_version`). The loader
  refuses any `.so` whose `composite_abi_version()` doesn't match. Rebuild components against the
  framework when the ABI version changes.

See [CHANGELOG.md](CHANGELOG.md) for the 0.5 API migration table (old → new) and release notes.
