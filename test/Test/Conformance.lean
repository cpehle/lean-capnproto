import LeanTest
import Capnp.Runtime
import Capnp.Gen.c__.src.capnp.test

set_option maxHeartbeats 2000000

open LeanTest
open Capnp.Gen.c__.src.capnp.test

private def testdataPath (name : String) : String :=
  "../../c++/src/capnp/testdata/" ++ name

private def bytes (s : String) : ByteArray :=
  s.toUTF8

private def assertListEq {α} [BEq α] (got : Capnp.ListReader α) (expected : List α) : IO Unit :=
  assertTrue (got.toList == expected) "list mismatch"

private def assertEnumEq (got expected : TestEnum) : IO Unit :=
  assertEqual (TestEnum.toUInt16 got) (TestEnum.toUInt16 expected)

private def assertEnumListEq (got : Capnp.ListReader TestEnum) (expected : List TestEnum) : IO Unit :=
  let gotIds := got.map TestEnum.toUInt16
  let expectedIds := expected.map TestEnum.toUInt16
  assertListEq gotIds expectedIds

private def f32bits (v : Float) : UInt32 :=
  Float32.toBits (Float.toFloat32 v)

private def assertFloat32Eq (actual expected : Float) : IO Unit :=
  assertEqual (f32bits actual) (f32bits expected)

private def assertFloat32ListEq (got : Capnp.ListReader Float) (expected : List Float) : IO Unit :=
  let gotBits := got.map f32bits
  let expectedBits := expected.map f32bits
  assertListEq gotBits expectedBits

private def getAt! {α} (xs : Capnp.ListReader α) (i : Nat) : IO α := do
  match xs.get? i with
  | some v => pure v
  | none => fail s!"index {i} out of bounds (size {xs.size})"

private def posInf : Float :=
  Float.ofBits (0x7ff0000000000000 : UInt64)

private def negInf : Float :=
  Float.ofBits (0xfff0000000000000 : UInt64)

private def assertIsNaN (v : Float) : IO Unit :=
  assertTrue v.isNaN

