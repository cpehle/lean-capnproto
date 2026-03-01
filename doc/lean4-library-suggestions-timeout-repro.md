# Lean4 Tooling Issue Repro: `LibrarySuggestions` Heartbeat Panic During Module Export

## Summary

Compiling the large generated Lean module
`test/lean4/out/Capnp/Gen/c__/src/capnp/test.lean`
can emit deterministic panics from Lean's library-suggestion export path:

- `Lean.LibrarySuggestions.SymbolFrequency`
- `Lean.LibrarySuggestions.SineQuaNon`

The panic message reports:

- `timeout at whnf, maximum number of heartbeats (200000) has been reached`

Notably, the build still reports success (`Build completed successfully`).

## Environment

- Host: macOS arm64
- Lean: `4.27.0` (`db93fe1`)
- Lake: `5.0.0-src+db93fe1`
- Repo: `capnproto`

## Reproduction

From repo root:

```bash
cd test/lean4
rm -f .lake/build/lib/lean/Capnp/Gen/c__/src/capnp/test.olean \
      .lake/build/lib/lean/Capnp/Gen/c__/src/capnp/test.ilean \
      .lake/build/ir/Capnp/Gen/c__/src/capnp/test.c \
      .lake/build/ir/Capnp/Gen/c__/src/capnp/test.setup.json \
      .lake/build/ir/Capnp/Gen/c__/src/capnp/test.c.o \
      .lake/build/ir/Capnp/Gen/c__/src/capnp/test.c.o.export
lake build Capnp.Gen.c__.src.capnp.test
```

Observed output includes:

- `PANIC at Lean.Environment.unsafeRunMetaM Lean.LibrarySuggestions.SymbolFrequency:75:24`
- `PANIC at Lean.Environment.unsafeRunMetaM Lean.LibrarySuggestions.SineQuaNon:...`
- `Build completed successfully`

## Why This Looks Like a Lean Tooling/Performance Issue

- Panic site is in Lean core `LibrarySuggestions` export code, not Cap'n Proto RPC code.
- The generated module is large (`~9,879` lines), which increases cost of symbol-frequency and trigger-map export.
- The panic mentions heartbeat limit `200000`, while the compile command uses `-DmaxHeartbeats=2000000`.
  This suggests the export-time meta run is using a different/default heartbeat budget.

## Expected vs Actual

- Expected: no panic output for a successful build.
- Actual: panic stack traces printed to stderr, but build exits successfully.

## Suggested Investigation Directions

1. Check heartbeat configuration used by `Environment.unsafeRunMetaM` during extension export.
2. Evaluate whether `LibrarySuggestions` export should degrade gracefully instead of panic.
3. Determine whether the panic should fail the build (or be converted to warning) for consistency.
4. Profile symbol-frequency and sine-qua-non export on large generated modules.

## Current Workaround in `test/lean4`

- `lake test` now uses a fast test driver (`TestDriverRpc`) that excludes conformance tests and
  avoids the large generated module in normal RPC/backend iteration.
- The full driver remains available as `lake build test_full` (or running the `test_full` binary),
  and still reproduces this tooling issue when it must compile
  `Capnp.Gen.c__.src.capnp.test`.
