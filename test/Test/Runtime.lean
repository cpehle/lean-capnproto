import LeanTest
import Capnp.Runtime

open LeanTest

-- One-segment message with a root struct containing a single UInt64 value.
def simpleStructMessage : ByteArray :=
  ByteArray.mk #[
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
  ]

-- One-segment message with a root list of UInt16 values [10, 20, 30].
def listUInt16Message : ByteArray :=
  ByteArray.mk #[
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00,
    0x0a, 0x00, 0x14, 0x00, 0x1e, 0x00, 0x00, 0x00
  ]

-- One-segment message with a root text "hi" (null-terminated).
def textMessage : ByteArray :=
  ByteArray.mk #[
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00,
    0x68, 0x69, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  ]

@[test]
def testReadStructUInt64 : IO Unit := do
  let msg := Capnp.readMessage simpleStructMessage
  let root := Capnp.getRoot msg
  let r := Capnp.readStruct root
  assertEqual (Capnp.getUInt64 r 0) (0x1122334455667788 : UInt64)

@[test]
def testReadListUInt16 : IO Unit := do
  let msg := Capnp.readMessage listUInt16Message
  let root := Capnp.getRoot msg
  let values := Capnp.readListUInt16 root
  let expected : List UInt16 := [10, 20, 30]
  assertEqual values.toList expected

@[test]
def testReadText : IO Unit := do
  let msg := Capnp.readMessage textMessage
  let root := Capnp.getRoot msg
  let value := Capnp.readText root
  assertEqual value "hi"

@[test]
def testReadTextView : IO Unit := do
  let msg := Capnp.readMessage textMessage
  let root := Capnp.getRoot msg
  let view := Capnp.readTextView root
  assertEqual (Capnp.textViewToString view) "hi"
  assertEqual (Capnp.dataViewToByteArray view) (ByteArray.mk #[0x68, 0x69])
