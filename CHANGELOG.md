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
| **`property_change_handler()`** | no-arg virtual | `property_change_handler(const properties::json& diff)`; the no-arg form is **deprecated** |
| **Component factory** | multi-arity `create()` / `create(type)` / `create(id)` hand-written `extern "C"` | single `create(std::string_view id, const create_args&)` + `composite_abi_version()`, emitted by `COMPOSITE_REGISTER_SIMPLE` / `COMPOSITE_REGISTER_COMPONENT` |
| **Construction args** | scalar `"create_arg": "cf32"` | `"args": { "type": "cf32" }` (the scalar form still works, mapped to `{"type": ...}`) |
| **Input read** | blocking / 1 s default timeout / `blocking` overloads | `try_get()` → `std::optional` is the **canonical** read (distinguishes empty ring from a zero-length packet); `get_data()` stays for the `buffer.empty()` idiom; the no-op timeout/`blocking` overloads were **removed** |
| **Input queue** | unbounded `std::deque` + condition variable | bounded lock-free SPSC ring, **default depth 1024, drop-on-full** at the producer (+ overflow callback) |
| **Output metadata** | `send_metadata()` latched separately | metadata rides atomically with the packet — the optional 3rd argument of `send_data` |
| **`enabled`** | a property + mandatory `apply_lifecycle_changes()` two-step | a spec/status virtual — a RUNTIME write **is** the start/stop (immediate); `apply_lifecycle_changes()` only after an INITIALIZE write |
| **`process()` return** | `{NORMAL, NOOP, FINISH, NO_YIELD}` | adds `AWAIT_OUTPUT` (lossless backpressure via the reverse doorbell) |
| **Metric naming** | name auto-prefixed with the component id (`my_comp.packets`) | names used verbatim; identity is the auto-added `component_id` **label** |
| **REST: list/struct mutation** | `/properties/:name/items[/:index]` and `/properties/:name/fields/:field` routes | removed — `PATCH` the whole property with a partial JSON object/array (`null` resets/erases) |
| **REST: multi-component PATCH** | "atomic with rollback" across the batch | per-component atomic; **not** transactional across components — returns `207 Multi-Status` on partial failure |
| **Logging** | `spdlog::logger` exposed in the public API | `composite::logger` facade (spdlog is private); levels via `composite::log_level` |

### Highlights

- **Lock-free data path** — bounded SPSC ring ports with an event-driven doorbell (forward for data,
  reverse for backpressure); move-on-last-receiver buffer transfer; batched yielding.
- **Park-coordinated reconfiguration** — property writes validate-then-commit under a worker park;
  `config<T>` reactions run at the worker loop-top (no torn config/derived-state).
- **Typed, reflected properties** — `config<T>` + `COMPOSITE_FIELDS` with per-field attributes
  (`runtime`, `range`, `unit`, `doc`, `one_of`, `power_of_two`) and JSON-Schema 2020-12 export.
- **Single component ABI** — one `create(id, create_args)` entry point with an ABI-version handshake.
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
