import Lake
open System
open Lake DSL

private def findExecutableOnPath (name : String) : IO (Option System.FilePath) := do
  let searchPath :=
    match (← IO.getEnv "PATH") with
    | some path => System.SearchPath.parse path
    | none => []
  for dir in searchPath do
    let candidate := dir / name
    if ← candidate.pathExists then
      return some candidate
  return none

def useLibcxx : Bool :=
  match get_config? useLibcxx with
  | some value => value != "false" && value != "0"
  | none => !System.Platform.isOSX && !System.Platform.isWindows

def capnpBridgeCompiler [Monad m] [MonadLakeEnv m] [MonadLiftT BaseIO m] : m String := do
  match ← IO.getEnv "CXX" with
  | some cxx => pure cxx
  | none =>
      if System.Platform.isOSX || System.Platform.isWindows then
        pure "c++"
      else
        pure (← getLeanc).toString

private def preferredLinuxNativeCxxCandidates : List String :=
  ["clang++-19", "g++-14", "clang++", "g++", "c++"]

def capnpNativeCxxCompiler : IO String := do
  match ← IO.getEnv "CXX" with
  | some cxx => pure cxx
  | none =>
      if System.Platform.isOSX || System.Platform.isWindows then
        pure "c++"
      else
        for name in preferredLinuxNativeCxxCandidates do
          if let some path ← findExecutableOnPath name then
            return path.toString
        pure "c++"

private def linuxGnuCxxIncludeArgs : Array String :=
  let archInclude :=
    if System.Platform.target.contains "aarch64" then
      "/usr/include/aarch64-linux-gnu"
    else if System.Platform.target.contains "x86_64" then
      "/usr/include/x86_64-linux-gnu"
    else
      "/usr/include"
  #[
    "-I/usr/lib/llvm-19/lib/clang/19/include",
    "-I/usr/include/c++/14",
    s!"-I{archInclude}/c++/14",
    "-I/usr/include/c++/14/backward",
    s!"-I{archInclude}",
    "-I/usr/include"
  ]

private def linuxLibcxxIncludeArgs : Array String :=
  let archInclude :=
    if System.Platform.target.contains "aarch64" then
      "/usr/include/aarch64-linux-gnu"
    else if System.Platform.target.contains "x86_64" then
      "/usr/include/x86_64-linux-gnu"
    else
      "/usr/include"
  #[
    "-I/usr/lib/llvm-19/include/c++/v1",
    "-I/usr/lib/llvm-19/lib/clang/19/include",
    s!"-I{archInclude}",
    "-I/usr/include"
  ]

private def linuxGnuCxxCMakeFlags (pkgDir : FilePath) : String :=
  String.intercalate " " <|
    (#["-stdlib=libstdc++"] ++ linuxGnuCxxIncludeArgs ++ #[
      "-include",
      (pkgDir / "ffi" / "glibc_compat_features.h").toString
    ]).toList

def capnpBridgeLinkArgs (pkgDir : FilePath) : Array String :=
  if System.Platform.isOSX then
    #[
      "-L", (pkgDir / "extern" / "capnproto" / "build" / "c++" / "src" / "capnp").toString,
      "-L", (pkgDir / "extern" / "capnproto" / "build" / "c++" / "src" / "kj").toString,
      "-L/opt/homebrew/lib",
      "-L/usr/local/lib",
      "-L/opt/homebrew/opt/openssl@3/lib",
      "-L/usr/local/opt/openssl@3/lib",
      "-L/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib",
      "-lcapnp-rpc", "-lcapnp", "-lkj-http", "-lkj-gzip", "-lkj-tls", "-lkj-async", "-lkj",
      "-lssl", "-lcrypto", "-lz", "-lc++"
    ]
  else
    #[
      "-L", (pkgDir / "extern" / "capnproto" / "build" / "c++" / "src" / "capnp").toString,
      "-L", (pkgDir / "extern" / "capnproto" / "build" / "c++" / "src" / "kj").toString,
      "-lcapnp-rpc", "-lcapnp", "-lkj-http", "-lkj-gzip", "-lkj-tls", "-lkj-async", "-lkj",
      "-lssl", "-lcrypto"
    ] ++
      if useLibcxx then
        #["-lc++", "-lc++abi", "-lz", "-pthread"]
      else
        #["-lstdc++", "-lz", "-pthread"]

