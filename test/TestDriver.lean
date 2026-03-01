/-
Copyright (c) 2026 The Cap'n Proto Authors.
Released under the MIT License.

Test driver for LeanTest-based checks in test/lean4.
-/

import LeanTest
import Lean.Util.Path
import Test.Runtime
import Test.Generated
import Test.Builder
import Test.Capability
import Test.Packed
import Test.Checked
import Test.Rpc
import Test.RpcClient
import Test.RpcShutdown
import Test.RpcOrderingControl
import Test.Conformance

/-- Parse command line arguments into a RunConfig. -/
def parseArgs (args : List String) : IO LeanTest.RunConfig := do
  let mut config : LeanTest.RunConfig := {}
  let mut remaining := args
  while _h : !remaining.isEmpty do
    match remaining with
    | "--filter" :: pattern :: rest =>
      config := { config with filter := some pattern }
      remaining := rest
    | "--ignored" :: rest =>
      config := { config with includeIgnored := true }
      remaining := rest
    | "--fail-fast" :: rest =>
      config := { config with failFast := true }
      remaining := rest
    | "--help" :: _ =>
      IO.println "Usage: lake test [OPTIONS]"
      IO.println ""
      IO.println "Options:"
      IO.println "  --filter PATTERN  Only run tests matching PATTERN"
      IO.println "  --ignored         Include tests marked as ignored"
      IO.println "  --fail-fast       Stop on first failure"
      IO.println "  --help            Show this help"
      IO.Process.exit 0
    | _ :: rest =>
      remaining := rest
    | [] =>
      remaining := []
  return config

/-- Main entry point for the test driver. -/
unsafe def main (args : List String) : IO UInt32 := do
  let config ← parseArgs args

  -- Initialize search path for imports
  Lean.initSearchPath (← Lean.findSysroot)

  -- Enable execution of module initializers
  Lean.enableInitializersExecution

  -- Import LeanTest first so the test registry extension is registered.
  let env ← Lean.importModules
    #[{ module := `LeanTest }, { module := `Test.Runtime }, { module := `Test.Generated },
      { module := `Test.Builder }, { module := `Test.Capability }, { module := `Test.Packed },
      { module := `Test.Checked }, { module := `Test.Rpc }, { module := `Test.RpcClient },
      { module := `Test.RpcShutdown },
      { module := `Test.RpcOrderingControl },
      { module := `Test.Conformance }]
    {}

  LeanTest.runTestsAndExit env {} config
