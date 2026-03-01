# Lean4 Plugin Layering Guide

This is the entry point for the Lean4 plugin/runtime layer documentation.

## Subdocuments

- `doc/lean4-plugin-layers/layer-stack.md`: layer-by-layer map of the current implementation.
- `doc/lean4-plugin-layers/boundary-rules.md`: boundary and dependency rules between layers.
- `doc/lean4-plugin-layers/ownership-lifecycle.md`: runtime/resource ownership model.
- `doc/lean4-plugin-layers/end-to-end-flow.md`: end-to-end execution flow through the stack.
- `doc/lean4-plugin-layers/feature-placement.md`: where to add new functionality.

## Related Planning Docs

- `doc/lean4-rpc-plan.md`: RPC parity and closure plan vs C++ behavior classes.
- `doc/lean4-kjasync-goals.md`: RPC-independent KJ async/runtime expansion goals.
- `doc/lean4-backend.md`: historical backend design sketch and invocation basics.