def capnpBridgeCompileArgs : Array String :=
  if System.Platform.isOSX then
    #["-fPIC", "-std=c++23"]
  else if useLibcxx then
    #["-fPIC", "-std=c++23", "-pthread", "-stdlib=libc++"] ++ linuxLibcxxIncludeArgs ++
      #["-include", (__dir__ / "ffi" / "glibc_compat_features.h").toString]
  else
    #["-fPIC", "-std=c++23", "-pthread", "-stdlib=libstdc++"] ++ linuxGnuCxxIncludeArgs ++
      #["-include", (__dir__ / "ffi" / "glibc_compat_features.h").toString]


private def capnpSourceDir (pkgDir : FilePath) : FilePath :=
  pkgDir / "extern" / "capnproto" / "c++"

private def capnpBuildDir (pkgDir : FilePath) : FilePath :=
  pkgDir / "extern" / "capnproto" / "build" / "c++"

private def capnpNativeArtifactsExist (pkgDir : FilePath) : IO Bool := do
  let buildDir := capnpBuildDir pkgDir
  let capnpRpc := buildDir / "src" / "capnp" / "libcapnp-rpc.a"
  let kjTls := buildDir / "src" / "kj" / "libkj-tls.a"
  let capnpTool := buildDir / "src" / "capnp" / "capnp"
  let plugin := buildDir / "src" / "capnp" / "capnpc-lean4"
  let testHeader := buildDir / "src" / "capnp" / "test_capnp" / "capnp" / "test.capnp.h"
  pure ((← capnpRpc.pathExists) && (← kjTls.pathExists) && (← capnpTool.pathExists) &&
    (← plugin.pathExists) && (← testHeader.pathExists))

private def runChecked (cwd : FilePath) (cmd : String) (args : Array String) : IO Unit := do
  let out ← IO.Process.output { cmd, args, cwd := cwd }
  if out.exitCode ≠ 0 then
    error s!"command failed in {cwd}: {cmd} {String.intercalate " " args.toList}
stdout:
{out.stdout}
stderr:
{out.stderr}"

private def capnpNativeBuildTargets : Array String := #[
  "kj",
  "kj-async",
  "kj-gzip",
  "kj-http",
  "kj-tls",
  "capnp",
  "capnp-rpc",
  "capnp_tool",
  "capnpc_lean4",
  "test_capnp"
]

package capnproto_lean where
  moreLeanArgs := #["-DmaxHeartbeats=2000000"]
  moreLinkArgs := capnpBridgeLinkArgs __dir__

