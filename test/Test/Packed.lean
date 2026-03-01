import LeanTest
import Capnp.Runtime
import Test.Builder
import Capnp.Gen.test.lean4.fixtures.defaults

open LeanTest
open Capnp.Gen.test.lean4.fixtures.defaults

@[test]
def testPackRoundtrip : IO Unit := do
  let msg := buildDefaultsMessage
  let bytes := Capnp.writeMessage msg
  let packed := Capnp.pack bytes
  let unpacked := Capnp.unpack packed
  assertEqual unpacked bytes

@[test]
def testReadPackedMessage : IO Unit := do
  let msg := buildDefaultsMessage
  let bytes := Capnp.writeMessage msg
  let packed := Capnp.pack bytes
  let msg2 := Capnp.readMessagePacked packed
  let root := Defaults.read (Capnp.getRoot msg2)
  assertEqual root.getTextField "world"
  assertEqual root.getUint32Field 42
  assertEqual root.getListUInt16.toList [7, 8]

@[test]
def testWriteMessageTo : IO Unit := do
  let msg := buildDefaultsMessage
  IO.FS.withTempFile (fun h path => do
    Capnp.writeMessageTo h msg
    IO.FS.Handle.flush h
    let bytes ← IO.FS.readBinFile path
    assertEqual bytes (Capnp.writeMessage msg))

@[test]
def testWriteMessagePackedTo : IO Unit := do
  let msg := buildDefaultsMessage
  IO.FS.withTempFile (fun h path => do
    Capnp.writeMessagePackedTo h msg
    IO.FS.Handle.flush h
    let bytes ← IO.FS.readBinFile path
    assertEqual bytes (Capnp.writeMessagePacked msg))
