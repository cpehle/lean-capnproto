# Lean4 backend (design sketch)

This document outlines a lean, staged Lean4 codegen backend for Cap'n Proto using the standard
compiler plugin mechanism. The goal is a minimal serialization-only backend first, with room to
add builders, packed encoding, and RPC later.

For the current implementation layout across codegen, runtime, async, KJ, RPC, and parity tests,
see `doc/lean4-plugin-layers.md`.

## Overview

- Plugin name: `capnpc-lean4` (invoked via `capnp compile -o lean4 ...`).
- Input: `schema::CodeGeneratorRequest` on stdin (unpacked stream).
- Output: one `.lean` file per requested `.capnp` file, rooted under `Capnp/Gen/`.
- Runtime: small Lean4 library for wire-format parsing and (later) building.

## Module & file mapping

Given a schema file path like `foo/bar-baz.capnp`, the plugin generates:

- Module: `Capnp.Gen.Foo.BarBaz`
- File: `Capnp/Gen/Foo/BarBaz.lean`

Mapping rules (initial draft):
- Split path on `/` or `\\`.
- Drop `.capnp` extension.
- Sanitize each segment to a valid Lean identifier:
  - Replace non-alphanumeric characters with `_`.
  - If a segment starts with a digit, prefix `_`.
  - Optionally, apply `UpperCamel` to each segment (future improvement).

## Generated API (serialization-only first)

For each struct `Foo`:
- `Foo.Reader` backed by `Capnp.StructReader`
- `Foo.read` / `Foo.fromStruct` helpers
- `Foo.Which` for unions + `Foo.Reader.which`
- Field accessors: `Foo.Reader.getX` (data + pointer fields)

For each enum `Foo`:
- `inductive Foo` with explicit cases
- `Foo.ofUInt16 : UInt16 -> Foo` with `unknown` fallback
- `Foo.toUInt16 : Foo -> UInt16`

Lists:
- `ListReader α` with element layout typeclass for pointer vs data lists

## Runtime library (Lean4)

- `Capnp.Runtime` defines core aliases (`Text`, `Data`, `AnyPointer`, `Capability`)
- `Message` = array of segments (ByteArray)
- `StructReader` / `ListReader` / pointer decoding
- Bounds checks and safe accessors
- Optional: `StructBuilder` + allocator (phase 2)

## Milestones

1) Plugin skeleton + file/module mapping
2) Read-only runtime + struct/enum accessors
3) Builders + serialization
4) Packed encoding support
5) RPC stubs (optional)

## Build / usage

Once `capnpc-lean4` is built and on your PATH, generate Lean modules with:

```sh
capnp compile \
  -o lean4:path/to/out \
  --src-prefix path/to/repo-root \
  -I path/to/repo-root/c++/src \
  -I path/to/repo-root/test/lean4 \
  path/to/schema.capnp
```

The plugin writes files under `Capnp/Gen/` relative to the output directory.
Use `--src-prefix` to keep output module paths deterministic across machines.

## Golden test

See `test/lean4/README.md` for a small addressbook schema and a Lake build check.

## Open questions

- Pure Lean runtime vs. optional C++ FFI for performance
- Namespace strategy for imports and `using` across files
- How to encode default pointer values (embedded blobs vs. helper defs)
