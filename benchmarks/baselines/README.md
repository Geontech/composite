# Benchmark baselines

Committed JSON artifacts from the benchmark harnesses (`bench_datapath`, `bench_registry`),
captured at a tagged release on an identified machine. They are the **before** in every
before/after performance claim a release makes: a PERF item's exit gate is a delta against
the baseline for that release line, measured on a comparable machine.

## Reproducing an artifact

One command per harness, from a fresh checkout:

```bash
cmake --preset release
cmake --build build/release --target bench_datapath bench_registry -j"$(nproc)"
# --framework-commit names the PRODUCTION CODE being measured (for a release baseline, the
# release tag's commit); --harness-commit names the harness that measured it (usually HEAD).
# For an ordinary "how does my branch perform" run, both are HEAD.
./build/release/test/bench_datapath 1 --reps 5 --json datapath.json \
    --framework-commit "$(git rev-parse v0.5.1)" --harness-commit "$(git rev-parse HEAD)"
./build/release/test/bench_registry   --reps 5 --json registry.json \
    --framework-commit "$(git rev-parse v0.5.1)" --harness-commit "$(git rev-parse HEAD)"
```

Release build, quiet machine (no concurrent builds — a saturated machine produces 2x swings in
the *control* columns, which is how you detect an invalid run). The scheduled `bench:baseline`
CI job runs the same commands and uploads the artifacts without gating anything.

## File naming

`v<release>-<runner-profile>-<harness>.json` — e.g. `v0.5.1-buildhost7-datapath.json`. The
runner profile names the machine class; the artifact's `meta` block records the exact CPU
model, kernel, governor, affinity, compiler, and commit. **Only compare artifacts whose
`schema_version` matches and whose `meta` describes comparable machines.** A local capture is
a provisional baseline until a pinned CI runner recaptures it under the same name discipline.

## Reading the numbers

- Every case carries raw `samples` (one per repetition, or per trial for latency cases),
  `median`, `min`/`max`, `stddev`, and `cv` (stddev/mean).
- `p95`/`p99` appear only when the sample count supports them (>= 20 / >= 100). With a handful
  of repetitions the tail *is* the max; the schema omits the field rather than mislead.
- The `meta` block records `assertions` and `optimized` separately — **a baseline must say
  `"assertions": "disabled"` and `"optimized": true`**; the `bench:baseline` job enforces this.
  Provenance is two commits: `framework_commit` (the production code measured) and
  `harness_commit` (+ `harness_revision`, since a baseline committed alongside its harness
  cannot know the hash of the commit it lands in).
- **Tolerance bands are derived, not chosen — and only where derivable**: for a case with
  coefficient of variation `cv <= 0.15`, treat a change within `max(4 * cv, 5%)` of the
  baseline median as noise. A noisier case (several `us`-scale snapshot and wake-latency cases
  are) has no meaningful band yet — compare its median qualitatively and treat the result as
  provisional until repeated captures on a pinned runner characterize between-run variance.
  Bands are advisory throughout; do not wire them into a merge gate before that
  characterization exists.

## Updating a baseline

A new baseline is captured only at a release tag (or when the harness's `schema_version`
changes, which makes old artifacts incomparable by definition). Never overwrite a baseline to
absorb a regression — that is the thing this directory exists to catch.
