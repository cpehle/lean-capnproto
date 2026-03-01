# Lean4 golden test (capnpc-lean4)

This is a tiny golden test for the Lean4 backend.

## Generate output and compare

From the repo root:

```sh
# Build capnp + capnpc-lean4 first (CMake or Bazel).
capnp compile \
  -o lean4:test/lean4/out \
  --src-prefix . \
  -I c++/src \
  -I test/lean4 \
  test/lean4/addressbook.capnp \
  test/lean4/fixtures/defaults.capnp \
  test/lean4/fixtures/capability.capnp \
  test/lean4/fixtures/rpc_echo.capnp \
  c++/src/capnp/test.capnp \
  c++/src/capnp/rpc.capnp \
  c++/src/capnp/rpc-twoparty.capnp \
  c++/src/capnp/stream.capnp

diff -u \
  test/lean4/expected/Capnp/Gen/test/lean4/addressbook.lean \
  test/lean4/out/Capnp/Gen/test/lean4/addressbook.lean
```

## Compile with Lake

```sh
cd test/lean4
lake test
```

The `lake` project uses:
- `../../lean` for `Capnp.Runtime`
- `out/` for generated files
- `src/` for a tiny `Main.lean`

## CTest integration

If you build with CMake, you can run the same Lean4 flow with:

```sh
cmake -S . -B build -DCAPNP_ENABLE_LEAN4_TESTS=ON
cmake --build build
ctest --test-dir build -R capnp-lean4-tests -V
```

## RPC parity artifact

`test/lean4/parity_matrix.json` is the machine-readable Lean/C++ RPC behavior-class parity map
used by `doc/lean4-rpc-plan.md`.
