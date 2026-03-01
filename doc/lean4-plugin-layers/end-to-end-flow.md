# Lean4 Plugin End-to-End Flow

1. `capnp compile -o lean4:...` runs the plugin frontend and emits generated Lean modules.
2. Application code imports generated modules and runtime APIs (`Capnp.Runtime`, `Capnp.KjAsync`, `Capnp.Rpc`, `Capnp.RpcKjAsync`).
3. Runtime calls pass through Lean wrappers into bridge C++ implementations.
4. Bridge C++ schedules work on KJ/Cap'n Proto runtimes and returns values/promises to Lean.
5. Test and parity harnesses validate semantics and C++ behavior parity.