private def checkTestMessage (r : TestAllTypes.Reader) : IO Unit := do
  assertEqual r.getVoidField ()
  assertEqual r.getBoolField true
  assertEqual r.getInt8Field (Int8.ofInt (-123))
  assertEqual r.getInt16Field (Int16.ofInt (-12345))
  assertEqual r.getInt32Field (Int32.ofInt (-12345678))
  assertEqual r.getInt64Field (Int64.ofInt (-123456789012345))
  assertEqual r.getUInt8Field (UInt8.ofNat 234)
  assertEqual r.getUInt16Field (UInt16.ofNat 45678)
  assertEqual r.getUInt32Field (UInt32.ofNat 3456789012)
  assertEqual r.getUInt64Field (UInt64.ofNat 12345678901234567890)
  assertFloat32Eq r.getFloat32Field 1234.5
  assertEqual r.getFloat64Field (-123e45)
  assertEqual r.getTextField "foo"
  assertEqual r.getDataField (bytes "bar")
  let sub := r.getStructField
  assertEqual sub.getVoidField ()
  assertEqual sub.getBoolField true
  assertEqual sub.getInt8Field (Int8.ofInt (-12))
  assertEqual sub.getInt16Field (Int16.ofInt 3456)
  assertEqual sub.getInt32Field (Int32.ofInt (-78901234))
  assertEqual sub.getInt64Field (Int64.ofInt 56789012345678)
  assertEqual sub.getUInt8Field (UInt8.ofNat 90)
  assertEqual sub.getUInt16Field (UInt16.ofNat 1234)
  assertEqual sub.getUInt32Field (UInt32.ofNat 56789012)
  assertEqual sub.getUInt64Field (UInt64.ofNat 345678901234567890)
  assertFloat32Eq sub.getFloat32Field (-1.25e-10)
  assertEqual sub.getFloat64Field 345
  assertEqual sub.getTextField "baz"
  assertEqual sub.getDataField (bytes "qux")
  let subSub := sub.getStructField
  assertEqual subSub.getTextField "nested"
  assertEqual subSub.getStructField.getTextField "really nested"
  assertEnumEq sub.getEnumField TestEnum.baz
  assertListEq sub.getVoidList [(), (), ()]
  assertListEq sub.getBoolList [false, true, false, true, true]
  assertListEq sub.getInt8List [Int8.ofInt 12, Int8.ofInt (-34), Int8.ofInt (-128), Int8.ofInt 127]
  assertListEq sub.getInt16List [Int16.ofInt 1234, Int16.ofInt (-5678), Int16.ofInt (-32768), Int16.ofInt 32767]
  assertListEq sub.getInt32List
    [ Int32.ofInt 12345678
    , Int32.ofInt (-90123456)
    , Int32.ofInt (-2147483648)
    , Int32.ofInt 2147483647
    ]
  assertListEq sub.getInt64List
    [ Int64.ofInt 123456789012345
    , Int64.ofInt (-678901234567890)
    , Int64.ofInt (-9223372036854775808)
    , Int64.ofInt 9223372036854775807
    ]
  assertListEq sub.getUInt8List [UInt8.ofNat 12, UInt8.ofNat 34, UInt8.ofNat 0, UInt8.ofNat 255]
  assertListEq sub.getUInt16List [UInt16.ofNat 1234, UInt16.ofNat 5678, UInt16.ofNat 0, UInt16.ofNat 65535]
  assertListEq sub.getUInt32List
    [ UInt32.ofNat 12345678
    , UInt32.ofNat 90123456
    , UInt32.ofNat 0
    , UInt32.ofNat 4294967295
    ]
  assertListEq sub.getUInt64List
    [ UInt64.ofNat 123456789012345
    , UInt64.ofNat 678901234567890
    , UInt64.ofNat 0
    , UInt64.ofNat 18446744073709551615
    ]
  assertFloat32ListEq sub.getFloat32List [0.0, 1234567.0, 1e37, -1e37, 1e-37, -1e-37]
  assertListEq sub.getFloat64List [0.0, 123456789012345.0, 1e306, -1e306, 1e-306, -1e-306]
  assertListEq sub.getTextList ["quux", "corge", "grault"]
  assertListEq sub.getDataList [bytes "garply", bytes "waldo", bytes "fred"]
  let subStructs := sub.getStructList
  assertEqual subStructs.size 3
  assertEqual (← getAt! subStructs 0).getTextField "x structlist 1"
  assertEqual (← getAt! subStructs 1).getTextField "x structlist 2"
  assertEqual (← getAt! subStructs 2).getTextField "x structlist 3"
  assertEnumListEq sub.getEnumList [TestEnum.qux, TestEnum.bar, TestEnum.grault]
  assertEnumEq r.getEnumField TestEnum.corge
  assertEqual r.getVoidList.size 6
  assertListEq r.getBoolList [true, false, false, true]
  assertListEq r.getInt8List [Int8.ofInt 111, Int8.ofInt (-111)]
  assertListEq r.getInt16List [Int16.ofInt 11111, Int16.ofInt (-11111)]
  assertListEq r.getInt32List [Int32.ofInt 111111111, Int32.ofInt (-111111111)]
  assertListEq r.getInt64List
    [ Int64.ofInt 1111111111111111111
    , Int64.ofInt (-1111111111111111111)
    ]
  assertListEq r.getUInt8List [UInt8.ofNat 111, UInt8.ofNat 222]
  assertListEq r.getUInt16List [UInt16.ofNat 33333, UInt16.ofNat 44444]
  assertListEq r.getUInt32List [UInt32.ofNat 3333333333]
  assertListEq r.getUInt64List [UInt64.ofNat 11111111111111111111]
  let f32 := r.getFloat32List
  assertEqual f32.size 4
  assertFloat32Eq (← getAt! f32 0) 5555.5
  assertFloat32Eq (← getAt! f32 1) posInf
  assertFloat32Eq (← getAt! f32 2) negInf
  assertIsNaN (← getAt! f32 3)
  let f64 := r.getFloat64List
  assertEqual f64.size 4
  assertEqual (← getAt! f64 0) 7777.75
  assertEqual (← getAt! f64 1) posInf
  assertEqual (← getAt! f64 2) negInf
  assertIsNaN (← getAt! f64 3)
  assertListEq r.getTextList ["plugh", "xyzzy", "thud"]
  assertListEq r.getDataList [bytes "oops", bytes "exhausted", bytes "rfc3092"]
  let structs := r.getStructList
  assertEqual structs.size 3
  assertEqual (← getAt! structs 0).getTextField "structlist 1"
  assertEqual (← getAt! structs 1).getTextField "structlist 2"
  assertEqual (← getAt! structs 2).getTextField "structlist 3"
  assertEnumListEq r.getEnumList [TestEnum.foo, TestEnum.garply]

@[test]
def testConformanceBinary : IO Unit := do
  let bytes ← IO.FS.readBinFile (testdataPath "binary")
  let msg := Capnp.readMessage bytes
  checkTestMessage (TestAllTypes.read (Capnp.getRoot msg))

@[test]
def testConformanceSegmented : IO Unit := do
  let bytes ← IO.FS.readBinFile (testdataPath "segmented")
  let msg := Capnp.readMessage bytes
  checkTestMessage (TestAllTypes.read (Capnp.getRoot msg))

@[test]
def testConformancePacked : IO Unit := do
  let bytes ← IO.FS.readBinFile (testdataPath "packed")
  let msg := Capnp.readMessagePacked bytes
  checkTestMessage (TestAllTypes.read (Capnp.getRoot msg))

@[test]
def testConformanceSegmentedPacked : IO Unit := do
  let bytes ← IO.FS.readBinFile (testdataPath "segmented-packed")
  let msg := Capnp.readMessagePacked bytes
  checkTestMessage (TestAllTypes.read (Capnp.getRoot msg))

@[test]
def testConformanceFlat : IO Unit := do
  let bytes ← IO.FS.readBinFile (testdataPath "flat")
  let msg := Capnp.messageOfSegment bytes
  checkTestMessage (TestAllTypes.read (Capnp.getRoot msg))

@[test]
def testConformancePackedFlat : IO Unit := do
  let bytes ← IO.FS.readBinFile (testdataPath "packedflat")
  let unpacked := Capnp.unpack bytes
  let msg := Capnp.messageOfSegment unpacked
  checkTestMessage (TestAllTypes.read (Capnp.getRoot msg))
