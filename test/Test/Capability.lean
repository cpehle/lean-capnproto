import LeanTest
import Capnp.Runtime
import Capnp.Gen.test.lean4.fixtures.capability

open LeanTest
open Capnp.Gen.test.lean4.fixtures.capability

def buildCapHolderMessage : Capnp.Message :=
  let init := Capnp.initMessageBuilder 16
  let (_, st) := (Capnp.runBuilder (do
    let root ← CapHolder.initRoot
    CapHolder.Builder.setCap root (UInt32.ofNat 7)
    CapHolder.Builder.setCaps root #[UInt32.ofNat 1, UInt32.ofNat 2, UInt32.ofNat 3]
    ) init)
  Capnp.buildMessage st

def buildAnyHolderMessage : Capnp.Message :=
  let init := Capnp.initMessageBuilder 32
  let (_, st) := (Capnp.runBuilder (do
    let root ← AnyHolder.initRoot
    let anyPtr := AnyHolder.Builder.getAny root
    let sb ← Capnp.initStructPointer anyPtr 1 0
    let small := Small.Builder.fromStruct sb
    Small.Builder.setX small 123
    ) init)
  Capnp.buildMessage st

@[test]
def testCapabilityPointers : IO Unit := do
  let msg := buildCapHolderMessage
  let root := CapHolder.read (Capnp.getRoot msg)
  assertEqual root.getCap (UInt32.ofNat 7)
  assertEqual root.getCaps.toList [UInt32.ofNat 1, UInt32.ofNat 2, UInt32.ofNat 3]

@[test]
def testAnyPointerRoundtrip : IO Unit := do
  let msg := buildAnyHolderMessage
  let root := AnyHolder.read (Capnp.getRoot msg)
  let anyPtr := root.getAny
  let small := Small.read anyPtr
  assertEqual small.getX (UInt32.ofNat 123)
