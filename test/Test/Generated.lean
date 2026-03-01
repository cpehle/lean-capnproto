import LeanTest
import Capnp.Runtime
import Capnp.Gen.test.lean4.fixtures.defaults

open LeanTest
open Capnp.Gen.test.lean4.fixtures.defaults

private def loadMessage (path : String) : IO Capnp.Message := do
  let bytes ← IO.FS.readBinFile path
  return Capnp.readMessage bytes

@[test]
def testDefaultsMessage : IO Unit := do
  let msg ← loadMessage "fixtures/defaults.bin"
  let r := Defaults.read (Capnp.getRoot msg)

  assertEqual (r.getBoolField) true
  assertEqual (r.getInt16Field) (Int16.ofInt (-123))
  assertEqual (r.getUint32Field) (UInt32.ofNat 123456)
  assertEqual (r.getFloat32Field) 1.5
  assertEqual (r.getFloat64Field) (-2.25)
  assertTrue (r.getEnumField == Color.green)
  assertEqual (r.getTextField) "hello"
  assertEqual (Capnp.textViewToString r.getTextFieldView) "hello"
  assertTrue (r.getDataField == ByteArray.mk #[97, 98, 99])
  assertEqual (Capnp.dataViewToByteArray r.getDataFieldView) (ByteArray.mk #[97, 98, 99])
  assertEqual (r.getListUInt16).toList [10, 20, 30]
  assertEqual (r.getListText).toList ["a", "b"]
  assertEqual ((r.getListListUInt8).toList.map (fun xs => xs.toList)) [[1, 2], [3]]
  assertEqual (r.getNested.getNum) (UInt32.ofNat 7)
  assertEqual ((r.getNestedList).toList.map (fun n => n.getNum)) [UInt32.ofNat 1, UInt32.ofNat 2]

@[test]
def testValuesMessage : IO Unit := do
  let msg ← loadMessage "fixtures/values.bin"
  let r := Defaults.read (Capnp.getRoot msg)

  assertEqual (r.getBoolField) false
  assertEqual (r.getInt16Field) (Int16.ofInt 321)
  assertEqual (r.getUint32Field) (UInt32.ofNat 999)
  assertEqual (r.getFloat32Field) 3.25
  assertEqual (r.getFloat64Field) 4.5
  assertTrue (r.getEnumField == Color.blue)
  assertEqual (r.getTextField) "world"
  match r.getTextFieldViewChecked with
  | Except.ok v => assertEqual (Capnp.textViewToString v) "world"
  | Except.error _ => assertEqual true false
  assertTrue (r.getDataField == ByteArray.mk #[120, 121, 122])
  match r.getDataFieldViewChecked with
  | Except.ok v => assertEqual (Capnp.dataViewToByteArray v) (ByteArray.mk #[120, 121, 122])
  | Except.error _ => assertEqual true false
  assertEqual (r.getListUInt16).toList [7, 8]
  assertEqual (r.getListText).toList ["x"]
  assertEqual ((r.getListListUInt8).toList.map (fun xs => xs.toList)) [[9], [10, 11]]
  assertEqual (r.getNested.getNum) (UInt32.ofNat 42)
  assertEqual ((r.getNestedList).toList.map (fun n => n.getNum)) [UInt32.ofNat 5]

@[test]
def testConstants : IO Unit := do
  assertEqual nestedConst.num (UInt32.ofNat 99)
  assertEqual listConst.toList [4, 5]
  assertEqual (listNestedConst.toList.map (fun n => n.num)) [UInt32.ofNat 8]
