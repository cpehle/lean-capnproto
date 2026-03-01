/-
Copyright (c) 2026 Christian Pehle. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

# LeanTest - A Rust-inspired Test Framework for Lean4

This package provides a test framework similar to Rust's `#[test]` attribute system.

## Features

- `@[test]` attribute to mark test functions
- `@[test_ignore]` for tests to skip by default
- `@[test_should_error]` for tests expected to fail
- Assertion helpers: `assertTrue`, `assertEqual`, `assertThrows`, etc.
- Test runner with filtering and fail-fast options
- Integration with `lake test`

## Usage

```lean
import LeanTest

@[test]
def testAddition : IO Unit := do
  LeanTest.assertEqual (2 + 2) 4

@[test_ignore]
def testSlowOperation : IO Unit := do
  -- This test is skipped by default
  IO.sleep 10000

@[test_should_error]
def testExpectedError : IO Unit := do
  throw <| IO.userError "expected"
```

## Running Tests

```bash
lake test                     # Run all tests
lake test -- --filter foo     # Run tests matching "foo"
lake test -- --ignored        # Include ignored tests
lake test -- --fail-fast      # Stop on first failure
```
-/

import LeanTest.Basic
import LeanTest.Assert
import LeanTest.Attr
import LeanTest.Runner
