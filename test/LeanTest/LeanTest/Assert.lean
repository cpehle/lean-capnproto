/-
Copyright (c) 2026 Christian Pehle. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

# Assertion Helpers for LeanTest
-/

/-- Check if `needle` is a substring of `haystack`. -/
def String.containsSubstr (haystack needle : String) : Bool :=
  if needle.isEmpty then true
  else Id.run do
    for i in [:haystack.length] do
      if (haystack.drop i).startsWith needle then
        return true
    return false

namespace LeanTest

/-- Assert that a boolean condition is true. -/
def assertTrue (cond : Bool) (msg : String := "expected true, got false") : IO Unit := do
  unless cond do
    throw <| IO.userError s!"Assertion failed: {msg}"

/-- Assert that a boolean condition is false. -/
def assertFalse (cond : Bool) (msg : String := "expected false, got true") : IO Unit := do
  if cond then
    throw <| IO.userError s!"Assertion failed: {msg}"

/-- Assert that two values are equal. -/
def assertEqual [BEq α] [ToString α] (actual expected : α)
    (msg : Option String := none) : IO Unit := do
  unless actual == expected do
    let defaultMsg := s!"expected {expected}, got {actual}"
    throw <| IO.userError s!"Assertion failed: {msg.getD defaultMsg}"

/-- Assert that two values are not equal. -/
def assertNotEqual [BEq α] [ToString α] (actual notExpected : α)
    (msg : Option String := none) : IO Unit := do
  if actual == notExpected then
    let defaultMsg := s!"expected value different from {notExpected}, but got {actual}"
    throw <| IO.userError s!"Assertion failed: {msg.getD defaultMsg}"

/-- Assert that an optional value is `some`. -/
def assertSome [ToString α] (opt : Option α) (msg : String := "expected Some, got None") : IO Unit := do
  match opt with
  | some _ => pure ()
  | none => throw <| IO.userError s!"Assertion failed: {msg}"

/-- Assert that an optional value is `none`. -/
def assertNone [ToString α] (opt : Option α) (msg : Option String := none) : IO Unit := do
  match opt with
  | none => pure ()
  | some v =>
    let defaultMsg := s!"expected None, got Some {v}"
    throw <| IO.userError s!"Assertion failed: {msg.getD defaultMsg}"

/-- Assert that an action throws an error. -/
def assertThrows (action : IO α) (msgPattern : Option String := none) : IO Unit := do
  try
    let _ ← action
    throw <| IO.userError "Assertion failed: expected exception but none was thrown"
  catch e =>
    if let some pattern := msgPattern then
      let errMsg := toString e
      unless errMsg.containsSubstr pattern do
        throw <| IO.userError s!"Assertion failed: exception '{errMsg}' did not contain pattern '{pattern}'"

/-- Assert that an action does not throw an error. -/
def assertNoThrow (action : IO α) : IO α := do
  try
    action
  catch e =>
    throw <| IO.userError s!"Assertion failed: unexpected exception: {e}"

/-- Fail the test unconditionally with a message. -/
def fail (msg : String) : IO α := do
  throw <| IO.userError s!"Test failed: {msg}"

/-- Assert that a value satisfies a predicate. -/
def assertSatisfies [ToString α] (value : α) (pred : α → Bool) (msg : String) : IO Unit := do
  unless pred value do
    throw <| IO.userError s!"Assertion failed: {msg} (value was {value})"

end LeanTest
