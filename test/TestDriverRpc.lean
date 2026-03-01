/-
Copyright (c) 2026 The Cap'n Proto Authors.
Released under the MIT License.

Fast test driver for Lean4 backend work. This excludes conformance tests, which
pull in large generated modules and are run via the full test driver.
-/

import LeanTest
import Lean.Util.Path
import Test.Runtime
import Test.Generated
import Test.Builder
import Test.Capability
import Test.Packed
import Test.Checked
import Test.KjAsync
import Test.Rpc
import Test.RpcClient
import Test.RpcShutdown
import Test.RpcReliability
import Test.RpcOrderingControl
import Test.RpcLayout
import Test.AsyncBridge

/-- Driver options layered over LeanTest.RunConfig. -/
structure DriverConfig where
  runConfig : LeanTest.RunConfig := {}
  parityCritical : Bool := false
  deriving Inhabited

/--
Deterministic parity-critical suite used by CI lock-in for RPC/KjAsync behavior
classes.
-/
private def parityCriticalTests : Array Lean.Name := #[
  -- Core release/cancel semantics.
  `testRpcReleaseMessageRoundtrip,
  `testRpcReturnCanceled,
  `testRuntimePendingCallRelease,
  `testRuntimeParityAdvancedDeferredReleaseWithoutAllowCancellation,

  -- Ordering-sensitive resolve/disembargo/tail-call behavior.
  `testRuntimeParityResolvePipelineOrdering,
  `testRuntimeParityDisembargoNullPipelineDoesNotDisconnect,
  `testRuntimeParityEmbargoErrorKeepsConnectionAlive,
  `testRuntimeParityTailCallPipelineOrdering,
  `testRuntimeParityAdvancedDeferredSetPipelineOrdering,
  `testRuntimeTwoHopPipelinedResolveOrdering,
  `testRuntimeTwoHopPipelinedResolveOrderingWithNestedPromise,
  `testRuntimeOrderingResolveHoldControlsDisembargo,
  `testRuntimeOrderingResolveHooksTrackHeldCount,
  `testRuntimeProtocolResolveDisembargoMessageCounters,
  `testRuntimeProtocolNullPipelineDoesNotEmitDisembargo,
  `testRuntimeProtocolBlockingOrdering,

  -- Lifecycle and disconnect visibility.
  `testRuntimeAsyncClientLifecyclePrimitives,
  `testRuntimeClientOnDisconnectAfterServerRelease,
  `testRuntimeDisconnectVisibilityViaCallResult,
  `testInteropLeanClientObservesCppDisconnectAfterOneShot,

  -- Failure propagation and cancellation sequencing.
  `testRuntimeParityCancelDisconnectSequencing,
  `testRuntimeParityPromisedCapabilityDelayedRejectPropagatesToPendingCalls,
  `testRuntimeParityPromisedCapabilityCancelBeforeRejectSequencing,
  `testRuntimeTransportAbortPendingCall,
  `testRuntimeProtocolDisconnectDetail,
  `testInteropLeanClientCancelsPendingCallToCppDelayedServer,
  `testInteropLeanPendingCallOutcomeCapturesCppException,
  `testInteropLeanClientReceivesCppExceptionDetail,

  -- Flow control and trace observability.
  `testRuntimeClientQueueMetrics,
  `testRuntimeClientQueueMetricsPreAcceptBacklogDrains,
  `testRuntimeProtocolDiagnostics,
  `testRuntimeClientSetFlowLimit,
  `testRuntimeTraceEncoderToggle,
  `testRuntimeSetTraceEncoderOnExistingConnection,
  `testRuntimeTraceEncoderCallResultVisibility,

  -- Streaming and FD transfer behavior.
  `testRuntimeStreamingCall,
  `testRuntimeRegisterStreamingHandlerTarget,
  `testRuntimeStreamingCancellation,
  `testRuntimeStreamingNoPrematureCancellationWhenTargetDropped,
  `testRuntimeStreamingForwardedAcrossMultiVatNoPrematureCancellation,
  `testRuntimeStreamingChainedBackpressure,
  `testRuntimeFdPassingOverNetwork,
  `testRuntimeFdPerMessageLimitDropsExcessFds,
  `testRuntimeFdTargetLocalGetFd,

  -- RPC/KjAsync bridge-critical checks.
  `testKjAsyncPromiseOpsOnRpcRuntimeHandle,
  `testKjAsyncTaskSetOpsOnRpcRuntimeHandle,
  `testKjAsyncPipeFdOpsOnRpcRuntimeHandle,
  `testRpcRuntimeMRunKjAsyncBridge,
  `testRpcRuntimeRunKjAsyncBridgeHelpers,
  `testRpcRuntimeMWithKjAsyncRuntimeHelpers,
  `testRuntimeAdvancedHandlerStartsKjAsyncPromisesOnSameRuntime,
  `testRuntimeAdvancedHandlerRejectsKjAsyncAwaitOnWorkerThread,
  `testRuntimeAdvancedHandlerRejectsRpcShutdownOnWorkerThread,
  `testRuntimeAdvancedHandlerRejectsKjAsyncShutdownOnWorkerThread,
  `testKjAsyncShutdownReleasesRpcRuntimeHandle,
  `testRuntimeKjAsyncSleepAsTaskAndPromiseHelpers,
  `testRuntimeAsyncFFIPump,

  -- FFI Layout Validation
  `testRpcDiagnosticsLayout,
  `testRemoteExceptionLayout
]

/-- Resolve deterministic parity-critical test declarations from discovered tests. -/
private def selectParityCriticalTests (allTests : Array LeanTest.TestEntry) :
    IO (Array LeanTest.TestEntry) := do
  let mut selected : Array LeanTest.TestEntry := #[]
  let mut missing : Array String := #[]

  for testName in parityCriticalTests do
    match allTests.find? (fun entry => entry.declName == testName) with
    | some entry =>
      selected := selected.push entry
    | none =>
      missing := missing.push testName.toString

  if !missing.isEmpty then
    throw (IO.userError
      s!"parity-critical suite is stale; missing declarations: {String.intercalate ", " missing.toList}")

  pure selected

/-- Run an explicit set of tests, preserving deterministic declaration order. -/
private unsafe def runSelectedTestsAndExit (env : Lean.Environment) (opts : Lean.Options)
    (selected : Array LeanTest.TestEntry) (config : LeanTest.RunConfig) : IO UInt32 := do
  let tests := LeanTest.filterTests config selected

  IO.println s!"Running {tests.size} tests...\n"

  let startTime ← IO.monoMsNow
  let mut passed := 0
  let mut failed := 0
  let mut skipped := 0
  let mut results : Array (Lean.Name × LeanTest.TestResult) := #[]

  for entry in tests do
    let name := LeanTest.getTestName entry
    IO.println s!"starting {name}"
    let result ← LeanTest.runSingleTest env opts entry
    IO.println (LeanTest.formatResult name result)
    results := results.push (entry.declName, result)

    match result with
    | .passed _ =>
      passed := passed + 1
    | .failed _ _ =>
      failed := failed + 1
      if config.failFast then
        break
    | .skipped _ =>
      skipped := skipped + 1

  let endTime ← IO.monoMsNow
  let summary : LeanTest.TestSummary := {
    total := tests.size
    passed := passed
    failed := failed
    skipped := skipped
    duration := endTime - startTime
    results := results
  }

  IO.println (LeanTest.formatSummary summary)
  pure <| if summary.allPassed then 0 else 1

/-- Parse command line arguments into a RunConfig. -/
def parseArgs (args : List String) : IO DriverConfig := do
  let mut runConfig : LeanTest.RunConfig := {}
  let mut parityCritical := false
  let mut remaining := args
  while _h : !remaining.isEmpty do
    match remaining with
    | "--filter" :: pattern :: rest =>
      runConfig := { runConfig with filter := some pattern }
      remaining := rest
    | "--ignored" :: rest =>
      runConfig := { runConfig with includeIgnored := true }
      remaining := rest
    | "--fail-fast" :: rest =>
      runConfig := { runConfig with failFast := true }
      remaining := rest
    | "--parity-critical" :: rest =>
      parityCritical := true
      remaining := rest
    | "--help" :: _ =>
      IO.println "Usage: lake test [OPTIONS]"
      IO.println ""
      IO.println "Options:"
      IO.println "  --filter PATTERN  Only run tests matching PATTERN"
      IO.println "  --ignored         Include tests marked as ignored"
      IO.println "  --fail-fast       Stop on first failure"
      IO.println "  --parity-critical Run deterministic RPC/KjAsync parity-critical suite"
      IO.println "  --help            Show this help"
      IO.Process.exit 0
    | _ :: rest =>
      remaining := rest
    | [] =>
      remaining := []
  return { runConfig := runConfig, parityCritical := parityCritical }

/-- Main entry point for the fast test driver. -/
unsafe def main (args : List String) : IO UInt32 := do
  let driverConfig ← parseArgs args

  Lean.initSearchPath (← Lean.findSysroot)
  Lean.enableInitializersExecution

  let env ← Lean.importModules
    #[{ module := `LeanTest }, { module := `Test.Runtime }, { module := `Test.Generated },
      { module := `Test.Builder }, { module := `Test.Capability }, { module := `Test.Packed },
      { module := `Test.Checked }, { module := `Test.KjAsync }, { module := `Test.Rpc },
      { module := `Test.RpcClient }, { module := `Test.RpcShutdown },
      { module := `Test.RpcReliability },
      { module := `Test.RpcOrderingControl }, { module := `Test.RpcLayout },
      { module := `Test.AsyncBridge }]
    {}

  let allTests := LeanTest.getTests env
  if driverConfig.parityCritical then
    let selected ← selectParityCriticalTests allTests
    runSelectedTestsAndExit env {} selected driverConfig.runConfig
  else
    runSelectedTestsAndExit env {} allTests driverConfig.runConfig
