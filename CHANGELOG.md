# Changelog

All notable changes to **composite** are documented here. The project follows
[Semantic Versioning](https://semver.org/).

## Versioning & ABI policy

- **Package version (SemVer).** The installed CMake package declares
  `COMPATIBILITY SameMinorVersion`, so `find_package(composite x.y)` accepts any `x.y.*` but **not**
  a different minor. Pin to the minor line you build against.
- **Component ABI version.** Loadable components carry an independent `composite_abi_version()`
  (`composite::abi_version`, currently **1**), emitted by the `COMPOSITE_REGISTER_*` macros. The loader
  refuses to call `create()` on a library whose ABI version differs from the framework's, so a stale
  `.so` fails cleanly. Rebuild components whenever the ABI version is bumped.
- **Supported toolchains (what CI actually verifies).** C++20 is required
  (`CMAKE_CXX_STANDARD 20`, `STANDARD_REQUIRED ON`). Components cross the DSO boundary as C++
  objects, so build them with the *same* compiler and standard library as the framework — the
  ABI handshake below does not detect a mismatch.
  - **Gating, on every pipeline:** **Rocky 9** with **GCC Toolset 14** and **libstdc++**, on
    **x86-64** — glibc 2.34. This is deliberately the *exact* environment the published container
    images are built in, down to the pinned `nlohmann_json` and `opentelemetry-cpp` revisions,
    because the paragraph above is only meaningful if the ABI under test is the ABI being shipped.
    Until v0.5 the gate ran on Debian trixie (GCC 14.4, glibc 2.41, Debian's opentelemetry-cpp
    1.19) while the images shipped GCC Toolset 13 and OTel 1.22 — so this section previously
    described a toolchain nothing was released against. Covered modes: Debug + `-Wall -Wextra
    -Wpedantic -Werror` with the full ctest suite (`ci` preset), Release,
    `-DCOMPOSITE_USE_OPENSSL=ON`, `-DCOMPOSITE_USE_OPENTELEMETRY=ON`, both of those under
    ASan+UBSan and TSan, `-DCOMPOSITE_USE_DPDK=ON` (compile-only — no NIC in CI), and an
    installed-package consumer build.
  - **Also gating, as a portability check:** one **Debian trixie / GCC 14.4** job building the
    default option set and running ctest. It is not the reference toolchain; it exists so a change
    that only compiles against one compiler or one distro's packaging still fails.
  - **NOT verified: arm64, or any weak-memory architecture.** There is an arm64 TSan job, but no
    arm64 runner has ever been attached, so it has never executed — with `tags: [arm64]` and
    nothing to match, it sat pending until it timed out, holding the pipeline open. It is now off
    unless `ARM64_RUNNER` is set to `"true"`, which is honest about the coverage and stops the
    stuck job. Do not read its presence in the file as evidence anything ran on arm64.
    Concretely: **every ordering claim about `park.hpp` in this changelog and in the source has
    only ever been tested on x86-64**, whose TSO hides exactly the visibility bugs those
    seq_cst pairings exist to prevent. Attaching a runner and flipping the variable should be
    expected to find real failures, not to confirm the current state.
  - **NOT verified: Clang, in any version, and libc++.** There is no Clang job in the
    pipeline. Do not read "GCC and Clang floors" in any planning document as coverage that
    exists — if Clang support is to be advertised for v0.5, a gating job has to be added
    first.
- **When the ABI version is bumped.** It is **not** a release counter — it changes whenever a
  component built against an older framework could no longer run correctly against a newer one:
  1. the `create()` / `create_args` entry-point contract changes;
  2. the layout **or vtable** of any type a component inherits from or embeds by value changes
     (`component`, `pipeline_component`, `source_component`, the port and buffer types,
     `property_set` / `config<T>`, `park_coordinator`). This includes **adding, removing, or
     reordering any `virtual` — including appending one at the end of a class**: a component's
     own overrides are addressed by vtable slot index, so appending to a base vtable silently
     shifts them. It also includes adding, removing, or reordering **any data member**, which
     changes object size and member offsets;
  3. the `COMPOSITE_REGISTER_*` macros change what they emit.
  Only additions that touch neither the vtable nor the object layout — a non-virtual member
  function, a `static` function, a free function, a new type — are ABI-neutral. When in doubt,
  bump: a false positive costs a rebuild, a false negative corrupts memory.
- **RC1 is the first stable ABI-1 baseline.** ABI 1 is *declared* at RC1, not carried forward
  from the 0.5 pre-releases. Pre-0.5 components do not export `composite_abi_version()` at all
  and are refused by the loader outright, so the 0.4 → 0.5 migration needs no bump. But 0.5
  **pre-release** components do export version 1 while predating layout changes made during
  development (the redesign itself, and the RC1 hardening, which altered both `component`'s
  vtable and its data members). The loader therefore **cannot** distinguish a pre-RC1 ABI-1
  component from an RC1 one: **every component built before RC1 must be rebuilt, and that
  rebuild is not enforceable by the handshake.** From RC1 onward the number is meaningful, and
  the criteria above govern it. If a pre-RC1 fleet cannot be rebuilt wholesale, bump to 2 at RC1
  instead and take the clean refusal.

---

## 0.5.0 — the "inverted-core" redesign

This release reworks the framework's core (lock-free data path, park-coordinated reconfiguration,
typed/reflected properties, a single component ABI, a logging facade, and an overhauled REST control
plane). The wire/JSON contract for properties is preserved, but several **C++ and REST APIs changed**.
The table below maps the old API to the new one.

