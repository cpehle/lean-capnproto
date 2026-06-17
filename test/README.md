# Lean4 schema and RPC tests

This directory contains the generated-schema fixtures and Lean test drivers for
the Lean4 backend.

## Generate schema fixtures

From the repo root:

```sh
lake build capnpNativeDeps
export PATH="$PWD/extern/capnproto/build/c++/src/capnp:$PATH"
./scripts/generate-test-schemas.sh
```

Generated modules are written under `test/out/`, which is ignored by git.
If you maintain local golden files under `test/expected/`, compare them with:

```sh
diff -ru test/expected test/out
```

## Build and test

From the repo root:

```sh
lake build
lake test -- --parity-critical
```

For a broader local sweep, run `lake test` without `--parity-critical`.

## RPC parity artifact

`test/parity_matrix.json` is the machine-readable Lean/C++ RPC behavior-class
parity map used by `doc/lean4-rpc-plan.md`. Validate it with:

```sh
python3 test/scripts/validate_parity_matrix.py
```
