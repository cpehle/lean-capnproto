/-
Copyright (c) 2026 Christian Pehle. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

# Test Runner for LeanTest
-/

import Lean.CoreM
import Lean.Environment
import LeanTest.Basic
import LeanTest.Assert
import LeanTest.Attr

namespace LeanTest

open Lean

/-- Configuration for running tests. -/
structure RunConfig where
  /-- Include tests marked as ignored. -/
  includeIgnored : Bool := false
  /-- Stop on first failure. -/
  failFast : Bool := false
  /-- Filter tests by name pattern (substring match). -/
  filter : Option String := none
  deriving Inhabited, Repr

/-- Default run configuration. -/
def RunConfig.default : RunConfig := {}

/-- Filter tests based on run configuration. -/
def filterTests (config : RunConfig) (tests : Array TestEntry) : Array TestEntry :=
  tests.filter fun entry =>
    let ignoreOk := !entry.config.ignore || config.includeIgnored
    let filterOk := match config.filter with
      | none => true
      | some pattern => entry.declName.toString.containsSubstr pattern
    ignoreOk && filterOk

/-- Get the display name for a test. -/
def getTestName (entry : TestEntry) : String :=
  entry.config.name.getD entry.declName.toString

/-- Run a single test and return the result. -/
unsafe def runSingleTest (env : Environment) (opts : Options) (entry : TestEntry) : IO TestResult := do
  let startTime ← IO.monoMsNow

  if entry.config.ignore then
    return .skipped "ignored"

  try
    let testFn ← IO.ofExcept <| env.evalConst (IO Unit) opts entry.declName
    testFn

    let endTime ← IO.monoMsNow
    let duration := endTime - startTime

    if entry.config.shouldError then
      return .failed duration "expected error but test passed"
    else
      return .passed duration

  catch e =>
    let endTime ← IO.monoMsNow
    let duration := endTime - startTime
    let errMsg := toString e

    if entry.config.shouldError then
      match entry.config.errorPattern with
      | none => return .passed duration
      | some pattern =>
        if errMsg.containsSubstr pattern then
          return .passed duration
        else
          return .failed duration s!"error did not match pattern '{pattern}': {errMsg}"
    else
      return .failed duration errMsg

/-- Format a single test result for display. -/
def formatResult (name : String) (result : TestResult) : String :=
  let status := match result with
    | .passed duration => s!"ok ({duration}ms)"
    | .failed duration msg => s!"FAILED ({duration}ms)\n  Error: {msg}"
    | .skipped reason => s!"skipped ({reason})"
  let maxNameLen := 50
  let nameLen := min maxNameLen name.length
  let dots := String.ofList (List.replicate (maxNameLen - nameLen) '.')
  s!"{name} {dots} {status}"

/-- Format the test summary for display. -/
def formatSummary (summary : TestSummary) : String :=
  s!"\nTest Summary:
  Passed:   {summary.passed}
  Failed:   {summary.failed}
  Skipped:  {summary.skipped}
  Total:    {summary.total}
  Duration: {summary.duration}ms"

/-- Run all tests and return a summary. -/
unsafe def runTests (env : Environment) (opts : Options) (config : RunConfig := {}) : IO TestSummary := do
  let allTests := getTests env
  let tests := filterTests config allTests

  IO.println s!"Running {tests.size} tests...\n"

  let startTime ← IO.monoMsNow
  let mut passed := 0
  let mut failed := 0
  let mut skipped := 0
  let mut results : Array (Name × TestResult) := #[]

  for entry in tests do
    let name := getTestName entry
    let result ← runSingleTest env opts entry
    IO.println (formatResult name result)
    results := results.push (entry.declName, result)

    match result with
    | .passed _ => passed := passed + 1
    | .failed _ _ =>
      failed := failed + 1
      if config.failFast then
        break
    | .skipped _ => skipped := skipped + 1

  let endTime ← IO.monoMsNow
  let duration := endTime - startTime

  let summary : TestSummary := {
    total := tests.size
    passed := passed
    failed := failed
    skipped := skipped
    duration := duration
    results := results
  }

  IO.println (formatSummary summary)

  return summary

/-- Run tests and exit with appropriate exit code.

Returns 0 if all tests passed, 1 otherwise.
-/
unsafe def runTestsAndExit (env : Environment) (opts : Options) (config : RunConfig := {}) : IO UInt32 := do
  let summary ← runTests env opts config
  return if summary.allPassed then 0 else 1

end LeanTest
