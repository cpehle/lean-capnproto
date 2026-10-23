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

private def boundarySegment (words : Array UInt64) : Capnp.SegmentView :=
  let bytes := words.foldl (fun bytes (word : UInt64) =>
    Capnp.appendUInt32LE (Capnp.appendUInt32LE bytes word.toUInt32)
      (word >>> 32).toUInt32) ByteArray.empty
  { bytes, off := 0, len := bytes.size, h := by simp }

private def boundaryRoot (segments : Array (Array UInt64)) : Capnp.AnyPointer :=
  Capnp.getRoot { segments := segments.map boundarySegment }

@[test]
def testCheckedDirectEmptyListAtSegmentEnd : IO Unit := do
  -- The root pointer occupies the entire segment; the zero-length UInt32 list
  -- starts immediately after it. One-past-the-end is valid for zero extent.
  let empty := boundaryRoot #[#[0x0000000400000001]]
  let reader ← IO.ofExcept <| (Capnp.readListUInt32CheckedReader empty).mapError IO.userError
  assertEqual reader.size 0
  let nonempty := boundaryRoot #[#[0x0000000c00000001]]
  assertEqual (Capnp.readListUInt32CheckedReader nonempty).isOk false
  let beyond := boundaryRoot #[#[0x0000000400000005]]
  assertEqual (Capnp.readListUInt32CheckedReader beyond).isOk false
  -- Even an empty inline-composite list needs its element-layout tag word.
  let missingTag := boundaryRoot #[#[0x0000000700000001]]
  assertEqual (Capnp.decodeListPtrChecked missingTag).isOk false

@[test]
def testCheckedDirectEmptyStructAtSegmentEnd : IO Unit := do
  -- Nonzero offset distinguishes this empty struct pointer from null.
  let empty := boundaryRoot #[#[4, 0xaaaaaaaaaaaaaaaa]]
  let reader ← IO.ofExcept <| (Capnp.readStructChecked empty).mapError IO.userError
  assertEqual reader.dataWords 0
  assertEqual reader.ptrCount 0
  let nonempty := boundaryRoot #[#[0x0000000100000004, 0xaaaaaaaaaaaaaaaa]]
  assertEqual (Capnp.readStructChecked nonempty).isOk false

@[test]
def testCheckedSingleFarEmptyListAtSegmentEnd : IO Unit := do
  -- Segment 1 contains only the landing pad, whose direct pointer targets its end.
  let empty := boundaryRoot #[#[0x0000000100000002], #[0x0000000400000001]]
  let reader ← IO.ofExcept <| (Capnp.readListUInt32CheckedReader empty).mapError IO.userError
  assertEqual reader.size 0
  let nonempty := boundaryRoot #[#[0x0000000100000002], #[0x0000000c00000001]]
  assertEqual (Capnp.readListUInt32CheckedReader nonempty).isOk false
  let missingPad := boundaryRoot #[#[0x0000000100000002], #[]]
  assertEqual (Capnp.readListUInt32CheckedReader missingPad).isOk false

@[test]
def testCheckedDoubleFarEmptyObjectsAtSegmentEnd : IO Unit := do
  -- A double-far pointer separates the two-word landing pad from the object.
  -- Both an empty target segment and the end of a nonempty target are valid.
  for (target, far) in #[((#[] : Array UInt64), (0x0000000200000002 : UInt64)),
      (#[0xaaaaaaaaaaaaaaaa], 0x000000020000000a)] do
    let emptyList := boundaryRoot #[#[0x0000000100000006], #[far, 0x0000000400000001], target]
    let list ← IO.ofExcept <| (Capnp.readListUInt32CheckedReader emptyList).mapError IO.userError
    assertEqual list.size 0
    let emptyStruct := boundaryRoot #[#[0x0000000100000006], #[far, 0], target]
    let struct ← IO.ofExcept <| (Capnp.readStructChecked emptyStruct).mapError IO.userError
    assertEqual struct.dataWords 0
    assertEqual struct.ptrCount 0
    let nonemptyList := boundaryRoot #[#[0x0000000100000006], #[far, 0x0000000c00000001], target]
    assertEqual (Capnp.readListUInt32CheckedReader nonemptyList).isOk false
    let nonemptyStruct := boundaryRoot #[#[0x0000000100000006], #[far, 0x0000000100000000], target]
    assertEqual (Capnp.readStructChecked nonemptyStruct).isOk false
  let beyond := boundaryRoot #[#[0x0000000100000006],
    #[0x0000000200000012, 0x0000000400000001], #[0xaaaaaaaaaaaaaaaa]]
  assertEqual (Capnp.readListUInt32CheckedReader beyond).isOk false
  let missingTag := boundaryRoot #[#[0x0000000100000006], #[0x0000000200000002], #[]]
  assertEqual (Capnp.readListUInt32CheckedReader missingTag).isOk false