### Migration: old → new

| Area | Old (pre-0.5) | New (0.5) |
|------|---------------|-----------|
| **Property values** | stringified — `"threshold": "75.5"`, `"enabled": "true"` | native typed JSON — `"threshold": 75.5`, `"enabled": true` (validated/decoded against the registered schema) |
| **`set_properties`** | list of string key/value pairs; type-converted from strings | `set_properties(const properties::json&, config_type = INITIALIZE, bool allow_unknown = false)` |
| **Configurability** | fluent `.configurability(RUNTIME)` setter | third argument: `add_property(name, ref, RUNTIME)` (default `INITIALIZE`) |
| **Per-property validation/reaction** | `.change_listener([]{ ... })` (returns bool to reject) | `.validate(fn[, "reason"])` (rejects a candidate) + `.on_change([](const json& diff){ ... })` (post-commit) |
| **`add_property_change_listener(name, fn)`** | indexed/contextual list listeners | removed — use `.on_change`/`.validate`, or `config<T>::on_apply` |
| **Struct properties** | specialize `property_traits<T>` + `ps.add(...)` | reflect with `COMPOSITE_FIELDS`/`COMPOSITE_STRUCT` and register the `config<T>` member via `add_config(cfg)` |
| **Struct reaction** | parent change listener | `config<T>::on_apply(prev, changes<T>)` (runs at the worker loop-top) + `config<T>::validate(...)` |
| **List/keyed properties** | `"prop[0]"`, `"prop[]"`, `"prop.field"` index addressing | `keyed_collection` via `add_keyed`, addressed as nested JSON under the property name (RFC-7396 merge; `null` resets/erases) |
| **`property_change_handler()`** | no-arg virtual | `property_change_handler(const properties::json& diff)`; the no-arg form is **removed** — override the diff-taking one (the `diff` argument may simply be ignored) |
| **Unknown property keys** | tolerated at config load, rejected at runtime | rejected in **both** paths. The application-level `properties` block is still broadcast to every component, but is now filtered per component to the keys that component actually defines — so a global that applies to only some components still works, while a typo in a component's own `properties` block fails the load instead of being silently discarded. A global matching *no* component is logged as a warning |
| **Failure detail** | `finish_reason` only (`completed`/`error`/`none`) | adds `component::finish_error()` and a `finish_error` field in the component's property state — the `what()` behind a `finish_reason::error`, so a control plane need not scrape the log |
| **Component factory** | multi-arity `create()` / `create(type)` / `create(id)` hand-written `extern "C"` | single `create(std::string_view id, const create_args&)` + `composite_abi_version()`, emitted by `COMPOSITE_REGISTER_SIMPLE` / `COMPOSITE_REGISTER_COMPONENT` |
| **`start()` / `stop()` overrides** | overridable (but silently bypassed by the `enabled` reconcile path) | **`final`** — hook `on_worker_start()` / `on_worker_stop()`, which run on every start/stop path; create heap components via `make_component<T>()` (stops-before-destroy deleter) |
| **Construction args** | scalar `"create_arg": "cf32"` | `"args": { "type": "cf32" }` (the scalar form still works, mapped to `{"type": ...}`) |
| **Input read** | blocking / 1 s default timeout / `blocking` overloads | `try_get()` → `std::optional` is the **canonical** read (distinguishes empty ring from a zero-length packet); `get_data()` stays for the `buffer.empty()` idiom; the no-op timeout/`blocking` overloads were **removed** |
| **Input queue** | unbounded `std::deque` + condition variable | bounded lock-free SPSC ring, **default depth 1024, drop-on-full** at the producer (+ overflow callback) |
| **Output metadata** | `send_metadata()` latched separately | metadata rides atomically with the packet — the 3rd argument of `send_data`, carried as a shared immutable `metadata_ptr` built once per change (`make_metadata`); a value-accepting convenience overload wraps per call |
| **`enabled`** | a property + mandatory `apply_lifecycle_changes()` two-step | a spec/status virtual — a RUNTIME write **is** the start/stop (immediate); `apply_lifecycle_changes()` only after an INITIALIZE write |
| **`process()` return** | `{NORMAL, NOOP, FINISH, NO_YIELD}` | adds `AWAIT_OUTPUT` (lossless backpressure via the reverse doorbell) |
| **Metric naming** | name auto-prefixed with the component id (`my_comp.packets`) | names used verbatim; identity is the auto-added `component_id` **label** |
| **TLS CLI options** | `-c, --client-certificate`, `-k, --client-key`, `-a, --certificate-authority` — all **required** in an OpenSSL build, so compiling TLS in made it compulsory | `-c, --server-certificate`, `-k, --server-key` (this server's own identity) and `-a, --client-ca` (verifies CLIENT certificates, i.e. enables mutual TLS). The long names are **removed, not aliased**: they described the server's own credentials as if they were a client's, and the previous README example fed one file in as both server identity and client trust root. TLS is now **runtime-selected** — supply both cert and key for TLS, neither for plain HTTP, exactly one is an error. Short forms `-c`/`-k`/`-a` unchanged |
| **`histogram::enable_power_of_2_lookup()`** | opt-in O(1) bucket lookup | **removed**. It had already become a no-op: the fast path was deleted for disagreeing with the binary search about which bucket a value belongs to (e.g. 1.5), leaving a method that advertised an optimization it did not perform. Lookup is O(log n); `power_of_2_boundaries()` remains as a boundary generator. Delete the call — there is no replacement and nothing to replace |
| **`histogram::record(double)`** | any `double` accepted, including negative, NaN and infinity | observations MUST be **finite and non-negative**. Invalid ones are rejected — updating neither the buckets, `count()`, nor `sum()` — and counted in the new `rejected_observations()`. Still `noexcept` and still silent (this is the `process()` hot path; a throwing metric would turn an instrumentation bug into a component fault), so check that counter. This is what makes `sum()` monotonic, which is what lets the OTLP bridge export `_sum` as a **Counter** rather than an UpDownCounter: a decreasing OTLP counter is read as a counter *reset*, so the collector adds the whole new value and fabricates an enormous rate. A single NaN was worse — `NaN + x` is NaN, so one bad observation poisoned the accumulated sum permanently |
| **`metrics::deregistration_callback`** | `void(const metric_metadata&)` | `void(const metric_metadata&, void* ptr)` — **source-breaking**; add the parameter (ignore it with an unnamed `void*` if unused). The pointer is the same one `registration_callback` handed you, and matching on it is now **required for correctness**: a name is reusable, so removing `"x"` and creating `"x"` again produces a different metric, and a consumer keying its bookkeeping by name alone will cancel the live replacement when the original's retraction arrives. Valid only for the duration of the callback |
| **REST: list/struct mutation** | `/properties/:name/items[/:index]` and `/properties/:name/fields/:field` routes | removed — `PATCH` the whole property with a partial JSON object/array (`null` resets/erases) |
| **REST: multi-component PATCH** | "atomic with rollback" across the batch | per-component atomic; **not** transactional across components — returns `207 Multi-Status` on partial failure |
| **`GET /app/components/:id/schema`** | an ARRAY of per-property descriptors, each with a `name` field, in a bespoke vocabulary (`fields`, `choices`, `unit`, `powerOfTwo`) | a single **JSON Schema 2020-12 document**: `$schema` / `type: object` / `additionalProperties: false` / `title` (the component id) / `properties` keyed by property name. Structural keywords are standard (`properties` not `fields`, `enum` not `choices`, nested `properties`/`items`); composite metadata moved to vendor extensions — `unit` → `x-composite-unit`, `configurability` → `x-composite-configurability`, `powerOfTwo` → `x-composite-powerOfTwo`. `required` is deliberately omitted so a partial `PATCH` body validates. **The 0.5 pre-releases advertised 2020-12 export but did not actually publish it — this makes the endpoint match what the docs always claimed.** A client that walked the array looking for `name` must now index `properties` by key |
| **Stopping** | `stop()` only — unbounded, and silent while it waited | adds a **bounded pair**: `component::request_stop()` (signals without joining the worker — it still takes the lifecycle lock and runs the user wake hook, so it is not instantaneous) and `component::try_stop(timeout) -> bool`, plus `application::try_stop(timeout)` returning a `not_stopped` list of ids. A `false`/non-empty return means **not torn down — do not destroy it**, and covers three cases: the lifecycle lock was unavailable, the worker did not exit, or a property write was still in flight; nothing is torn down, so a later `try_stop()`/`stop()` completes the job. The budget covers the lifecycle lock, the worker-exit wait and any in-flight property write, but cannot cover synchronous user hooks (`on_park_requested`, `on_worker_stop`). `stop()` itself is unchanged in behaviour but now REPORTS on an interval instead of waiting mutely, naming the component and what to look at |
| **`POST /app/stop`** | ran the unbounded `stop()`; one component whose `process()` never returned blocked the request forever | bounded to a shared 10s budget. `200` when everything stopped, or **`207 Multi-Status`** with a `not_stopped` list. A component lands there if its worker did not exit, its lifecycle lock was unavailable, or a property write was still in flight; in every case it was not torn down and is still registered |
| **REST: `PUT` on a single property** | `PUT` and `PATCH` both accepted on `/app/components/:id/properties/:name`, sharing one handler | **`PUT` removed; use `PATCH`** (identical behavior — the shared handler was always a merge). `PUT` was a misnomer: HTTP defines it as replace, but a `PUT` of `{"port": 5000}` onto a `{ip, port, mtu}` struct property merged and left the other fields untouched. Rather than freeze a verb that does not do what it says, it is withdrawn; it may return later as a genuine replace operation. Replace semantics are available today as `DELETE` (reset to default) followed by `PATCH` |
| **Logging** | `spdlog::logger` exposed in the public API | `composite::logger` facade (spdlog is private); levels via `composite::log_level` |

### Highlights

- **Lock-free data path** — bounded SPSC ring ports with an event-driven doorbell (forward for data,
  reverse for backpressure); move-on-last-receiver buffer transfer; direct batched ring handoff.
  Batch overflow callbacks run once with the aggregate rejected-packet count.
- **Named threads** — every thread the framework spawns carries its owner's name, so `top -H`,
  `perf` and gdb identify it: a component's worker is named after its id, and a
  `pipeline_component`'s pool workers as `<id>.wN`. Names are truncated to the 15 characters Linux
  allows (`composite/util/thread_name.hpp`); previously a longer id was rejected outright and the
  thread went unnamed.
- **Park-coordinated reconfiguration** — property writes validate-then-commit under a worker park;
  `config<T>` reactions run at the worker loop-top (no torn config/derived-state).
- **Callback failure contract (frozen for the v0.5 line).** Validation runs *before* the commit, so a
  `validate()` rejection fails the whole write and nothing is applied. Everything that runs *after*
  the commit — `config<T>::on_apply`, `property_change_handler`, `typed_property::on_change`, and a
  port's overflow callback — is **post-commit**, and the framework guarantees:
  1. **committed values stay committed.** A failing reaction never rolls back a value that is
     already live, and never turns a successful write into an error response;
  2. **no user callback unwinds across a framework boundary.** An exception from any of them is
     caught and reported at the point of failure, never propagated out of a worker thread, a
     destructor (`~component`, `auto_stop`), or graph teardown (`remove_component`, `clear`) —
     each of which would otherwise be `std::terminate`;
  3. **failures are reported, not swallowed.** Reaction and listener failures are logged against
     the component and property; contained overflow-callback failures are counted in
     `input_port_base::overflow_callback_errors()`;
  4. **a failed reaction does not skip its peers.** Each `config<T>` binding's reaction is contained
     individually, and a contained failure does not re-arm, so it cannot spin.
  Consequence to note when migrating: an `on_apply` that throws during an INITIALIZE-time config
  load is now logged and the load continues, where a pre-release 0.5 build propagated it.
- **Typed, reflected properties** — `config<T>` + `COMPOSITE_FIELDS` with per-field attributes
  (`runtime`, `range`, `unit`, `doc`, `one_of`, `power_of_two`) and JSON-Schema 2020-12 export.
- **Single component ABI** — one `create(id, create_args)` entry point with an ABI-version handshake.
- **Safe ownership by default** — `make_component<T>()` returns a `shared_ptr` whose deleter stops
  the component *before* destruction (leaf vtable intact, so the worker and its hooks tear down
  fully derived); `COMPOSITE_REGISTER_SIMPLE` builds through it, and the deleter survives the
  upcast to `shared_ptr<component>`. `component::start()`/`stop()` are `final` — subclasses hook
  `on_worker_start()`/`on_worker_stop()`, which run on every start/stop path (including the
  `enabled` reconcile, which bypassed overrides).
- **`snapshot<T>`** — atomically published, immutable config for threads the park does not quiesce
  (pipeline pool workers, receiver threads): the property handler `publish()`es, any thread
  `load()`s a `shared_ptr<const T>` that keeps the value alive while in use.
- **Connect-time copy warning** — connecting an immutable output to a mutable input is flagged
  loudly at `connect()` (every frame is deep-copied to give the consumer writable storage).
- **Self-contained package** — `find_package(composite)` works against the install **and** the build
  tree; spdlog is private, `nlohmann_json` is a public `find_dependency`.
- **Observability** — lock-free metrics registry (label-only) over REST + SSE, optional OTLP export.
- **`pipeline_component<In, Out>`** — order-preserving worker-pool base for high-throughput,
  one-in/one-out components (used by `fft`/`psd`). Self-completes: when its input reaches
  end-of-stream and its in-flight work has drained, it FINISHes and forwards EOS downstream.
- **Lifecycle & completion** — a component that reaches the end of its work self-terminates:
  - **End-of-stream is the default, not homework** — when `process()` returns `NOOP` while every
    input is drained *and* producer-closed (`inputs_at_end()`), the base auto-promotes it to
    `FINISH`, so any plain consumer self-completes at end-of-stream with no boilerplate. Opt out via
    the `finish_at_end` property. Override `on_end_of_stream()` (called just before the synthesized
    FINISH/EOS, may `send_data()`) to flush held state — a delay line's last frame, a framer's
    partial residue. Sources (no inputs) are unaffected.
  - `process()` returns `FINISH` → `on_finished(finish_reason::completed)`, and end-of-stream is
    propagated on every output port (out-of-band, so the drop-on-full ring cannot lose it); an
    unhandled `process()` throw ends the run as `finish_reason::error`.
  - `is_finished()` / `finished_reason()` and the `finished` / `finish_reason` fields in
    `property_state()` expose the terminal state; `wait_until_finished([timeout])` (on both
    `component` and `application`) blocks until the worker(s) exit — the natural join for a batch or
    file-processing run.
  - **`source_component<Out>`** — a first-class base for data sources: implement `produce()` →
    `emit` / `idle` / `done`; `done` sends EOS and FINISHes, so a finite source drives the whole
    downstream graph to completion. Paces against backpressure automatically (`AWAIT_OUTPUT`).
  - **Resilience** — an opt-in `error_restart_max` / `error_restart_backoff_ms` policy retries a
    throwing `process()` with exponential, park-interruptible backoff instead of dying on the first
    error.
  - **`application::drain_stop(timeout)`** — graceful shutdown: stop the sources, let EOS propagate
    so downstream drains and self-finishes, then hard-stop any straggler.
