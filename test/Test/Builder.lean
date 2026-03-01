import LeanTest
import Capnp.Runtime
import Capnp.Gen.test.lean4.addressbook
import Capnp.Gen.test.lean4.fixtures.defaults

open LeanTest
open Capnp.Gen.test.lean4.addressbook
open Capnp.Gen.test.lean4.fixtures.defaults

def buildAddressBookMessage : Capnp.Message :=
  let init := Capnp.initMessageBuilder 32
  let (_, st) := (Capnp.runBuilder (do
    let book ← AddressBook.initRoot
    let people ← AddressBook.Builder.initPeople book 1
    match people.toList with
    | p :: _ =>
        Person.Builder.setId p 123
        Person.Builder.setName p "Alice"
        Person.Builder.setEmail p "alice@example.com"
        Person.Builder.setUnemployed p
        let phones ← Person.Builder.initPhones p 1
        match phones.toList with
        | ph :: _ =>
            Person.PhoneNumber.Builder.setNumber ph "555-0100"
            Person.PhoneNumber.Builder.setType ph Person.PhoneType.mobile
        | [] => pure ()
    | [] => pure ()
    ) init)
  Capnp.buildMessage st

def buildDefaultsMessage : Capnp.Message :=
  let init := Capnp.initMessageBuilder 64
  let dataBytes : ByteArray := ByteArray.mk #[9, 8, 7]
  let (_, st) := (Capnp.runBuilder (do
    let root ← Defaults.initRoot
    Defaults.Builder.setBoolField root false
    Defaults.Builder.setInt16Field root (Int16.ofInt (-321))
    Defaults.Builder.setUint32Field root 42
    Defaults.Builder.setFloat32Field root 2.5
    Defaults.Builder.setFloat64Field root (-3.75)
    Defaults.Builder.setEnumField root Color.blue
    Defaults.Builder.setTextField root "world"
    Defaults.Builder.setDataField root dataBytes
    Defaults.Builder.setListUInt16 root #[7, 8]
    Defaults.Builder.setListText root #["x", "y", "z"]
    let listList ← Defaults.Builder.initListListUInt8 root 2
    match listList.toList with
    | p0 :: p1 :: _ =>
        Capnp.writeListUInt8 p0 #[1, 2]
        Capnp.writeListUInt8 p1 #[3]
    | _ => pure ()
    let nested ← Defaults.Builder.initNested root
    Nested.Builder.setNum nested 42
    let nestedList ← Defaults.Builder.initNestedList root 2
    match nestedList.toList with
    | n0 :: n1 :: _ =>
        Nested.Builder.setNum n0 100
        Nested.Builder.setNum n1 200
    | _ => pure ()
    ) init)
  Capnp.buildMessage st

@[test]
def testBuildAddressBook : IO Unit := do
  let msg := buildAddressBookMessage
  let root := AddressBook.read (Capnp.getRoot msg)
  let people := root.getPeople
  assertEqual people.size 1
  match people.toList with
  | p :: _ =>
      assertEqual p.getId 123
      assertEqual p.getName "Alice"
      assertEqual p.getEmail "alice@example.com"
      let phones := p.getPhones
      assertEqual phones.size 1
      match phones.toList with
      | ph :: _ =>
          assertEqual ph.getNumber "555-0100"
          assertEqual (ph.getType == Person.PhoneType.mobile) true
      | [] => assertEqual true false
      let isUnemployed :=
        match p.which with
        | Person.Which.unemployed _ => true
        | _ => false
      assertEqual isUnemployed true
  | [] =>
      assertEqual true false

@[test]
def testUnionEqualityUsesPayload : IO Unit := do
  let a : Person.Which := Person.Which.employment "engineer"
  let b : Person.Which := Person.Which.employment "teacher"
  let c : Person.Which := Person.Which.employment "engineer"
  assertEqual (a == b) false
  assertEqual (a == c) true

@[test]
def testBuildDefaults : IO Unit := do
  let msg := buildDefaultsMessage
  let root := Defaults.read (Capnp.getRoot msg)
  assertEqual root.getBoolField false
  assertEqual root.getInt16Field (Int16.ofInt (-321))
  assertEqual root.getUint32Field 42
  assertEqual root.getFloat32Field 2.5
  assertEqual root.getFloat64Field (-3.75)
  assertEqual (root.getEnumField == Color.blue) true
  assertEqual root.getTextField "world"
  assertEqual root.getDataField (ByteArray.mk #[9, 8, 7])
  assertEqual root.getListUInt16.toList [7, 8]
  assertEqual root.getListText.toList ["x", "y", "z"]
  let listList := root.getListListUInt8
  assertEqual listList.size 2
  match listList.toList with
  | a :: b :: _ =>
      assertEqual a.toList [1, 2]
      assertEqual b.toList [3]
  | _ =>
      assertEqual true false
  assertEqual root.getNested.getNum 42
  let nestedList := root.getNestedList
  assertEqual nestedList.size 2
  match nestedList.toList with
  | n0 :: n1 :: _ =>
      assertEqual n0.getNum 100
      assertEqual n1.getNum 200
  | _ =>
      assertEqual true false

@[test]
def testReadDataView : IO Unit := do
  let msg := buildDefaultsMessage
  let root := Defaults.read (Capnp.getRoot msg)
  let view := Capnp.readDataView (Capnp.getPointer root.struct 1)
  assertEqual (Capnp.dataViewToByteArray view) (ByteArray.mk #[9, 8, 7])
