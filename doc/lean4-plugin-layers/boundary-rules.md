# Lean4 Plugin Boundary Rules

These rules define how code should be split between layers.

1. Layer 0 should not hard-code runtime semantics.
Keep codegen focused on schema shape and generated API signatures; runtime behavior belongs in Layers 2-6.

2. Layer 2 (`Capnp.Runtime`) stays pure Lean.
No KJ/RPC event-loop or network dependencies should leak into this layer.

3. Layer 3 (`Capnp.Async`) is the shared async vocabulary.
If two higher layers need the same lifecycle/await/cancel pattern, add it here instead of duplicating.

4. Layer 4 and Layer 5 own FFI contracts.
`@[extern ...]` signatures in Lean and exported C functions in bridge C++ must evolve together.

5. Layer 6 should only be glue.
Do not duplicate RPC or KJ logic here; keep it as composition helpers on shared runtime handles.
