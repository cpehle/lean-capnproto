# Lean4 Plugin Ownership and Lifecycle

## Runtime Lifecycle

- Runtime lifecycle is explicit:
  - `Runtime.init` / `Runtime.shutdown` remain explicit on both KJ and RPC sides.

## Resource Lifecycle

- Resource lifecycle is gradually becoming ergonomic:
  - many APIs expose explicit `release`;
  - common scoped helpers (`withRelease`, `cancelAndRelease`) are centralized in `Capnp.Async`.

## Cross-Runtime Safety

- Cross-runtime safety checks are enforced in Lean wrappers:
  - mixed-handle misuse should fail fast with clear `IO.userError` messages.
