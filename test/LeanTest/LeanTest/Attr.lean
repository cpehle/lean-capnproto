/-
Copyright (c) 2026 Christian Pehle. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

# Test Attributes for LeanTest

This module defines the @[test], @[test_ignore], and @[test_should_error] attributes.

Note: We use `initialize` instead of `builtin_initialize` because this is a
user package, not part of Lean core. The attributes become available when
this module is imported.
-/

import Lean.Attributes
import Lean.Environment
import LeanTest.Basic

namespace LeanTest

open Lean

/-- Validate that a declaration has the correct type for a test (IO Unit). -/
def validateTestType (env : Environment) (declName : Name) : AttrM Unit := do
  let some info := env.find? declName
    | throwError "unknown declaration '{declName}'"
  let type := info.type
  unless type.isAppOf ``IO do
    throwError "@[test] function must have type 'IO Unit', got {type}"

/-- The @[test] attribute marks a function as a test.

```lean
@[test]
def myTest : IO Unit := do
  LeanTest.assertEqual (2 + 2) 4
```
-/
initialize testAttr : TagAttribute ←
  registerTagAttribute `test "Mark a function as a test" (fun declName => do
    let env ← getEnv
    validateTestType env declName
  )

/-- The @[test_ignore] attribute marks a test to be skipped by default.

```lean
@[test_ignore]
def slowTest : IO Unit := do
  IO.sleep 10000
```

Run with `lake test -- --ignored` to include these tests.
-/
initialize testIgnoreAttr : TagAttribute ←
  registerTagAttribute `test_ignore "Mark a test to be skipped by default" (fun declName => do
    let env ← getEnv
    validateTestType env declName
  )

/-- The @[test_should_error] attribute marks a test that is expected to throw an error.

```lean
@[test_should_error]
def testExpectedError : IO Unit := do
  throw <| IO.userError "this error is expected"
```

The test passes if it throws any error, and fails if it completes successfully.
-/
initialize testShouldErrorAttr : TagAttribute ←
  registerTagAttribute `test_should_error "Mark a test as expected to throw an error" (fun declName => do
    let env ← getEnv
    validateTestType env declName
  )

/-- Get all tests from the environment by scanning for tagged declarations. -/
def getTests (env : Environment) : Array TestEntry := Id.run do
  let mut tests : Array TestEntry := #[]

  -- Collect tests from @[test] attribute
  for name in env.constants.map₁.toList do
    if testAttr.hasTag env name.1 then
      tests := tests.push { declName := name.1, config := {} }

  -- Collect tests from @[test_ignore] attribute
  for name in env.constants.map₁.toList do
    if testIgnoreAttr.hasTag env name.1 then
      tests := tests.push { declName := name.1, config := { ignore := true } }

  -- Collect tests from @[test_should_error] attribute
  for name in env.constants.map₁.toList do
    if testShouldErrorAttr.hasTag env name.1 then
      tests := tests.push { declName := name.1, config := { shouldError := true } }

  return tests

end LeanTest
