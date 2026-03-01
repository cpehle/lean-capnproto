import LeanTest
import Capnp.Runtime

open LeanTest

-- Message with root text "hi" (null-terminated).
def checkedTextMessage : ByteArray :=
  ByteArray.mk #[
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00,
    0x68, 0x69, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  ]

-- Same but missing NUL terminator.
def badTextMessage : ByteArray :=
  ByteArray.mk #[
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00,
    0x68, 0x69, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
  ]

@[test]
def testReadMessageCheckedOk : IO Unit := do
  let opts : Capnp.ReaderOptions := {}
  match Capnp.readMessageChecked opts checkedTextMessage with
  | Except.ok _ => assertEqual true true
  | Except.error e => assertEqual e ""

@[test]
def testReadTextChecked : IO Unit := do
  let msg := Capnp.readMessage checkedTextMessage
  let root := Capnp.getRoot msg
  match Capnp.readTextChecked root with
  | Except.ok s => assertEqual s "hi"
  | Except.error e => assertEqual e ""

@[test]
def testReadTextCheckedFails : IO Unit := do
  let msg := Capnp.readMessage badTextMessage
  let root := Capnp.getRoot msg
  match Capnp.readTextChecked root with
  | Except.ok _ => assertEqual true false
  | Except.error _ => assertEqual true true

@[test]
def testUnpackCheckedFails : IO Unit := do
  let bad := ByteArray.mk #[0xff]  -- missing 8 bytes after 0xff tag
  match Capnp.unpackChecked bad with
  | Except.ok _ => assertEqual true false
  | Except.error _ => assertEqual true true
