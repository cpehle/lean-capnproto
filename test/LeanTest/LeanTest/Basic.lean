/-
Copyright (c) 2026 Christian Pehle. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

# Core Types for LeanTest
-/

namespace LeanTest

/-- Configuration for a single test. -/
structure TestConfig where
  /-- Optional custom name for the test. -/
  name : Option String := none
  /-- If true, skip this test by default. -/
  ignore : Bool := false
  /-- If true, expect this test to throw an error. -/
  shouldError : Bool := false
  /-- If shouldError is true, optionally match error message pattern. -/
  errorPattern : Option String := none
  deriving Inhabited, BEq, Repr

/-- A registered test entry. -/
structure TestEntry where
  /-- The declaration name of the test function. -/
  declName : Lean.Name
  /-- Configuration for this test. -/
  config : TestConfig
  deriving Inhabited, Repr

/-- Result of running a single test. -/
inductive TestResult where
  /-- Test passed with given duration in milliseconds. -/
  | passed (duration : Nat)
  /-- Test failed with given duration and error message. -/
  | failed (duration : Nat) (error : String)
  /-- Test was skipped with given reason. -/
  | skipped (reason : String)
  deriving Inhabited, Repr

/-- Summary of a test run. -/
structure TestSummary where
  /-- Total number of tests. -/
  total : Nat
  /-- Number of passed tests. -/
  passed : Nat
  /-- Number of failed tests. -/
  failed : Nat
  /-- Number of skipped tests. -/
  skipped : Nat
  /-- Total duration in milliseconds. -/
  duration : Nat
  /-- Individual test results. -/
  results : Array (Lean.Name × TestResult)
  deriving Inhabited, Repr

/-- Check if all tests passed. -/
def TestSummary.allPassed (s : TestSummary) : Bool :=
  s.failed == 0

end LeanTest
