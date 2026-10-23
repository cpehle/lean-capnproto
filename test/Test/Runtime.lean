import LeanTest
import Capnp.Runtime

open LeanTest

-- The trailing all-ones word belongs to another object, not this struct.
private def boundedStruct : Capnp.StructReader :=
  let msg := Capnp.readMessage <| ByteArray.mk #[
    0, 0, 0, 0, 3, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0,
    255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255]
  Capnp.readStruct (Capnp.getRoot msg)

@[test]
def testStructScalarBounds : IO Unit := do
  let r := boundedStruct
  assertEqual (Capnp.getUInt8 r 7) (255 : UInt8)
  assertEqual (Capnp.getUInt16 r 6) (65535 : UInt16)
  assertEqual (Capnp.getUInt32 r 4) (0xffffffff : UInt32)
  assertEqual (Capnp.getUInt64 r 0) (0xffffffffffffffff : UInt64)
  assertEqual (Capnp.getBool r 63) true
  assertEqual (Capnp.getUInt8 r 8) (0 : UInt8)
  assertEqual (Capnp.getUInt16 r 8) (0 : UInt16)
  assertEqual (Capnp.getUInt32 r 8) (0 : UInt32)
  assertEqual (Capnp.getUInt64 r 8) (0 : UInt64)
  assertEqual (Capnp.getBool r 64) false
  -- Like the C++ reader, return zero for the entire field if it straddles
  -- the data boundary, rather than salvaging a prefix from another field.
  assertEqual (Capnp.getUInt16 r 7) (0 : UInt16)
  assertEqual (Capnp.getUInt32 r 5) (0 : UInt32)
  assertEqual (Capnp.getUInt64 r 1) (0 : UInt64)

@[test]
def testStructMissingFieldDefaults : IO Unit := do
  let r := boundedStruct
  assertEqual (Capnp.getBoolMasked r 64 true) true
  assertEqual (Capnp.getUInt8Masked r 8 37) (37 : UInt8)
  assertEqual (Capnp.getUInt16Masked r 8 1234) (1234 : UInt16)
  assertEqual (Capnp.getUInt32Masked r 8 56789) (56789 : UInt32)
  assertEqual (Capnp.getUInt64Masked r 8 987654321) (987654321 : UInt64)
  assertEqual (Capnp.getInt8 r 8) (0 : Int8)
  assertEqual (Capnp.getInt16 r 8) (0 : Int16)
  assertEqual (Capnp.getInt32 r 8) (0 : Int32)
  assertEqual (Capnp.getInt64 r 8) (0 : Int64)
  assertEqual (Capnp.getFloat32Masked r 8 0x3f800000) (1 : Float)
  assertEqual (Capnp.getFloat64Masked r 8 0x3ff0000000000000) (1 : Float)
  let absent := Capnp.getPointer r 0
  assertEqual (Capnp.isNullPointer absent) true
  assertEqual (Capnp.readText absent) ""
  let text ← IO.ofExcept (Capnp.readTextChecked absent)
  assertEqual text ""
  let nested ← IO.ofExcept (Capnp.readStructChecked absent)
  assertEqual nested.dataWords 0
  assertEqual nested.ptrCount 0
  assertEqual (Capnp.getUInt64 nested 0) (0 : UInt64)
  let deeper ← IO.ofExcept (Capnp.readTextChecked (Capnp.getPointer nested 0))
  assertEqual deeper ""

@[test]
def testNullStructDoesNotReadNeighboringData : IO Unit := do
  -- A null root followed by another object's nonzero data. emptyStruct uses
  -- offset zero; neither the root nor neighboring data belongs to its fields.
  let msg := Capnp.readMessage <| ByteArray.mk #[
    0, 0, 0, 0, 2, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    255, 255, 255, 255, 255, 255, 255, 255]
  let r ← IO.ofExcept (Capnp.readStructChecked (Capnp.getRoot msg))
  assertEqual (Capnp.getUInt8 r 8) (0 : UInt8)
  assertEqual (Capnp.getUInt16 r 8) (0 : UInt16)
  assertEqual (Capnp.getUInt32 r 8) (0 : UInt32)
  assertEqual (Capnp.getUInt64 r 8) (0 : UInt64)
  assertEqual (Capnp.getBool r 64) false
  assertEqual (Capnp.getBoolMasked r 64 true) true
  assertEqual (Capnp.isNullPointer (Capnp.getPointer r 1)) true

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
def testStructPointerBoundsAndDefault : IO Unit := do
  let msg := Capnp.readMessage textMessage
  let r : Capnp.StructReader := {
    msg, seg := 0, dataOff := 0, dataWords := 0, ptrOff := 0, ptrCount := 1 }
  let present ← IO.ofExcept (Capnp.readTextChecked (Capnp.getPointer r 0))
  assertEqual present "hi"
  let absent := Capnp.getPointer r 1
  assertEqual (Capnp.isNullPointer absent) true
  let empty ← IO.ofExcept (Capnp.readTextChecked absent)
  assertEqual empty ""
  let defaulted ← IO.ofExcept <| Capnp.readTextChecked <|
    Capnp.withDefaultPointer absent (Capnp.getRoot msg)
  assertEqual defaulted "hi"

@[test]
def testReadTextView : IO Unit := do
  let msg := Capnp.readMessage textMessage
  let root := Capnp.getRoot msg
  let view := Capnp.readTextView root
  assertEqual (Capnp.textViewToString view) "hi"
  assertEqual (Capnp.dataViewToByteArray view) (ByteArray.mk #[0x68, 0x69])