target capnpNativeDeps pkg : Unit := do
  let srcDir := capnpSourceDir pkg.dir
  let buildDir := capnpBuildDir pkg.dir
  let mut configureArgs := #[
    "-S", srcDir.toString,
    "-B", buildDir.toString,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_TESTING=ON"
  ]
  if let some cc ← IO.getEnv "CC" then
    configureArgs := configureArgs ++ #[s!"-DCMAKE_C_COMPILER={cc}"]
  let cxx ← capnpNativeCxxCompiler
  configureArgs := configureArgs ++ #[s!"-DCMAKE_CXX_COMPILER={cxx}"]
  if useLibcxx && !System.Platform.isOSX && !System.Platform.isWindows then
    configureArgs := configureArgs ++ #[
      s!"-DCMAKE_CXX_FLAGS=-stdlib=libc++ -include {pkg.dir / "ffi" / "glibc_compat_features.h"}",
      "-DCMAKE_EXE_LINKER_FLAGS=-stdlib=libc++",
      "-DCMAKE_SHARED_LINKER_FLAGS=-stdlib=libc++"
    ]
  else if !System.Platform.isOSX && !System.Platform.isWindows then
    configureArgs := configureArgs ++ #[
      s!"-DCMAKE_CXX_FLAGS={linuxGnuCxxCMakeFlags pkg.dir}"
    ]

  let cmakeCache := buildDir / "CMakeCache.txt"
  if ← cmakeCache.pathExists then
    let cacheContents ← IO.FS.readFile cmakeCache
    let mut resetBuildDir := !cacheContents.contains s!"CMAKE_HOME_DIRECTORY:INTERNAL={srcDir}"
    if let some cc := (← IO.getEnv "CC") then
      resetBuildDir := resetBuildDir || !cacheContents.contains s!"CMAKE_C_COMPILER:FILEPATH={cc}"
    resetBuildDir := resetBuildDir || !cacheContents.contains s!"CMAKE_CXX_COMPILER:FILEPATH={cxx}"
    resetBuildDir := resetBuildDir || !cacheContents.contains "BUILD_TESTING:BOOL=ON"
    if useLibcxx then
      resetBuildDir := resetBuildDir || !cacheContents.contains s!"CMAKE_CXX_FLAGS:STRING=-stdlib=libc++ -include {pkg.dir / "ffi" / "glibc_compat_features.h"}"
    else
      resetBuildDir := resetBuildDir || !cacheContents.contains s!"CMAKE_CXX_FLAGS:STRING={linuxGnuCxxCMakeFlags pkg.dir}"
    if resetBuildDir && (← buildDir.pathExists) then
      IO.FS.removeDirAll buildDir
  if !(← cmakeCache.pathExists) then
    runChecked pkg.dir "cmake" configureArgs
  if !(← capnpNativeArtifactsExist pkg.dir) then
    runChecked pkg.dir "cmake" (#["--build", buildDir.toString, "--parallel", "--target"] ++ capnpNativeBuildTargets)
  return .nil

require LeanTest from "test/LeanTest"

def ffiWeakArgs (leanIncludeDir : FilePath) (pkgDir : FilePath) : Array String := #[
  "-I", leanIncludeDir.toString,
  "-I", (pkgDir / "extern" / "capnproto" / "c++" / "src").toString,
  "-I", (pkgDir / "extern" / "capnproto" / "build" / "c++" / "src" / "capnp" / "test_capnp").toString,
  "-I/opt/homebrew/include",
  "-I/usr/local/include"
]

target rpc_bridge.o pkg : FilePath := do
  let _ ← capnpNativeDeps.fetch
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge.o"
  let compiler ← capnpBridgeCompiler
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir) pkg.dir) capnpBridgeCompileArgs compiler getLeanTrace

target rpc_bridge_runtime.o pkg : FilePath := do
  let _ ← capnpNativeDeps.fetch
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_runtime.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_runtime.o"
  let compiler ← capnpBridgeCompiler
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir) pkg.dir) capnpBridgeCompileArgs compiler getLeanTrace

target rpc_bridge_common.o pkg : FilePath := do
  let _ ← capnpNativeDeps.fetch
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_common.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_common.o"
  let compiler ← capnpBridgeCompiler
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir) pkg.dir) capnpBridgeCompileArgs compiler getLeanTrace

target rpc_bridge_payload_ref.o pkg : FilePath := do
  let _ ← capnpNativeDeps.fetch
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_payload_ref.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_payload_ref.o"
  let compiler ← capnpBridgeCompiler
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir) pkg.dir) capnpBridgeCompileArgs compiler getLeanTrace

target rpc_bridge_generic_vat.o pkg : FilePath := do
  let _ ← capnpNativeDeps.fetch
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "rpc_bridge_generic_vat.cpp"
  let oFile := pkg.buildDir / "ffi" / "rpc_bridge_generic_vat.o"
  let compiler ← capnpBridgeCompiler
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir) pkg.dir) capnpBridgeCompileArgs compiler getLeanTrace

target kj_async_bridge.o pkg : FilePath := do
  let _ ← capnpNativeDeps.fetch
  let srcJob ← inputTextFile <| pkg.dir / "ffi" / "kj_async_bridge.cpp"
  let oFile := pkg.buildDir / "ffi" / "kj_async_bridge.o"
  let compiler ← capnpBridgeCompiler
  buildO oFile srcJob (ffiWeakArgs (← getLeanIncludeDir) pkg.dir) capnpBridgeCompileArgs compiler getLeanTrace

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
