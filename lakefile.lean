import Lake
open System
open Lake DSL

def capnpBridgeLinkArgs : Array String :=
  if System.Platform.isOSX then
    #[
      "-L/opt/homebrew/lib",
      "-L/usr/local/lib",
      "-L/opt/homebrew/opt/openssl@3/lib",
      "-L/usr/local/opt/openssl@3/lib",
      "-lcapnp-rpc", "-lcapnp", "-lkj-http", "-lkj-gzip", "-lkj-tls", "-lkj-async", "-lkj",
      "-lssl", "-lcrypto", "-lz", "-lc++"
    ]
  else
    #[
      "-lcapnp-rpc", "-lcapnp", "-lkj-http", "-lkj-gzip", "-lkj-tls", "-lkj-async", "-lkj",
      "-lssl", "-lcrypto", "-lstdc++",
      "-lz", "-pthread"
    ]

def capnpBridgeCompileArgs : Array String :=
  if System.Platform.isOSX then
    #["-fPIC", "-std=c++23"]
  else
    #["-fPIC", "-std=c++23", "-pthread"]

package capnproto_lean where
  moreLeanArgs := #["-DmaxHeartbeats=2000000"]
  moreLinkArgs := capnpBridgeLinkArgs

require LeanTest from "test/LeanTest"

def ffiWeakArgs (leanIncludeDir : FilePath) : Array String := #[
  "-I", leanIncludeDir.toString,
  "-I/opt/homebrew/include",
  "-I/usr/local/include"
]

target rpc_bridge.o pkg : FilePath := do
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge.o"
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir)) capnpBridgeCompileArgs "c++" getLeanTrace

target rpc_bridge_runtime.o pkg : FilePath := do
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_runtime.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_runtime.o"
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir)) capnpBridgeCompileArgs "c++" getLeanTrace

target rpc_bridge_common.o pkg : FilePath := do
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_common.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_common.o"
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir)) capnpBridgeCompileArgs "c++" getLeanTrace

target rpc_bridge_payload_ref.o pkg : FilePath := do
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_payload_ref.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_payload_ref.o"
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir)) capnpBridgeCompileArgs "c++" getLeanTrace

target rpc_bridge_generic_vat.o pkg : FilePath := do
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_generic_vat.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_generic_vat.o"
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir)) capnpBridgeCompileArgs "c++" getLeanTrace

target kj_async_bridge.o pkg : FilePath := do
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "kj_async_bridge.cpp"
  let oFile := pkg.buildDir / "ffi" / "kj_async_bridge.o"
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir)) capnpBridgeCompileArgs "c++" getLeanTrace

target libleanrpcbridge pkg : FilePath := do
  let bridgeO ← rpc_bridge.o.fetch
  let bridgeRuntimeO ← rpc_bridge_runtime.o.fetch
  let bridgeCommonO ← rpc_bridge_common.o.fetch
  let bridgePayloadRefO ← rpc_bridge_payload_ref.o.fetch
  let bridgeGenericVatO ← rpc_bridge_generic_vat.o.fetch
  let kjAsyncBridgeO ← kj_async_bridge.o.fetch
  let name := nameToStaticLib "leanrpcbridge"
  buildStaticLib (pkg.staticLibDir / name) #[
    bridgeO, bridgeRuntimeO, bridgeCommonO, bridgePayloadRefO, bridgeGenericVatO, kjAsyncBridgeO
  ]

lean_lib CapnpRuntime where
  srcDir := "lean"
  roots := #[`Capnp.Runtime]
  globs := #[.submodules `Capnp]
  moreLinkObjs := #[libleanrpcbridge]

lean_lib CapnpGen where
  srcDir := "test/out"
  roots := #[`Capnp.Gen]
  globs := #[.submodules `Capnp.Gen]

lean_lib CapnpTest where
  srcDir := "test/src"
  roots := #[`Main]

lean_lib CapnpLeanTests where
  srcDir := "test"
  roots := #[`Test]
  globs := #[.submodules `Test]

lean_exe test_full where
  srcDir := "test"
  root := `TestDriver
  supportInterpreter := true

@[test_driver]
lean_exe test where
  srcDir := "test"
  root := `TestDriverRpc
  supportInterpreter := true

lean_exe kj_async_bench where
  srcDir := "test"
  root := `Bench.KjAsyncBench
  supportInterpreter := true
