import Init
import Init.Data.String.Extra
import Init.Data.UInt.Lemmas
import Init.System.IO
namespace Capnp

abbrev Text := String
abbrev Data := ByteArray
abbrev Capability := UInt32
abbrev Unknown := Unit

structure SegmentView where
  bytes : ByteArray
  off : Nat
  len : Nat
  h : off + len ≤ bytes.size

abbrev DataView := SegmentView
abbrev TextView := SegmentView

instance : BEq SegmentView where
  beq a b := a.bytes == b.bytes && a.off == b.off && a.len == b.len

structure Message where
  segments : Array SegmentView
  deriving Inhabited, BEq

structure ReaderOptions where
  maxSegments : Nat := 512
  maxMessageWords : Nat := 1_000_000
  maxNestingDepth : Nat := 64
  deriving Inhabited

structure AnyPointer where
  msg : Message
  seg : Nat
  word : Nat
  deriving Inhabited, BEq

structure ResolvedPointer where
  msg : Message
  tagSeg : Nat
  tagWord : Nat
  targetSeg : Nat
  targetWord : Nat
  deriving Inhabited, BEq

structure StructReader where
  msg : Message
  seg : Nat
  dataOff : Nat
  dataWords : Nat
  ptrOff : Nat
  ptrCount : Nat
  deriving Inhabited, BEq

structure ListPointer where
  msg : Message
  seg : Nat
  startWord : Nat
  elemSize : Nat
  elemCount : Nat
  structDataWords : Nat
  structPtrCount : Nat
  inlineComposite : Bool
  deriving Inhabited, BEq

structure ListReader (α : Type) where
  size : Nat
  get : (i : Nat) -> i < size -> α

structure MessageBuilder where
  segments : Array ByteArray
  usedWords : Array Nat
  currentSeg : Nat
  nextWord : Nat
  defaultSegmentWords : Nat
  deriving Inhabited

structure AnyPointerBuilder where
  seg : Nat
  word : Nat
  deriving Inhabited, BEq

structure CapTable where
  caps : Array Capability
  deriving Inhabited, BEq

@[inline] def emptyCapTable : CapTable := { caps := #[] }

@[inline] def capTableAdd (t : CapTable) (cap : Capability) : Capability × CapTable :=
  let idx := t.caps.size.toUInt32
  (idx, { caps := t.caps.push cap })

@[inline] def capTableGet (t : CapTable) (idx : Capability) : Option Capability :=
  let idxNat := idx.toNat
  if _h : idxNat < t.caps.size then
    some (t.caps.getD idxNat 0)
  else
    none

structure RpcEnvelope where
  msg : Message
  capTable : CapTable
  deriving Inhabited, BEq


structure StructBuilder where
  seg : Nat
  dataOff : Nat
  dataWords : Nat
  ptrOff : Nat
  ptrCount : Nat
  deriving Inhabited, BEq

structure ListBuilder where
  seg : Nat
  startWord : Nat
  elemSize : Nat
  elemCount : Nat
  structDataWords : Nat
  structPtrCount : Nat
  inlineComposite : Bool
  deriving Inhabited, BEq

abbrev BuilderM := StateM MessageBuilder

@[inline] def emptyMessage : Message := { segments := #[] }

@[inline] def emptyRpcEnvelope : RpcEnvelope := { msg := emptyMessage, capTable := emptyCapTable }

@[inline] def emptyStruct (msg : Message) (seg : Nat) : StructReader :=
  { msg := msg, seg := seg, dataOff := 0, dataWords := 0, ptrOff := 0, ptrCount := 0 }

@[inline] def ListReader.empty : ListReader α :=
  { size := 0
  , get := by
      intro i h
      exact (False.elim ((Nat.not_lt_zero i) h))
  }

@[inline] def ListReader.map (f : α → β) (r : ListReader α) : ListReader β :=
  { size := r.size
  , get := fun i h => f (r.get i h)
  }

@[inline] def ListReader.toArray (r : ListReader α) : Array α :=
  Array.ofFn (fun i => r.get i.val i.isLt)

@[inline] def ListReader.toList (r : ListReader α) : List α :=
  r.toArray.toList

instance [BEq α] : BEq (ListReader α) where
  beq a b := a.toArray == b.toArray

@[inline] def ListReader.get? (r : ListReader α) (i : Nat) : Option α :=
  if h : i < r.size then
    some (r.get i h)
  else
    none

@[inline] def messageOfSegment (segment : ByteArray) : Message :=
  { segments := #[{ bytes := segment, off := 0, len := segment.size, h := by simp }] }

@[inline] def segmentView (bytes : ByteArray) (off : Nat) (len : Nat)
    (h : off + len ≤ bytes.size) : SegmentView :=
  { bytes := bytes, off := off, len := len, h := h }

@[inline] def subSegmentView (seg : SegmentView) (start : Nat) (len : Nat)
    (h : start + len ≤ seg.len) : SegmentView :=
  let off := seg.off + start
  have h1 : seg.off + (start + len) ≤ seg.off + seg.len := Nat.add_le_add_left h seg.off
  have h2 : seg.off + seg.len ≤ seg.bytes.size := seg.h
  have h' : off + len ≤ seg.bytes.size := by
    simpa [off, Nat.add_assoc] using (Nat.le_trans h1 h2)
  { bytes := seg.bytes, off := off, len := len, h := h' }

@[inline] def getRoot (msg : Message) : AnyPointer :=
  { msg := msg, seg := 0, word := 0 }

@[inline] def emptySegmentView : SegmentView :=
  { bytes := ByteArray.empty, off := 0, len := 0, h := by decide }

instance : Inhabited SegmentView := ⟨emptySegmentView⟩

@[inline] def getSegment (msg : Message) (seg : Nat) : SegmentView :=
  msg.segments.getD seg emptySegmentView

@[inline] def segmentSize (seg : SegmentView) : Nat :=
  seg.len

@[inline] def readSegmentByte (seg : SegmentView) (byteOff : Nat) : UInt8 :=
  if byteOff < seg.len then
    seg.bytes.get! (seg.off + byteOff)
  else
    0

@[inline] def segmentUSize (seg : SegmentView) : USize :=
  seg.len.toUSize

@[inline] def toUSizeToNatLe (n : Nat) : (n.toUSize).toNat ≤ n := by
  have hmod : n % (2 ^ System.Platform.numBits) ≤ n := Nat.mod_le _ _
  simpa [USize.toNat_ofNat'] using hmod

@[inline] def readSegmentByteU (seg : SegmentView) (byteOff : USize) (h : byteOff.toNat < seg.len) : UInt8 :=
  seg.bytes.uget (seg.off.toUSize + byteOff)
    (by
      have h1 : seg.off + byteOff.toNat < seg.off + seg.len := by
        exact Nat.add_lt_add_left h seg.off
      have hsum : seg.off + byteOff.toNat < seg.bytes.size := by
        exact Nat.lt_of_lt_of_le h1 seg.h
      have hoff : (seg.off.toUSize).toNat ≤ seg.off := by
        simpa [USize.toNat_ofNat'] using (Nat.mod_le seg.off (2 ^ System.Platform.numBits))
      have hsum' : seg.off.toUSize.toNat + byteOff.toNat ≤ seg.off + byteOff.toNat := by
        exact Nat.add_le_add_right hoff _
      have hmod' : (seg.off.toUSize + byteOff).toNat ≤ seg.off.toUSize.toNat + byteOff.toNat := by
        have : (seg.off.toUSize.toNat + byteOff.toNat) % (2 ^ System.Platform.numBits) ≤
            seg.off.toUSize.toNat + byteOff.toNat := Nat.mod_le _ _
        simpa [USize.toNat_add] using this
      have hmod : (seg.off.toUSize + byteOff).toNat ≤ seg.off + byteOff.toNat :=
        Nat.le_trans hmod' hsum'
      exact Nat.lt_of_le_of_lt hmod hsum)

@[inline] def readByteArray (bytes : ByteArray) (byteOff : Nat) : UInt8 :=
  if h : byteOff < bytes.size then
    bytes.get byteOff h
  else
    0

@[inline] def shl64 (x : UInt64) (n : Nat) : UInt64 :=
  x <<< (UInt64.ofNat n)

@[inline] def shr64 (x : UInt64) (n : Nat) : UInt64 :=
  x >>> (UInt64.ofNat n)

@[inline] def shl32 (x : UInt32) (n : Nat) : UInt32 :=
  x <<< (UInt32.ofNat n)

@[inline] def shr32 (x : UInt32) (n : Nat) : UInt32 :=
  x >>> (UInt32.ofNat n)

@[inline] def readUInt32LE (bytes : ByteArray) (byteOff : Nat) : UInt32 :=
  let b0 := (readByteArray bytes byteOff).toUInt32
  let b1 := ((readByteArray bytes (byteOff + 1)).toUInt32) <<< 8
  let b2 := ((readByteArray bytes (byteOff + 2)).toUInt32) <<< 16
  let b3 := ((readByteArray bytes (byteOff + 3)).toUInt32) <<< 24
  b0 ||| b1 ||| b2 ||| b3

def readMessage (bytes : ByteArray) : Message :=
  if bytes.size < 4 then
    emptyMessage
  else
    let segCount := (readUInt32LE bytes 0).toNat + 1
    let headerWords := segCount + 1
    let headerWordsPadded := if headerWords % 2 == 0 then headerWords else headerWords + 1
    let headerBytes := headerWordsPadded * 4
    if bytes.size < headerBytes then
      emptyMessage
    else
      Id.run do
        let mut offset := headerBytes
        let mut segments := Array.mkEmpty segCount
        let mut ok := true
        for i in [0:segCount] do
          if ok then
            let sizeWords := readUInt32LE bytes (4 + i * 4)
            let sizeBytes := sizeWords.toNat * 8
            if h : offset + sizeBytes <= bytes.size then
              segments := segments.push (segmentView bytes offset sizeBytes h)
              offset := offset + sizeBytes
            else
              ok := false
        if ok then
          return { segments := segments }
        else
          return emptyMessage

def readMessageChecked (opts : ReaderOptions) (bytes : ByteArray) : Except String Message := Id.run do
  if bytes.size < 4 then
    return Except.error "message header too small"
  let segCount := (readUInt32LE bytes 0).toNat + 1
  if segCount > opts.maxSegments then
    return Except.error "too many segments"
  let headerWords := segCount + 1
  let headerWordsPadded := if headerWords % 2 == 0 then headerWords else headerWords + 1
  let headerBytes := headerWordsPadded * 4
  if bytes.size < headerBytes then
    return Except.error "truncated segment header"
  let mut offset := headerBytes
  let mut segments := Array.mkEmpty segCount
  let mut totalWords := 0
  for i in [0:segCount] do
    let sizeWords := readUInt32LE bytes (4 + i * 4)
    let sizeBytes := sizeWords.toNat * 8
    totalWords := totalWords + sizeWords.toNat
    if totalWords > opts.maxMessageWords then
      return Except.error "message too large"
    if h : offset + sizeBytes <= bytes.size then
      segments := segments.push (segmentView bytes offset sizeBytes h)
      offset := offset + sizeBytes
    else
      return Except.error "truncated segment data"
  return Except.ok { segments := segments }

@[inline] def appendUInt32LE (ba : ByteArray) (v : UInt32) : ByteArray :=
  Id.run do
    let mut out := ba
    out := out.push v.toUInt8
    out := out.push ((v >>> 8).toUInt8)
    out := out.push ((v >>> 16).toUInt8)
    out := out.push ((v >>> 24).toUInt8)
    return out

@[inline] def appendByteArray (ba : ByteArray) (src : ByteArray) : ByteArray :=
  Id.run do
    let mut out := ba
    for i in [0:src.size] do
      out := out.push (src.get! i)
    return out

@[inline] def appendSegmentView (ba : ByteArray) (seg : SegmentView) : ByteArray :=
  Id.run do
    let mut out := ba
    for i in [0:seg.len] do
      out := out.push (readSegmentByte seg i)
    return out

def writeMessage (msg : Message) : ByteArray :=
  let segCount := msg.segments.size
  if segCount == 0 then
    ByteArray.empty
  else
    let headerWords := segCount + 1
    let headerWordsPadded := if headerWords % 2 == 0 then headerWords else headerWords + 1
    let headerBytes := headerWordsPadded * 4
    let totalSegBytes := msg.segments.foldl (fun acc s => acc + s.len) 0
    let total := headerBytes + totalSegBytes
    Id.run do
      let mut out := ByteArray.emptyWithCapacity total
      out := appendUInt32LE out (UInt32.ofNat (segCount - 1))
      for i in [0:segCount] do
        let seg := msg.segments.getD i emptySegmentView
        out := appendUInt32LE out (UInt32.ofNat (seg.len / 8))
      if headerWords % 2 != 0 then
        out := appendUInt32LE out 0
      for i in [0:segCount] do
        let seg := msg.segments.getD i emptySegmentView
        out := appendSegmentView out seg
      return out

@[inline] def writeMessageHeader (msg : Message) : ByteArray :=
  let segCount := msg.segments.size
  if segCount == 0 then
    ByteArray.empty
  else
    let headerWords := segCount + 1
    let headerWordsPadded := if headerWords % 2 == 0 then headerWords else headerWords + 1
    let headerBytes := headerWordsPadded * 4
    Id.run do
      let mut out := ByteArray.emptyWithCapacity headerBytes
      out := appendUInt32LE out (UInt32.ofNat (segCount - 1))
      for i in [0:segCount] do
        let seg := msg.segments.getD i emptySegmentView
        out := appendUInt32LE out (UInt32.ofNat (seg.len / 8))
      if headerWords % 2 != 0 then
        out := appendUInt32LE out 0
      return out

@[inline] def writeMessageParts (msg : Message) : ByteArray × Array SegmentView :=
  let segCount := msg.segments.size
  if segCount == 0 then
    (ByteArray.empty, #[])
  else
    (writeMessageHeader msg, msg.segments)

def writeMessageTo (h : IO.FS.Handle) (msg : Message) : IO Unit := do
  let (header, segs) := writeMessageParts msg
  if header.size != 0 then
    IO.FS.Handle.write h header
  for seg in segs do
    if seg.len == 0 then
      pure ()
    else if seg.off == 0 && seg.len == seg.bytes.size then
      IO.FS.Handle.write h seg.bytes
    else
      IO.FS.Handle.write h (seg.bytes.extract seg.off (seg.off + seg.len))

@[inline] def bitMask8 (i : Nat) : UInt8 :=
  (1 : UInt8) <<< (UInt8.ofNat i)

@[inline] def testBit8 (tag : UInt8) (i : Nat) : Bool :=
  (((tag >>> (UInt8.ofNat i)) &&& (1 : UInt8)) == (1 : UInt8))

@[inline] def wordIsZeroBytes (bytes : ByteArray) (wordOff : Nat) : Bool :=
  Id.run do
    let base := wordOff * 8
    let mut ok := true
    for i in [0:8] do
      if readByteArray bytes (base + i) != 0 then
        ok := false
    return ok

@[inline] def wordIsFullBytes (bytes : ByteArray) (wordOff : Nat) : Bool :=
  Id.run do
    let base := wordOff * 8
    let mut ok := true
    for i in [0:8] do
      if readByteArray bytes (base + i) == 0 then
        ok := false
    return ok

def pack (bytes : ByteArray) : ByteArray :=
  Id.run do
    let wordCount := (bytes.size + 7) / 8
    let mut out := ByteArray.emptyWithCapacity (bytes.size + wordCount + 16)
    let mut i := 0
    while i < wordCount do
      let base := i * 8
      let mut tag : UInt8 := 0
      let mut buf : Array UInt8 := Array.mkEmpty 8
      for j in [0:8] do
        let b := readByteArray bytes (base + j)
        buf := buf.push b
        if b != 0 then
          tag := tag ||| bitMask8 j
      out := out.push tag
      if tag == 0 then
        let mut run := 0
        let mut k := i + 1
        while k < wordCount && run < 255 && wordIsZeroBytes bytes k do
          run := run + 1
          k := k + 1
        out := out.push (UInt8.ofNat run)
        i := i + 1 + run
      else if tag == 0xFF then
        for j in [0:buf.size] do
          out := out.push (buf.getD j 0)
        let mut run := 0
        let mut k := i + 1
        let mut extra := ByteArray.emptyWithCapacity 0
        while k < wordCount && run < 255 && wordIsFullBytes bytes k do
          let base2 := k * 8
          for j in [0:8] do
            extra := extra.push (readByteArray bytes (base2 + j))
          run := run + 1
          k := k + 1
        out := out.push (UInt8.ofNat run)
        out := appendByteArray out extra
        i := i + 1 + run
      else
        for j in [0:buf.size] do
          let b := buf.getD j 0
          if b != 0 then
            out := out.push b
        i := i + 1
    return out

structure ByteCursor where
  header : ByteArray
  segs : Array SegmentView
  rem : Nat
  headerPos : Nat
  segIdx : Nat
  segPos : Nat

structure WordCursor where
  cursor : ByteCursor
  pending : Option (Array UInt8)

@[inline] def nextByte (c : ByteCursor) : UInt8 × ByteCursor :=
  if c.rem == 0 then
    (0, c)
  else if h : c.headerPos < c.header.size then
    let b := c.header.get c.headerPos h
    (b, { c with rem := c.rem - 1, headerPos := c.headerPos + 1 })
  else
    Id.run do
      let mut idx := c.segIdx
      let mut pos := c.segPos
      let mut seg := emptySegmentView
      let mut found := false
      while idx < c.segs.size && !found do
        seg := c.segs.getD idx emptySegmentView
        if pos < seg.len then
          found := true
        else
          idx := idx + 1
          pos := 0
      if found then
        let b := readSegmentByte seg pos
        return (b, { c with rem := c.rem - 1, segIdx := idx, segPos := pos + 1 })
      else
        return (0, { c with rem := c.rem - 1, segIdx := idx, segPos := pos })

@[inline] def takeWord (c : WordCursor) : Array UInt8 × WordCursor :=
  match c.pending with
  | some w => (w, { c with pending := none })
  | none =>
      Id.run do
        let mut arr := Array.mkEmpty 8
        let mut cur := c.cursor
        for _ in [0:8] do
          let (b, cur') := nextByte cur
          arr := arr.push b
          cur := cur'
        return (arr, { c with cursor := cur })

@[inline] def wordTag (word : Array UInt8) : UInt8 :=
  Id.run do
    let mut tag : UInt8 := 0
    for i in [0:8] do
      let b := word.getD i 0
      if b != 0 then
        tag := tag ||| bitMask8 i
    return tag

@[inline] def wordIsZero (word : Array UInt8) : Bool :=
  Id.run do
    let mut ok := true
    for i in [0:8] do
      if word.getD i 0 != 0 then
        ok := false
    return ok

@[inline] def wordIsFull (word : Array UInt8) : Bool :=
  Id.run do
    let mut ok := true
    for i in [0:8] do
      if word.getD i 0 == 0 then
        ok := false
    return ok

def writeMessagePackedTo (h : IO.FS.Handle) (msg : Message) : IO Unit := do
  let header := writeMessageHeader msg
  let segs := msg.segments
  let totalBytes := header.size + segs.foldl (fun acc s => acc + s.len) 0
  if totalBytes == 0 then
    return ()
  let wordCount := (totalBytes + 7) / 8
  let mut cur : WordCursor :=
    { cursor := { header := header, segs := segs, rem := totalBytes, headerPos := 0, segIdx := 0, segPos := 0 }
    , pending := none
    }
  let mut i := 0
  let mut buf := ByteArray.emptyWithCapacity 4096
  while i < wordCount do
    let (word, cur') := takeWord cur
    cur := cur'
    let tag := wordTag word
    buf := buf.push tag
    if buf.size >= 4096 then
      IO.FS.Handle.write h buf
      buf := ByteArray.emptyWithCapacity 4096
    if tag == 0 then
      let mut run := 0
      let mut cur2 := cur
      let mut done := false
      while i + 1 + run < wordCount && run < 255 && !done do
        let (next, curNext) := takeWord cur2
        if wordIsZero next then
          run := run + 1
          cur2 := curNext
        else
          cur2 := { curNext with pending := some next }
          done := true
      buf := buf.push (UInt8.ofNat run)
      if buf.size >= 4096 then
        IO.FS.Handle.write h buf
        buf := ByteArray.emptyWithCapacity 4096
      cur := cur2
      i := i + 1 + run
    else if tag == 0xFF then
      for j in [0:8] do
        buf := buf.push (word.getD j 0)
        if buf.size >= 4096 then
          IO.FS.Handle.write h buf
          buf := ByteArray.emptyWithCapacity 4096
      let mut run := 0
      let mut cur2 := cur
      let mut extra := ByteArray.emptyWithCapacity 0
      let mut done := false
      while i + 1 + run < wordCount && run < 255 && !done do
        let (next, curNext) := takeWord cur2
        if wordIsFull next then
          run := run + 1
          cur2 := curNext
          for j in [0:8] do
            extra := extra.push (next.getD j 0)
        else
          cur2 := { curNext with pending := some next }
          done := true
      buf := buf.push (UInt8.ofNat run)
      if buf.size >= 4096 then
        IO.FS.Handle.write h buf
        buf := ByteArray.emptyWithCapacity 4096
      for j in [0:extra.size] do
        buf := buf.push (extra.get! j)
        if buf.size >= 4096 then
          IO.FS.Handle.write h buf
          buf := ByteArray.emptyWithCapacity 4096
      cur := cur2
      i := i + 1 + run
    else
      for j in [0:8] do
        let b := word.getD j 0
        if b != 0 then
          buf := buf.push b
          if buf.size >= 4096 then
            IO.FS.Handle.write h buf
            buf := ByteArray.emptyWithCapacity 4096
      i := i + 1
  if buf.size != 0 then
    IO.FS.Handle.write h buf

def unpack (bytes : ByteArray) : ByteArray :=
  Id.run do
    let mut out := ByteArray.emptyWithCapacity (bytes.size * 2)
    let mut i := 0
    while i < bytes.size do
      let tag := bytes.get! i
      i := i + 1
      if tag == 0 then
        for _ in [0:8] do
          out := out.push 0
        if i >= bytes.size then
          return out
        let run := (bytes.get! i).toNat
        i := i + 1
        for _ in [0:run] do
          for _ in [0:8] do
            out := out.push 0
      else if tag == 0xFF then
        for _ in [0:8] do
          let b := if i < bytes.size then bytes.get! i else 0
          i := i + 1
          out := out.push b
        if i >= bytes.size then
          return out
        let run := (bytes.get! i).toNat
        i := i + 1
        for _ in [0:run] do
          for _ in [0:8] do
            let b := if i < bytes.size then bytes.get! i else 0
            i := i + 1
            out := out.push b
      else
        for j in [0:8] do
          if testBit8 tag j then
            let b := if i < bytes.size then bytes.get! i else 0
            i := i + 1
            out := out.push b
          else
            out := out.push 0
    return out

def unpackChecked (bytes : ByteArray) : Except String ByteArray := Id.run do
  let mut out := ByteArray.emptyWithCapacity (bytes.size * 2)
  let mut i := 0
  let mut err : Option String := none
  let mut ok := true
  while ok && i < bytes.size do
    let tag := bytes.get! i
    i := i + 1
    if tag == 0 then
      for _ in [0:8] do
        out := out.push 0
      if i >= bytes.size then
        err := some "truncated zero run"
        ok := false
      else
        let run := (bytes.get! i).toNat
        i := i + 1
        for _ in [0:run] do
          for _ in [0:8] do
            out := out.push 0
    else if tag == 0xFF then
      if i + 8 > bytes.size then
        err := some "truncated nonzero word"
        ok := false
      else
        for _ in [0:8] do
          out := out.push (bytes.get! i)
          i := i + 1
        if i >= bytes.size then
          err := some "truncated nonzero run"
          ok := false
        else
          let run := (bytes.get! i).toNat
          i := i + 1
          if i + run * 8 > bytes.size then
            err := some "truncated nonzero run data"
            ok := false
          else
            for _ in [0:run] do
              for _ in [0:8] do
                out := out.push (bytes.get! i)
                i := i + 1
    else
      let mut needed := 0
      for j in [0:8] do
        if testBit8 tag j then
          needed := needed + 1
      if i + needed > bytes.size then
        err := some "truncated tag data"
        ok := false
      else
        for j in [0:8] do
          if testBit8 tag j then
            out := out.push (bytes.get! i)
            i := i + 1
          else
            out := out.push 0
  match err with
  | some e => return Except.error e
  | none => return Except.ok out

@[inline] def readMessagePacked (bytes : ByteArray) : Message :=
  readMessage (unpack bytes)

@[inline] def writeMessagePacked (msg : Message) : ByteArray :=
  pack (writeMessage msg)

def readMessagePackedChecked (opts : ReaderOptions) (bytes : ByteArray) : Except String Message := do
  match unpackChecked bytes with
  | Except.error e => Except.error e
  | Except.ok raw => readMessageChecked opts raw

@[inline] def readByte (msg : Message) (seg : Nat) (byteOff : Nat) : UInt8 :=
  readSegmentByte (getSegment msg seg) byteOff

@[inline] def mkArray (n : Nat) (f : Nat → α) : Array α :=
  Id.run do
    let mut arr := Array.mkEmpty n
    for i in [0:n] do
      arr := arr.push (f i)
    return arr

@[inline] def readBytes (msg : Message) (seg : Nat) (byteOff : Nat) (count : Nat) : ByteArray :=
  let segv := getSegment msg seg
  if h : byteOff + count ≤ segv.len then
    let arr := Array.ofFn (fun i : Fin count =>
      let idx := byteOff + i.1
      have hidx : idx < segv.len := by
        have hlt : byteOff + i.1 < byteOff + count := Nat.add_lt_add_left i.isLt byteOff
        exact Nat.lt_of_lt_of_le hlt h
      readSegmentByteU segv idx.toUSize (Nat.lt_of_le_of_lt (toUSizeToNatLe idx) hidx))
    ByteArray.mk arr
  else
    Id.run do
      let mut arr := Array.mkEmpty count
      for i in [0:count] do
        arr := arr.push (readByte msg seg (byteOff + i))
      return ByteArray.mk arr


@[inline] def readWord (msg : Message) (seg : Nat) (wordOff : Nat) : UInt64 :=
  Id.run do
    let base := wordOff * 8
    let mut acc : UInt64 := 0
    for i in [0:8] do
      let b := readByte msg seg (base + i)
      acc := acc ||| shl64 b.toUInt64 (8 * i)
    return acc

@[inline] def segmentWordCount (msg : Message) (seg : Nat) : Nat :=
  (segmentSize (getSegment msg seg)) / 8

@[inline] def readWordChecked (msg : Message) (seg : Nat) (wordOff : Nat) : Except String UInt64 :=
  if seg >= msg.segments.size then
    Except.error "segment out of bounds"
  else
    let segv := getSegment msg seg
    let byteOff := wordOff * 8
    if (byteOff + 8) ≤ segv.len then
      let bytes := readBytes msg seg byteOff 8
      let acc := Id.run do
        let mut acc : UInt64 := 0
        for i in [0:8] do
          let b := bytes.get! i
          acc := acc ||| shl64 b.toUInt64 (8 * i)
        return acc
      Except.ok acc
    else
      Except.error "word out of bounds"

@[inline] def signExtend (bits : Nat) (x : UInt64) : Int :=
  if bits == 0 then
    0
  else
    let mask : UInt64 := (shl64 (UInt64.ofNat 1) bits) - 1
    let v := x &&& mask
    let signBit : UInt64 := shl64 (UInt64.ofNat 1) (bits - 1)
    if (v &&& signBit) != 0 then
      Int.ofNat v.toNat - Int.ofNat ((shl64 (UInt64.ofNat 1) bits).toNat)
    else
      Int.ofNat v.toNat

@[inline] def toNat? (i : Int) : Option Nat :=
  if i < 0 then none else some i.toNat

@[inline] def resolveDirectPointer (msg : Message) (seg : Nat) (word : Nat) : Option ResolvedPointer :=
  let w := readWord msg seg word
  if w == 0 then
    none
  else if (w &&& 0x3) == 3 then
    none
  else if (w &&& 0x3) == 2 then
    none
  else
    let off := signExtend 30 ((shr64 w 2) &&& 0x3fffffff)
    let base : Int := Int.ofNat (word + 1)
    match toNat? (base + off) with
    | none => none
    | some t =>
        some { msg := msg, tagSeg := seg, tagWord := word, targetSeg := seg, targetWord := t }

@[inline] def resolvePointer (p : AnyPointer) : Option ResolvedPointer :=
  let w := readWord p.msg p.seg p.word
  if w == 0 then
    none
  else if (w &&& 0x3) != 2 then
    resolveDirectPointer p.msg p.seg p.word
  else
    let padSeg := ((shr64 w 32) &&& 0xffffffff).toNat
    let padWord := ((shr64 w 3) &&& 0x1fffffff).toNat
    if padSeg >= p.msg.segments.size then
      none
    else
      let isDouble := ((shr64 w 2) &&& 0x1) == 1
      if isDouble then
        let padWordBits := readWord p.msg padSeg padWord
        if (padWordBits &&& 0x3) != 2 then
          none
        else
          let targetSeg := ((shr64 padWordBits 32) &&& 0xffffffff).toNat
          let targetWord := ((shr64 padWordBits 3) &&& 0x1fffffff).toNat
          if targetSeg >= p.msg.segments.size then
            none
          else
            some { msg := p.msg, tagSeg := padSeg, tagWord := padWord + 1,
                   targetSeg := targetSeg, targetWord := targetWord }
      else
        resolveDirectPointer p.msg padSeg padWord

@[inline] def resolveDirectPointerChecked (msg : Message) (seg : Nat) (word : Nat) :
    Except String (Option ResolvedPointer) := do
  let w ← readWordChecked msg seg word
  if w == 0 then
    return none
  else if (w &&& 0x3) == 2 then
    throw "far pointer not allowed here"
  else if (w &&& 0x3) == 3 then
    throw "capability pointer not allowed here"
  else
    let off := signExtend 30 ((shr64 w 2) &&& 0x3fffffff)
    let base : Int := Int.ofNat (word + 1)
    match toNat? (base + off) with
    | none => throw "pointer offset underflow"
    | some t =>
        if t < segmentWordCount msg seg then
          return some { msg := msg, tagSeg := seg, tagWord := word,
                        targetSeg := seg, targetWord := t }
        else
          throw "pointer target out of bounds"

@[inline] def resolvePointerChecked (p : AnyPointer) : Except String (Option ResolvedPointer) := do
  let w ← readWordChecked p.msg p.seg p.word
  if w == 0 then
    return none
  else if (w &&& 0x3) == 3 then
    throw "capability pointer used as data pointer"
  else if (w &&& 0x3) != 2 then
    return (← resolveDirectPointerChecked p.msg p.seg p.word)
  else
    let padSeg := ((shr64 w 32) &&& 0xffffffff).toNat
    let padWord := ((shr64 w 3) &&& 0x1fffffff).toNat
    if padSeg >= p.msg.segments.size then
      throw "far pointer segment out of bounds"
    let isDouble := ((shr64 w 2) &&& 0x1) == 1
    if isDouble then
      let padWordBits ← readWordChecked p.msg padSeg padWord
      if (padWordBits &&& 0x3) != 2 then
        throw "double-far landing pad invalid"
      let targetSeg := ((shr64 padWordBits 32) &&& 0xffffffff).toNat
      let targetWord := ((shr64 padWordBits 3) &&& 0x1fffffff).toNat
      if targetSeg >= p.msg.segments.size then
        throw "double-far target segment out of bounds"
      if targetWord >= segmentWordCount p.msg targetSeg then
        throw "double-far target out of bounds"
      return some { msg := p.msg, tagSeg := padSeg, tagWord := padWord + 1,
                    targetSeg := targetSeg, targetWord := targetWord }
    else
      return (← resolveDirectPointerChecked p.msg padSeg padWord)

@[inline] def decodeStructPtr (p : AnyPointer) : Option StructReader :=
  match resolvePointer p with
  | none => none
  | some r =>
      let w := readWord r.msg r.tagSeg r.tagWord
      if (w &&& 0x3) != 0 then
        none
      else
        let dataWords := ((shr64 w 32) &&& 0xffff).toNat
        let ptrCount := ((shr64 w 48) &&& 0xffff).toNat
        some { msg := r.msg, seg := r.targetSeg, dataOff := r.targetWord, dataWords := dataWords,
               ptrOff := r.targetWord + dataWords, ptrCount := ptrCount }

@[inline] def decodeStructPtrChecked (p : AnyPointer) : Except String StructReader := do
  match (← resolvePointerChecked p) with
  | none =>
      return emptyStruct p.msg p.seg
  | some r =>
      let w ← readWordChecked r.msg r.tagSeg r.tagWord
      if (w &&& 0x3) != 0 then
        throw "struct pointer kind mismatch"
      let dataWords := ((shr64 w 32) &&& 0xffff).toNat
      let ptrCount := ((shr64 w 48) &&& 0xffff).toNat
      let total := dataWords + ptrCount
      if r.targetWord + total <= segmentWordCount r.msg r.targetSeg then
        return { msg := r.msg, seg := r.targetSeg, dataOff := r.targetWord,
                 dataWords := dataWords, ptrOff := r.targetWord + dataWords,
                 ptrCount := ptrCount }
      else
        throw "struct out of bounds"

@[inline] def readStruct (p : AnyPointer) : StructReader :=
  match decodeStructPtr p with
  | some s => s
  | none => emptyStruct p.msg p.seg

@[inline] def readStructChecked (p : AnyPointer) : Except String StructReader :=
  decodeStructPtrChecked p

@[inline] def getPointer (r : StructReader) (index : Nat) : AnyPointer :=
  { msg := r.msg, seg := r.seg, word := r.ptrOff + index }

@[inline] def isNullPointer (p : AnyPointer) : Bool :=
  readWord p.msg p.seg p.word == 0

@[inline] def withDefaultPointer (p : AnyPointer) (defaultPtr : AnyPointer) : AnyPointer :=
  if isNullPointer p then defaultPtr else p

@[inline] def dataByteOff (r : StructReader) (byteOff : Nat) : Nat :=
  r.dataOff * 8 + byteOff

@[inline] def getBit (msg : Message) (seg : Nat) (bitOff : Nat) : Bool :=
  let byteOff := bitOff / 8
  let bit := bitOff % 8
  let b := readByte msg seg byteOff
  let v := (shr64 b.toUInt64 bit) &&& 0x1
  v == 1

@[inline] def getBool (r : StructReader) (bitOff : Nat) : Bool :=
  getBit r.msg r.seg (r.dataOff * 64 + bitOff)

@[inline] def getBoolMasked (r : StructReader) (bitOff : Nat) (mask : Bool) : Bool :=
  let v := getBool r bitOff
  if mask then !v else v

@[inline] def readUInt16 (msg : Message) (seg : Nat) (byteOff : Nat) : UInt16 :=
  let b0 := (readByte msg seg byteOff).toUInt16
  let b1 := ((readByte msg seg (byteOff + 1)).toUInt16) <<< 8
  b0 ||| b1

@[inline] def readUInt32 (msg : Message) (seg : Nat) (byteOff : Nat) : UInt32 :=
  let b0 := (readByte msg seg byteOff).toUInt32
  let b1 := ((readByte msg seg (byteOff + 1)).toUInt32) <<< 8
  let b2 := ((readByte msg seg (byteOff + 2)).toUInt32) <<< 16
  let b3 := ((readByte msg seg (byteOff + 3)).toUInt32) <<< 24
  b0 ||| b1 ||| b2 ||| b3

@[inline] def readUInt64 (msg : Message) (seg : Nat) (byteOff : Nat) : UInt64 :=
  Id.run do
    let mut acc : UInt64 := 0
    for i in [0:8] do
      let b := (readByte msg seg (byteOff + i)).toUInt64
      acc := acc ||| shl64 b (8 * i)
    return acc

@[inline] def getUInt8 (r : StructReader) (byteOff : Nat) : UInt8 :=
  readByte r.msg r.seg (dataByteOff r byteOff)

@[inline] def getUInt8Masked (r : StructReader) (byteOff : Nat) (mask : UInt8) : UInt8 :=
  (getUInt8 r byteOff) ^^^ mask

@[inline] def getUInt16 (r : StructReader) (byteOff : Nat) : UInt16 :=
  readUInt16 r.msg r.seg (dataByteOff r byteOff)

@[inline] def getUInt16Masked (r : StructReader) (byteOff : Nat) (mask : UInt16) : UInt16 :=
  (getUInt16 r byteOff) ^^^ mask

@[inline] def getUInt32 (r : StructReader) (byteOff : Nat) : UInt32 :=
  readUInt32 r.msg r.seg (dataByteOff r byteOff)

@[inline] def getUInt32Masked (r : StructReader) (byteOff : Nat) (mask : UInt32) : UInt32 :=
  (getUInt32 r byteOff) ^^^ mask

@[inline] def getUInt64 (r : StructReader) (byteOff : Nat) : UInt64 :=
  readUInt64 r.msg r.seg (dataByteOff r byteOff)

@[inline] def getUInt64Masked (r : StructReader) (byteOff : Nat) (mask : UInt64) : UInt64 :=
  (getUInt64 r byteOff) ^^^ mask

@[inline] def getInt8 (r : StructReader) (byteOff : Nat) : Int8 :=
  let u := (getUInt8 r byteOff).toUInt64
  Int8.ofInt (signExtend 8 u)

@[inline] def getInt8Masked (r : StructReader) (byteOff : Nat) (mask : UInt8) : Int8 :=
  let v := (getUInt8 r byteOff) ^^^ mask
  Int8.ofInt (signExtend 8 v.toUInt64)

@[inline] def getInt16 (r : StructReader) (byteOff : Nat) : Int16 :=
  let u := (getUInt16 r byteOff).toUInt64
  Int16.ofInt (signExtend 16 u)

@[inline] def getInt16Masked (r : StructReader) (byteOff : Nat) (mask : UInt16) : Int16 :=
  let v := (getUInt16 r byteOff) ^^^ mask
  Int16.ofInt (signExtend 16 v.toUInt64)

@[inline] def getInt32 (r : StructReader) (byteOff : Nat) : Int32 :=
  let u := (getUInt32 r byteOff).toUInt64
  Int32.ofInt (signExtend 32 u)

@[inline] def getInt32Masked (r : StructReader) (byteOff : Nat) (mask : UInt32) : Int32 :=
  let v := (getUInt32 r byteOff) ^^^ mask
  Int32.ofInt (signExtend 32 v.toUInt64)

@[inline] private def twoPow64Int : Int :=
  18446744073709551616

@[inline] private def int64FromBits (u : UInt64) : Int64 :=
  let signBit := (u &&& 0x8000000000000000) != 0
  let v : Int := if signBit then Int.ofNat u.toNat - twoPow64Int else Int.ofNat u.toNat
  Int64.ofInt v

@[inline] def getInt64 (r : StructReader) (byteOff : Nat) : Int64 :=
  int64FromBits (getUInt64 r byteOff)

@[inline] def getInt64Masked (r : StructReader) (byteOff : Nat) (mask : UInt64) : Int64 :=
  int64FromBits ((getUInt64 r byteOff) ^^^ mask)

@[inline] def float32FromBits (bits : UInt32) : Float :=
  let signBit := (bits &&& 0x80000000) != 0
  let exp := (shr32 bits 23) &&& 0xff
  let mant := bits &&& 0x7fffff
  let frac := UInt64.toFloat mant.toUInt64 / 8388608.0
  if exp == 0 then
    if mant == 0 then
      if signBit then -0.0 else 0.0
    else
      let value := Float.scaleB frac (-126)
      if signBit then -value else value
  else if exp == 255 then
    if mant == 0 then
      if signBit then Float.ofBits 0xfff0000000000000 else Float.ofBits 0x7ff0000000000000
    else
      Float.ofBits 0x7ff8000000000000
  else
    let value := Float.scaleB (1.0 + frac) (Int.ofNat exp.toNat - 127)
    if signBit then -value else value

@[inline] def getFloat32 (r : StructReader) (byteOff : Nat) : Float :=
  float32FromBits (getUInt32 r byteOff)

@[inline] def getFloat32Masked (r : StructReader) (byteOff : Nat) (mask : UInt32) : Float :=
  float32FromBits ((getUInt32 r byteOff) ^^^ mask)

@[inline] def getFloat64 (r : StructReader) (byteOff : Nat) : Float :=
  Float.ofBits (getUInt64 r byteOff)

@[inline] def getFloat64Masked (r : StructReader) (byteOff : Nat) (mask : UInt64) : Float :=
  Float.ofBits ((getUInt64 r byteOff) ^^^ mask)

@[inline] def elemSizeVoid : Nat := 0
@[inline] def elemSizeBit : Nat := 1
@[inline] def elemSizeByte : Nat := 2
@[inline] def elemSizeTwoBytes : Nat := 3
@[inline] def elemSizeFourBytes : Nat := 4
@[inline] def elemSizeEightBytes : Nat := 5
@[inline] def elemSizePointer : Nat := 6
@[inline] def elemSizeInlineComposite : Nat := 7

@[inline] def decodeListPtr (p : AnyPointer) : Option ListPointer :=
  match resolvePointer p with
  | none => none
  | some r =>
      let w := readWord r.msg r.tagSeg r.tagWord
      if (w &&& 0x3) != 1 then
        none
      else
        let elemSize := ((shr64 w 32) &&& 0x7).toNat
        let elemCount := ((shr64 w 35) &&& 0x1fffffff).toNat
        if elemSize == elemSizeInlineComposite then
          let tag := readWord r.msg r.targetSeg r.targetWord
          if (tag &&& 0x3) != 0 then
            none
          else
            let count := ((shr64 tag 2) &&& 0x3fffffff).toNat
            let dataWords := ((shr64 tag 32) &&& 0xffff).toNat
            let ptrCount := ((shr64 tag 48) &&& 0xffff).toNat
          some { msg := r.msg, seg := r.targetSeg, startWord := r.targetWord + 1,
                   elemSize := elemSizeInlineComposite, elemCount := count,
                   structDataWords := dataWords, structPtrCount := ptrCount,
                   inlineComposite := true }
        else
          some { msg := r.msg, seg := r.targetSeg, startWord := r.targetWord,
                 elemSize := elemSize, elemCount := elemCount,
                 structDataWords := 0, structPtrCount := 0,
                 inlineComposite := false }

@[inline] def roundUpToWords (bytes : Nat) : Nat :=
  (bytes + 7) / 8

@[inline] def roundUpToBytes (bits : Nat) : Nat :=
  (bits + 7) / 8

@[inline] def listDataWords (elemSize : Nat) (elemCount : Nat) : Nat :=
  if elemSize == elemSizeVoid then
    0
  else if elemSize == elemSizeBit then
    roundUpToWords (roundUpToBytes elemCount)
  else if elemSize == elemSizeByte then
    roundUpToWords elemCount
  else if elemSize == elemSizeTwoBytes then
    roundUpToWords (elemCount * 2)
  else if elemSize == elemSizeFourBytes then
    roundUpToWords (elemCount * 4)
  else if elemSize == elemSizeEightBytes then
    elemCount
  else if elemSize == elemSizePointer then
    elemCount
  else
    0

@[inline] def decodeListPtrChecked (p : AnyPointer) : Except String (Option ListPointer) := do
  match (← resolvePointerChecked p) with
  | none => return none
  | some r =>
      let w ← readWordChecked r.msg r.tagSeg r.tagWord
      if (w &&& 0x3) != 1 then
        throw "list pointer kind mismatch"
      let elemSize := ((shr64 w 32) &&& 0x7).toNat
      let elemCount := ((shr64 w 35) &&& 0x1fffffff).toNat
      if elemSize == elemSizeInlineComposite then
        let listWordCount := elemCount
        if r.targetWord + 1 + listWordCount > segmentWordCount r.msg r.targetSeg then
          throw "inline composite list out of bounds"
        let tag ← readWordChecked r.msg r.targetSeg r.targetWord
        if (tag &&& 0x3) != 0 then
          throw "inline composite tag not struct"
        let count := ((shr64 tag 2) &&& 0x3fffffff).toNat
        let dataWords := ((shr64 tag 32) &&& 0xffff).toNat
        let ptrCount := ((shr64 tag 48) &&& 0xffff).toNat
        let elemWords := dataWords + ptrCount
        if count * elemWords > listWordCount then
          throw "inline composite overrun"
        return some { msg := r.msg, seg := r.targetSeg, startWord := r.targetWord + 1,
                      elemSize := elemSizeInlineComposite, elemCount := count,
                      structDataWords := dataWords, structPtrCount := ptrCount,
                      inlineComposite := true }
      else
        let words := listDataWords elemSize elemCount
        if r.targetWord + words <= segmentWordCount r.msg r.targetSeg then
          return some { msg := r.msg, seg := r.targetSeg, startWord := r.targetWord,
                        elemSize := elemSize, elemCount := elemCount,
                        structDataWords := 0, structPtrCount := 0,
                        inlineComposite := false }
        else
          throw "list out of bounds"

@[inline] def readListVoidReader (p : AnyPointer) : ListReader Unit :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeVoid then
        ListReader.empty
      else
        { size := d.elemCount, get := fun _ _ => () }
  | none => ListReader.empty

@[inline] def readListBoolReader (p : AnyPointer) : ListReader Bool :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeBit then
        ListReader.empty
      else
        let baseBit := d.startWord * 64
        { size := d.elemCount, get := fun i _ => getBit d.msg d.seg (baseBit + i) }
  | none => ListReader.empty

@[inline] def readListUInt8Reader (p : AnyPointer) : ListReader UInt8 :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeByte then
        ListReader.empty
      else
        let base := d.startWord * 8
        { size := d.elemCount, get := fun i _ => readByte d.msg d.seg (base + i) }
  | none => ListReader.empty

@[inline] def readListUInt16Reader (p : AnyPointer) : ListReader UInt16 :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeTwoBytes then
        ListReader.empty
      else
        let base := d.startWord * 8
        { size := d.elemCount, get := fun i _ => readUInt16 d.msg d.seg (base + i * 2) }
  | none => ListReader.empty

@[inline] def readListUInt32Reader (p : AnyPointer) : ListReader UInt32 :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeFourBytes then
        ListReader.empty
      else
        let base := d.startWord * 8
        { size := d.elemCount, get := fun i _ => readUInt32 d.msg d.seg (base + i * 4) }
  | none => ListReader.empty

@[inline] def readListUInt64Reader (p : AnyPointer) : ListReader UInt64 :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeEightBytes then
        ListReader.empty
      else
        let base := d.startWord * 8
        { size := d.elemCount, get := fun i _ => readUInt64 d.msg d.seg (base + i * 8) }
  | none => ListReader.empty

@[inline] def readListInt8Reader (p : AnyPointer) : ListReader Int8 :=
  ListReader.map (fun v => Int8.ofInt (signExtend 8 v.toUInt64))
    (readListUInt8Reader p)

@[inline] def readListInt16Reader (p : AnyPointer) : ListReader Int16 :=
  ListReader.map (fun v => Int16.ofInt (signExtend 16 v.toUInt64))
    (readListUInt16Reader p)

@[inline] def readListInt32Reader (p : AnyPointer) : ListReader Int32 :=
  ListReader.map (fun v => Int32.ofInt (signExtend 32 v.toUInt64))
    (readListUInt32Reader p)

@[inline] def readListInt64Reader (p : AnyPointer) : ListReader Int64 :=
  ListReader.map int64FromBits (readListUInt64Reader p)

@[inline] def readListFloat32Reader (p : AnyPointer) : ListReader Float :=
  ListReader.map float32FromBits (readListUInt32Reader p)

@[inline] def readListFloat64Reader (p : AnyPointer) : ListReader Float :=
  ListReader.map Float.ofBits (readListUInt64Reader p)

@[inline] def readListPointerReader (p : AnyPointer) : ListReader AnyPointer :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizePointer then
        ListReader.empty
      else
        { size := d.elemCount
        , get := fun i _ => { msg := d.msg, seg := d.seg, word := d.startWord + i }
        }
  | none => ListReader.empty

@[inline] def readListStructReader (p : AnyPointer) : ListReader StructReader :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite then
        let elemWords := d.structDataWords + d.structPtrCount
        { size := d.elemCount
        , get := fun i _ =>
            let base := if elemWords == 0 then d.startWord else d.startWord + i * elemWords
            { msg := d.msg, seg := d.seg, dataOff := base, dataWords := d.structDataWords,
              ptrOff := base + d.structDataWords, ptrCount := d.structPtrCount }
        }
      else
        ListReader.empty
  | none => ListReader.empty

@[inline] def readListVoid (p : AnyPointer) : Array Unit :=
  ListReader.toArray (readListVoidReader p)

@[inline] def readListBool (p : AnyPointer) : Array Bool :=
  ListReader.toArray (readListBoolReader p)

@[inline] def readListUInt8 (p : AnyPointer) : Array UInt8 :=
  ListReader.toArray (readListUInt8Reader p)

@[inline] def readListUInt16 (p : AnyPointer) : Array UInt16 :=
  ListReader.toArray (readListUInt16Reader p)

@[inline] def readListUInt32 (p : AnyPointer) : Array UInt32 :=
  ListReader.toArray (readListUInt32Reader p)

@[inline] def readListUInt64 (p : AnyPointer) : Array UInt64 :=
  ListReader.toArray (readListUInt64Reader p)

@[inline] def readListInt8 (p : AnyPointer) : Array Int8 :=
  ListReader.toArray (readListInt8Reader p)

@[inline] def readListInt16 (p : AnyPointer) : Array Int16 :=
  ListReader.toArray (readListInt16Reader p)

@[inline] def readListInt32 (p : AnyPointer) : Array Int32 :=
  ListReader.toArray (readListInt32Reader p)

@[inline] def readListInt64 (p : AnyPointer) : Array Int64 :=
  ListReader.toArray (readListInt64Reader p)

@[inline] def readListFloat32 (p : AnyPointer) : Array Float :=
  ListReader.toArray (readListFloat32Reader p)

@[inline] def readListFloat64 (p : AnyPointer) : Array Float :=
  ListReader.toArray (readListFloat64Reader p)

@[inline] def readListPointer (p : AnyPointer) : Array AnyPointer :=
  ListReader.toArray (readListPointerReader p)

@[inline] def readListStruct (p : AnyPointer) : Array StructReader :=
  ListReader.toArray (readListStructReader p)

@[inline] def readData (p : AnyPointer) : Data :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeByte then ByteArray.empty
      else readBytes d.msg d.seg (d.startWord * 8) d.elemCount
  | none => ByteArray.empty

@[inline] def readDataView (p : AnyPointer) : DataView :=
  match decodeListPtr p with
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeByte then
        emptySegmentView
      else
        let segv := getSegment d.msg d.seg
        let start := d.startWord * 8
        let len := d.elemCount
        if h : start + len ≤ segv.len then
          subSegmentView segv start len h
        else
          emptySegmentView
  | none => emptySegmentView

@[inline] def dataViewToByteArray (v : DataView) : ByteArray :=
  v.bytes.extract v.off (v.off + v.len)

@[inline] def readText (p : AnyPointer) : Text :=
  let bytes := readData p
  let len := bytes.size
  let trimmed := if len > 0 && bytes.get! (len - 1) == 0 then
    bytes.extract 0 (len - 1)
  else
    bytes
  match String.fromUTF8? trimmed with
  | some s => s
  | none => ""

@[inline] def readTextView (p : AnyPointer) : TextView :=
  let v := readDataView p
  if v.len == 0 then
    emptySegmentView
  else if readSegmentByte v (v.len - 1) != 0 then
    emptySegmentView
  else
    let len := v.len - 1
    subSegmentView v 0 len (by
      have h : len ≤ v.len := Nat.sub_le _ _
      simpa using h)

@[inline] def textViewToString (v : TextView) : Text :=
  match String.fromUTF8? (dataViewToByteArray v) with
  | some s => s
  | none => ""

@[inline] def readListVoidCheckedReader (p : AnyPointer) : Except String (ListReader Unit) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeVoid then
        throw "void list kind mismatch"
      return { size := d.elemCount, get := fun _ _ => () }

@[inline] def readListBoolCheckedReader (p : AnyPointer) : Except String (ListReader Bool) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeBit then
        throw "bool list kind mismatch"
      let baseBit := d.startWord * 64
      return { size := d.elemCount, get := fun i _ => getBit d.msg d.seg (baseBit + i) }

@[inline] def readListUInt8CheckedReader (p : AnyPointer) : Except String (ListReader UInt8) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeByte then
        throw "uint8 list kind mismatch"
      let base := d.startWord * 8
      return { size := d.elemCount, get := fun i _ => readByte d.msg d.seg (base + i) }

@[inline] def readListUInt16CheckedReader (p : AnyPointer) : Except String (ListReader UInt16) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeTwoBytes then
        throw "uint16 list kind mismatch"
      let base := d.startWord * 8
      return { size := d.elemCount, get := fun i _ => readUInt16 d.msg d.seg (base + i * 2) }

@[inline] def readListUInt32CheckedReader (p : AnyPointer) : Except String (ListReader UInt32) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeFourBytes then
        throw "uint32 list kind mismatch"
      let base := d.startWord * 8
      return { size := d.elemCount, get := fun i _ => readUInt32 d.msg d.seg (base + i * 4) }

@[inline] def readListUInt64CheckedReader (p : AnyPointer) : Except String (ListReader UInt64) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeEightBytes then
        throw "uint64 list kind mismatch"
      let base := d.startWord * 8
      return { size := d.elemCount, get := fun i _ => readUInt64 d.msg d.seg (base + i * 8) }

@[inline] def readListInt8CheckedReader (p : AnyPointer) : Except String (ListReader Int8) := do
  let r ← readListUInt8CheckedReader p
  return ListReader.map (fun v => Int8.ofInt (signExtend 8 v.toUInt64)) r

@[inline] def readListInt16CheckedReader (p : AnyPointer) : Except String (ListReader Int16) := do
  let r ← readListUInt16CheckedReader p
  return ListReader.map (fun v => Int16.ofInt (signExtend 16 v.toUInt64)) r

@[inline] def readListInt32CheckedReader (p : AnyPointer) : Except String (ListReader Int32) := do
  let r ← readListUInt32CheckedReader p
  return ListReader.map (fun v => Int32.ofInt (signExtend 32 v.toUInt64)) r

@[inline] def readListInt64CheckedReader (p : AnyPointer) : Except String (ListReader Int64) := do
  let r ← readListUInt64CheckedReader p
  return ListReader.map int64FromBits r

@[inline] def readListFloat32CheckedReader (p : AnyPointer) : Except String (ListReader Float) := do
  let r ← readListUInt32CheckedReader p
  return ListReader.map float32FromBits r

@[inline] def readListFloat64CheckedReader (p : AnyPointer) : Except String (ListReader Float) := do
  let r ← readListUInt64CheckedReader p
  return ListReader.map Float.ofBits r

@[inline] def readListPointerCheckedReader (p : AnyPointer) : Except String (ListReader AnyPointer) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizePointer then
        throw "pointer list kind mismatch"
      return { size := d.elemCount
             , get := fun i _ => { msg := d.msg, seg := d.seg, word := d.startWord + i } }

@[inline] def readListStructCheckedReader (p : AnyPointer) : Except String (ListReader StructReader) := do
  match (← decodeListPtrChecked p) with
  | none => return ListReader.empty
  | some d =>
      if d.inlineComposite then
        let elemWords := d.structDataWords + d.structPtrCount
        return { size := d.elemCount
               , get := fun i _ =>
                    let base := if elemWords == 0 then d.startWord else d.startWord + i * elemWords
                    { msg := d.msg, seg := d.seg, dataOff := base, dataWords := d.structDataWords,
                      ptrOff := base + d.structDataWords, ptrCount := d.structPtrCount } }
      else
        throw "struct list not inline composite"

@[inline] def readListVoidChecked (p : AnyPointer) : Except String (Array Unit) := do
  let r ← readListVoidCheckedReader p
  return ListReader.toArray r

@[inline] def readListBoolChecked (p : AnyPointer) : Except String (Array Bool) := do
  let r ← readListBoolCheckedReader p
  return ListReader.toArray r

@[inline] def readListUInt8Checked (p : AnyPointer) : Except String (Array UInt8) := do
  let r ← readListUInt8CheckedReader p
  return ListReader.toArray r

@[inline] def readListUInt16Checked (p : AnyPointer) : Except String (Array UInt16) := do
  let r ← readListUInt16CheckedReader p
  return ListReader.toArray r

@[inline] def readListUInt32Checked (p : AnyPointer) : Except String (Array UInt32) := do
  let r ← readListUInt32CheckedReader p
  return ListReader.toArray r

@[inline] def readListUInt64Checked (p : AnyPointer) : Except String (Array UInt64) := do
  let r ← readListUInt64CheckedReader p
  return ListReader.toArray r

@[inline] def readListInt8Checked (p : AnyPointer) : Except String (Array Int8) := do
  let r ← readListInt8CheckedReader p
  return ListReader.toArray r

@[inline] def readListInt16Checked (p : AnyPointer) : Except String (Array Int16) := do
  let r ← readListInt16CheckedReader p
  return ListReader.toArray r

@[inline] def readListInt32Checked (p : AnyPointer) : Except String (Array Int32) := do
  let r ← readListInt32CheckedReader p
  return ListReader.toArray r

@[inline] def readListInt64Checked (p : AnyPointer) : Except String (Array Int64) := do
  let r ← readListInt64CheckedReader p
  return ListReader.toArray r

@[inline] def readListFloat32Checked (p : AnyPointer) : Except String (Array Float) := do
  let r ← readListFloat32CheckedReader p
  return ListReader.toArray r

@[inline] def readListFloat64Checked (p : AnyPointer) : Except String (Array Float) := do
  let r ← readListFloat64CheckedReader p
  return ListReader.toArray r

@[inline] def readListPointerChecked (p : AnyPointer) : Except String (Array AnyPointer) := do
  let r ← readListPointerCheckedReader p
  return ListReader.toArray r

@[inline] def readListStructChecked (p : AnyPointer) : Except String (Array StructReader) := do
  let r ← readListStructCheckedReader p
  return ListReader.toArray r

@[inline] def readDataChecked (p : AnyPointer) : Except String Data := do
  match (← decodeListPtrChecked p) with
  | none => return ByteArray.empty
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeByte then
        throw "data pointer kind mismatch"
      return readBytes d.msg d.seg (d.startWord * 8) d.elemCount

@[inline] def readDataViewChecked (p : AnyPointer) : Except String DataView := do
  match (← decodeListPtrChecked p) with
  | none => return emptySegmentView
  | some d =>
      if d.inlineComposite || d.elemSize != elemSizeByte then
        throw "data pointer kind mismatch"
      let segv := getSegment d.msg d.seg
      let start := d.startWord * 8
      let len := d.elemCount
      if h : start + len ≤ segv.len then
        return subSegmentView segv start len h
      else
        throw "data out of bounds"

@[inline] def readTextChecked (p : AnyPointer) : Except String Text := do
  let bytes ← readDataChecked p
  let len := bytes.size
  if len == 0 then
    return ""
  if bytes.get! (len - 1) != 0 then
    throw "text missing NUL terminator"
  let trimmed := bytes.extract 0 (len - 1)
  match String.fromUTF8? trimmed with
  | some s => return s
  | none => throw "invalid utf8"

@[inline] def readTextViewChecked (p : AnyPointer) : Except String TextView := do
  let v ← readDataViewChecked p
  if v.len == 0 then
    return emptySegmentView
  if readSegmentByte v (v.len - 1) != 0 then
    throw "text missing NUL terminator"
  let len := v.len - 1
  return subSegmentView v 0 len (by
    have h : len ≤ v.len := Nat.sub_le _ _
    simpa using h)

@[inline] def readListTextReader (p : AnyPointer) : ListReader Text :=
  ListReader.map readText (readListPointerReader p)

@[inline] def readListDataReader (p : AnyPointer) : ListReader Data :=
  ListReader.map readData (readListPointerReader p)

@[inline] def readListText (p : AnyPointer) : Array Text :=
  ListReader.toArray (readListTextReader p)

@[inline] def readListData (p : AnyPointer) : Array Data :=
  ListReader.toArray (readListDataReader p)

@[inline] def readCapability (p : AnyPointer) : Capability :=
  let w := readWord p.msg p.seg p.word
  -- Canonical cap pointers have low 32 bits equal to 3 and index in high 32 bits.
  if (w &&& 0xffffffff) == 3 then
    (shr64 w 32).toUInt32
  else
    0

@[inline] def readCapabilityChecked (p : AnyPointer) : Except String Capability := do
  let w ← readWordChecked p.msg p.seg p.word
  if w == 0 then
    return 0
  if (w &&& 0xffffffff) == 3 then
    return (shr64 w 32).toUInt32
  throw "capability pointer kind mismatch"

@[inline] def readAnyPointerChecked (p : AnyPointer) : Except String AnyPointer := do
  let _ ← readWordChecked p.msg p.seg p.word
  return p

@[inline] def readListCapabilityReader (p : AnyPointer) : ListReader Capability :=
  ListReader.map readCapability (readListPointerReader p)

@[inline] def readListCapability (p : AnyPointer) : Array Capability :=
  ListReader.toArray (readListCapabilityReader p)

@[inline] def readCapabilityFromTable (t : CapTable) (p : AnyPointer) : Option Capability :=
  capTableGet t (readCapability p)

@[inline] def mkZeroBytes (n : Nat) : ByteArray :=
  Id.run do
    let mut ba := ByteArray.emptyWithCapacity n
    for _ in [0:n] do
      ba := ba.push 0
    return ba

@[inline] def initMessageBuilder (segmentWords : Nat) : MessageBuilder :=
  let words := Nat.max 1 segmentWords
  let seg := mkZeroBytes (words * 8)
  { segments := #[seg]
  , usedWords := #[1]
  , currentSeg := 0
  , nextWord := 1
  , defaultSegmentWords := words
  }

@[inline] def runBuilder (m : BuilderM α) (b : MessageBuilder) : α × MessageBuilder :=
  m.run b

@[inline] def evalBuilder (m : BuilderM α) (b : MessageBuilder) : α :=
  (m.run b).fst

@[inline] def execBuilder (m : BuilderM α) (b : MessageBuilder) : MessageBuilder :=
  (m.run b).snd

@[inline] def buildMessage (b : MessageBuilder) : Message :=
  let segs := b.segments.mapIdx (fun i ba =>
    let used := b.usedWords.getD i (ByteArray.size ba / 8)
    let len := Nat.min (used * 8) (ByteArray.size ba)
    have h : 0 + len ≤ ByteArray.size ba := by
      simpa [len] using (Nat.min_le_right (used * 8) (ByteArray.size ba))
    { bytes := ba, off := 0, len := len, h := h })
  { segments := segs }

@[inline] def getRootPointer : BuilderM AnyPointerBuilder :=
  pure { seg := 0, word := 0 }

@[inline] def segmentWordCapacity (seg : ByteArray) : Nat :=
  seg.size / 8

@[inline] def modifySegment (seg : Nat) (f : ByteArray → ByteArray) : BuilderM Unit := do
  let st ← get
  let ba := st.segments.getD seg ByteArray.empty
  let ba' := f ba
  let segs :=
    if h : seg < st.segments.size then
      st.segments.set seg ba' h
    else
      st.segments
  set { st with segments := segs }

@[inline] def allocWords (n : Nat) : BuilderM (Nat × Nat) := do
  let st ← get
  let seg := st.currentSeg
  let cap := segmentWordCapacity (st.segments.getD seg ByteArray.empty)
  if st.nextWord + n <= cap then
    let start := st.nextWord
    let next := st.nextWord + n
    let used := st.usedWords.getD seg 0
    let usedWords :=
      if h : seg < st.usedWords.size then
        st.usedWords.set seg (Nat.max used next) h
      else
        st.usedWords
    set { st with nextWord := next, usedWords := usedWords }
    return (seg, start)
  else
    let size := Nat.max st.defaultSegmentWords n
    let ba := mkZeroBytes (size * 8)
    let newSeg := st.segments.size
    let segs := st.segments.push ba
    let usedWords := st.usedWords.push n
    set { st with segments := segs, usedWords := usedWords, currentSeg := newSeg, nextWord := n }
    return (newSeg, 0)

@[inline] def getByteB (seg : Nat) (byteOff : Nat) : BuilderM UInt8 := do
  let st ← get
  let ba := st.segments.getD seg ByteArray.empty
  if byteOff < ba.size then
    return ba.get! byteOff
  else
    return 0

@[inline] def setByteU (ba : ByteArray) (byteOff : Nat) (b : UInt8) : ByteArray :=
  if h : byteOff < ba.size then
    let i := byteOff.toUSize
    have h' : i.toNat < ba.size := by
      have hmod : byteOff % (2 ^ System.Platform.numBits) < ba.size := by
        exact Nat.lt_of_le_of_lt (Nat.mod_le _ _) h
      simpa [USize.toNat_ofNat'] using hmod
    ba.uset i b h'
  else
    ba.set! byteOff b

@[inline] def writeByte (seg : Nat) (byteOff : Nat) (b : UInt8) : BuilderM Unit :=
  modifySegment seg (fun ba => setByteU ba byteOff b)

@[inline] def writeWordLE (seg : Nat) (wordOff : Nat) (w : UInt64) : BuilderM Unit :=
  modifySegment seg (fun ba =>
    Id.run do
      let base := wordOff * 8
      let mut out := ba
      for i in [0:8] do
        let b := (shr64 w (8 * i)).toUInt8
        out := setByteU out (base + i) b
      return out)

@[inline] def writeUInt16LE (seg : Nat) (byteOff : Nat) (v : UInt16) : BuilderM Unit :=
  modifySegment seg (fun ba =>
    Id.run do
      let mut out := ba
      out := setByteU out byteOff v.toUInt8
      out := setByteU out (byteOff + 1) ((v >>> 8).toUInt8)
      return out)

@[inline] def writeUInt32LE (seg : Nat) (byteOff : Nat) (v : UInt32) : BuilderM Unit :=
  modifySegment seg (fun ba =>
    Id.run do
      let mut out := ba
      out := setByteU out byteOff v.toUInt8
      out := setByteU out (byteOff + 1) ((v >>> 8).toUInt8)
      out := setByteU out (byteOff + 2) ((v >>> 16).toUInt8)
      out := setByteU out (byteOff + 3) ((v >>> 24).toUInt8)
      return out)

@[inline] def writeUInt64LE (seg : Nat) (byteOff : Nat) (v : UInt64) : BuilderM Unit :=
  modifySegment seg (fun ba =>
    Id.run do
      let mut out := ba
      for i in [0:8] do
        let b := (shr64 v (8 * i)).toUInt8
        out := setByteU out (byteOff + i) b
      return out)

@[inline] def setBitB (seg : Nat) (bitOff : Nat) (value : Bool) : BuilderM Unit := do
  let byteOff := bitOff / 8
  let bit := bitOff % 8
  let b ← getByteB seg byteOff
  let mask := (shl64 (1 : UInt64) bit).toUInt8
  let newb := if value then (b ||| mask) else (b &&& (~~~mask))
  writeByte seg byteOff newb

@[inline] def dataByteOffB (r : StructBuilder) (byteOff : Nat) : Nat :=
  r.dataOff * 8 + byteOff

@[inline] def setBool (r : StructBuilder) (bitOff : Nat) (v : Bool) : BuilderM Unit :=
  setBitB r.seg (r.dataOff * 64 + bitOff) v

@[inline] def setBoolMasked (r : StructBuilder) (bitOff : Nat) (mask : Bool) (v : Bool) : BuilderM Unit :=
  setBool r bitOff (if mask then !v else v)

@[inline] def setUInt8 (r : StructBuilder) (byteOff : Nat) (v : UInt8) : BuilderM Unit :=
  writeByte r.seg (dataByteOffB r byteOff) v

@[inline] def setUInt8Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt8) (v : UInt8) : BuilderM Unit :=
  setUInt8 r byteOff (v ^^^ mask)

@[inline] def setUInt16 (r : StructBuilder) (byteOff : Nat) (v : UInt16) : BuilderM Unit :=
  writeUInt16LE r.seg (dataByteOffB r byteOff) v

@[inline] def setUInt16Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt16) (v : UInt16) : BuilderM Unit :=
  setUInt16 r byteOff (v ^^^ mask)

@[inline] def setUInt32 (r : StructBuilder) (byteOff : Nat) (v : UInt32) : BuilderM Unit :=
  writeUInt32LE r.seg (dataByteOffB r byteOff) v

@[inline] def setUInt32Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt32) (v : UInt32) : BuilderM Unit :=
  setUInt32 r byteOff (v ^^^ mask)

@[inline] def setUInt64 (r : StructBuilder) (byteOff : Nat) (v : UInt64) : BuilderM Unit :=
  writeUInt64LE r.seg (dataByteOffB r byteOff) v

@[inline] def setUInt64Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt64) (v : UInt64) : BuilderM Unit :=
  setUInt64 r byteOff (v ^^^ mask)

@[inline] def setInt8 (r : StructBuilder) (byteOff : Nat) (v : Int8) : BuilderM Unit :=
  setUInt8 r byteOff v.toUInt8

@[inline] def setInt8Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt8) (v : Int8) : BuilderM Unit :=
  setUInt8 r byteOff (v.toUInt8 ^^^ mask)

@[inline] def setInt16 (r : StructBuilder) (byteOff : Nat) (v : Int16) : BuilderM Unit :=
  setUInt16 r byteOff v.toUInt16

@[inline] def setInt16Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt16) (v : Int16) : BuilderM Unit :=
  setUInt16 r byteOff (v.toUInt16 ^^^ mask)

@[inline] def setInt32 (r : StructBuilder) (byteOff : Nat) (v : Int32) : BuilderM Unit :=
  setUInt32 r byteOff v.toUInt32

@[inline] def setInt32Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt32) (v : Int32) : BuilderM Unit :=
  setUInt32 r byteOff (v.toUInt32 ^^^ mask)

@[inline] def setInt64 (r : StructBuilder) (byteOff : Nat) (v : Int64) : BuilderM Unit :=
  setUInt64 r byteOff v.toUInt64

@[inline] def setInt64Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt64) (v : Int64) : BuilderM Unit :=
  setUInt64 r byteOff (v.toUInt64 ^^^ mask)

@[inline] def setFloat32 (r : StructBuilder) (byteOff : Nat) (v : Float) : BuilderM Unit :=
  let bits := Float32.toBits (Float.toFloat32 v)
  setUInt32 r byteOff bits

@[inline] def setFloat32Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt32) (v : Float) : BuilderM Unit :=
  let bits := Float32.toBits (Float.toFloat32 v)
  setUInt32 r byteOff (bits ^^^ mask)

@[inline] def setFloat64 (r : StructBuilder) (byteOff : Nat) (v : Float) : BuilderM Unit :=
  setUInt64 r byteOff (Float.toBits v)

@[inline] def setFloat64Masked (r : StructBuilder) (byteOff : Nat) (mask : UInt64) (v : Float) : BuilderM Unit :=
  setUInt64 r byteOff ((Float.toBits v) ^^^ mask)

@[inline] def getPointerBuilder (r : StructBuilder) (index : Nat) : AnyPointerBuilder :=
  { seg := r.seg, word := r.ptrOff + index }

@[inline] def intToUInt (bits : Nat) (x : Int) : UInt64 :=
  if x >= 0 then
    UInt64.ofNat x.toNat
  else
    let pow := Nat.pow 2 bits
    UInt64.ofNat (pow - (-x).toNat)

@[inline] def encodeStructPointer (offset : Int) (dataWords : Nat) (ptrCount : Nat) : UInt64 :=
  let off := intToUInt 30 offset
  (off <<< 2) ||| (UInt64.ofNat dataWords <<< 32) ||| (UInt64.ofNat ptrCount <<< 48)

@[inline] def encodeStructTag (elemCount : Nat) (dataWords : Nat) (ptrCount : Nat) : UInt64 :=
  (UInt64.ofNat elemCount <<< 2) ||| (UInt64.ofNat dataWords <<< 32) ||| (UInt64.ofNat ptrCount <<< 48)

@[inline] def encodeListPointer (offset : Int) (elemSize : Nat) (elemCount : Nat) : UInt64 :=
  let off := intToUInt 30 offset
  (1 : UInt64) ||| (off <<< 2) ||| (UInt64.ofNat elemSize <<< 32) ||| (UInt64.ofNat elemCount <<< 35)

@[inline] def encodeFarPointer (isDouble : Bool) (segId : Nat) (offset : Nat) : UInt64 :=
  let dbl := if isDouble then 1 else 0
  (2 : UInt64) ||| (UInt64.ofNat dbl <<< 2) ||| (UInt64.ofNat offset <<< 3) |||
    (UInt64.ofNat segId <<< 32)

@[inline] def initStructPointer (p : AnyPointerBuilder) (dataWords : Nat) (ptrCount : Nat) : BuilderM StructBuilder := do
  let total := dataWords + ptrCount
  let st ← get
  let cap := segmentWordCapacity (st.segments.getD st.currentSeg ByteArray.empty)
  if p.seg == st.currentSeg && st.nextWord + total <= cap then
    let (seg, start) ← allocWords total
    let off : Int := (Int.ofNat start) - (Int.ofNat (p.word + 1))
    writeWordLE p.seg p.word (encodeStructPointer off dataWords ptrCount)
    return { seg := seg, dataOff := start, dataWords := dataWords,
             ptrOff := start + dataWords, ptrCount := ptrCount }
  else
    let (seg, padStart) ← allocWords (total + 2)
    let objStart := padStart + 2
    writeWordLE p.seg p.word (encodeFarPointer true seg padStart)
    writeWordLE seg padStart (encodeFarPointer false seg objStart)
    writeWordLE seg (padStart + 1) (encodeStructPointer 0 dataWords ptrCount)
    return { seg := seg, dataOff := objStart, dataWords := dataWords,
             ptrOff := objStart + dataWords, ptrCount := ptrCount }

@[inline] def initListPointer (p : AnyPointerBuilder) (elemSize : Nat) (elemCount : Nat) : BuilderM ListBuilder := do
  let words :=
    if elemSize == elemSizeVoid then
      0
    else if elemSize == elemSizeBit then
      roundUpToWords (roundUpToBytes elemCount)
    else if elemSize == elemSizeByte then
      roundUpToWords elemCount
    else if elemSize == elemSizeTwoBytes then
      roundUpToWords (elemCount * 2)
    else if elemSize == elemSizeFourBytes then
      roundUpToWords (elemCount * 4)
    else
      elemCount
  let st ← get
  let cap := segmentWordCapacity (st.segments.getD st.currentSeg ByteArray.empty)
  if p.seg == st.currentSeg && st.nextWord + words <= cap then
    let (seg, start) ← allocWords words
    let off : Int := (Int.ofNat start) - (Int.ofNat (p.word + 1))
    writeWordLE p.seg p.word (encodeListPointer off elemSize elemCount)
    return { seg := seg, startWord := start, elemSize := elemSize, elemCount := elemCount,
             structDataWords := 0, structPtrCount := 0, inlineComposite := false }
  else
    let (seg, padStart) ← allocWords (words + 2)
    let objStart := padStart + 2
    writeWordLE p.seg p.word (encodeFarPointer true seg padStart)
    writeWordLE seg padStart (encodeFarPointer false seg objStart)
    writeWordLE seg (padStart + 1) (encodeListPointer 0 elemSize elemCount)
    return { seg := seg, startWord := objStart, elemSize := elemSize, elemCount := elemCount,
             structDataWords := 0, structPtrCount := 0, inlineComposite := false }

@[inline] def initStructListPointer (p : AnyPointerBuilder)
    (dataWords : Nat) (ptrCount : Nat) (elemCount : Nat) : BuilderM ListBuilder := do
  let elemWords := dataWords + ptrCount
  let listWordCount := 1 + elemCount * elemWords
  let st ← get
  let cap := segmentWordCapacity (st.segments.getD st.currentSeg ByteArray.empty)
  if p.seg == st.currentSeg && st.nextWord + listWordCount <= cap then
    let (seg, start) ← allocWords listWordCount
    let off : Int := (Int.ofNat start) - (Int.ofNat (p.word + 1))
    writeWordLE p.seg p.word (encodeListPointer off elemSizeInlineComposite listWordCount)
    writeWordLE seg start (encodeStructTag elemCount dataWords ptrCount)
    return { seg := seg, startWord := start + 1, elemSize := elemSizeInlineComposite,
             elemCount := elemCount, structDataWords := dataWords,
             structPtrCount := ptrCount, inlineComposite := true }
  else
    let (seg, padStart) ← allocWords (listWordCount + 2)
    let objStart := padStart + 2
    writeWordLE p.seg p.word (encodeFarPointer true seg padStart)
    writeWordLE seg padStart (encodeFarPointer false seg objStart)
    writeWordLE seg (padStart + 1) (encodeListPointer 0 elemSizeInlineComposite listWordCount)
    writeWordLE seg objStart (encodeStructTag elemCount dataWords ptrCount)
    return { seg := seg, startWord := objStart + 1, elemSize := elemSizeInlineComposite,
             elemCount := elemCount, structDataWords := dataWords,
             structPtrCount := ptrCount, inlineComposite := true }

@[inline] def listStructBuilders (l : ListBuilder) : Array StructBuilder :=
  if l.inlineComposite then
    let elemWords := l.structDataWords + l.structPtrCount
    mkArray l.elemCount (fun i =>
      let base := if elemWords == 0 then l.startWord else l.startWord + i * elemWords
      { seg := l.seg, dataOff := base, dataWords := l.structDataWords,
        ptrOff := base + l.structDataWords, ptrCount := l.structPtrCount })
  else
    #[]

@[inline] def listPointerBuilders (l : ListBuilder) : Array AnyPointerBuilder :=
  if l.inlineComposite || l.elemSize != elemSizePointer then
    #[]
  else
    mkArray l.elemCount (fun i => { seg := l.seg, word := l.startWord + i })

@[inline] def listSetBool (l : ListBuilder) (i : Nat) (v : Bool) : BuilderM Unit :=
  if i < l.elemCount && l.elemSize == elemSizeBit then
    let bit := l.startWord * 64 + i
    setBitB l.seg bit v
  else
    pure ()

@[inline] def listSetUInt8 (l : ListBuilder) (i : Nat) (v : UInt8) : BuilderM Unit :=
  if i < l.elemCount && l.elemSize == elemSizeByte then
    writeByte l.seg (l.startWord * 8 + i) v
  else
    pure ()

@[inline] def listSetUInt16 (l : ListBuilder) (i : Nat) (v : UInt16) : BuilderM Unit :=
  if i < l.elemCount && l.elemSize == elemSizeTwoBytes then
    writeUInt16LE l.seg (l.startWord * 8 + i * 2) v
  else
    pure ()

@[inline] def listSetUInt32 (l : ListBuilder) (i : Nat) (v : UInt32) : BuilderM Unit :=
  if i < l.elemCount && l.elemSize == elemSizeFourBytes then
    writeUInt32LE l.seg (l.startWord * 8 + i * 4) v
  else
    pure ()

@[inline] def listSetUInt64 (l : ListBuilder) (i : Nat) (v : UInt64) : BuilderM Unit :=
  if i < l.elemCount && l.elemSize == elemSizeEightBytes then
    writeUInt64LE l.seg (l.startWord * 8 + i * 8) v
  else
    pure ()

@[inline] def listSetInt8 (l : ListBuilder) (i : Nat) (v : Int8) : BuilderM Unit :=
  listSetUInt8 l i v.toUInt8

@[inline] def listSetInt16 (l : ListBuilder) (i : Nat) (v : Int16) : BuilderM Unit :=
  listSetUInt16 l i v.toUInt16

@[inline] def listSetInt32 (l : ListBuilder) (i : Nat) (v : Int32) : BuilderM Unit :=
  listSetUInt32 l i v.toUInt32

@[inline] def listSetInt64 (l : ListBuilder) (i : Nat) (v : Int64) : BuilderM Unit :=
  listSetUInt64 l i v.toUInt64

@[inline] def listSetFloat32 (l : ListBuilder) (i : Nat) (v : Float) : BuilderM Unit :=
  listSetUInt32 l i (Float32.toBits (Float.toFloat32 v))

@[inline] def listSetFloat64 (l : ListBuilder) (i : Nat) (v : Float) : BuilderM Unit :=
  listSetUInt64 l i (Float.toBits v)

@[inline] def writeBytesAt (seg : Nat) (byteOff : Nat) (src : ByteArray) : BuilderM Unit :=
  modifySegment seg (fun ba =>
    Id.run do
      let mut out := ba
      for i in [0:src.size] do
        out := setByteU out (byteOff + i) (src.get! i)
      return out)

@[inline] def writeListUInt8 (p : AnyPointerBuilder) (vals : Array UInt8) : BuilderM Unit := do
  let lb ← initListPointer p elemSizeByte vals.size
  modifySegment lb.seg (fun ba =>
    Id.run do
      let mut out := ba
      let base := lb.startWord * 8
      for i in [0:vals.size] do
        out := setByteU out (base + i) (vals.getD i 0)
      return out)

@[inline] def writeListUInt16 (p : AnyPointerBuilder) (vals : Array UInt16) : BuilderM Unit := do
  let lb ← initListPointer p elemSizeTwoBytes vals.size
  modifySegment lb.seg (fun ba =>
    Id.run do
      let mut out := ba
      let base := lb.startWord * 8
      for i in [0:vals.size] do
        let v := vals.getD i 0
        let off := base + i * 2
        out := setByteU out off v.toUInt8
        out := setByteU out (off + 1) ((v >>> 8).toUInt8)
      return out)

@[inline] def writeListUInt32 (p : AnyPointerBuilder) (vals : Array UInt32) : BuilderM Unit := do
  let lb ← initListPointer p elemSizeFourBytes vals.size
  modifySegment lb.seg (fun ba =>
    Id.run do
      let mut out := ba
      let base := lb.startWord * 8
      for i in [0:vals.size] do
        let v := vals.getD i 0
        let off := base + i * 4
        out := setByteU out off v.toUInt8
        out := setByteU out (off + 1) ((v >>> 8).toUInt8)
        out := setByteU out (off + 2) ((v >>> 16).toUInt8)
        out := setByteU out (off + 3) ((v >>> 24).toUInt8)
      return out)

@[inline] def writeListUInt64 (p : AnyPointerBuilder) (vals : Array UInt64) : BuilderM Unit := do
  let lb ← initListPointer p elemSizeEightBytes vals.size
  modifySegment lb.seg (fun ba =>
    Id.run do
      let mut out := ba
      let base := lb.startWord * 8
      for i in [0:vals.size] do
        let v := vals.getD i 0
        let off := base + i * 8
        for j in [0:8] do
          let b := (shr64 v (8 * j)).toUInt8
          out := setByteU out (off + j) b
      return out)

@[inline] def writeListVoid (p : AnyPointerBuilder) (vals : Array Unit) : BuilderM Unit := do
  let _ ← initListPointer p elemSizeVoid vals.size
  pure ()

@[inline] def writeListInt8 (p : AnyPointerBuilder) (vals : Array Int8) : BuilderM Unit :=
  writeListUInt8 p (Array.map (fun v => v.toUInt8) vals)

@[inline] def writeListInt16 (p : AnyPointerBuilder) (vals : Array Int16) : BuilderM Unit :=
  writeListUInt16 p (Array.map (fun v => v.toUInt16) vals)

@[inline] def writeListInt32 (p : AnyPointerBuilder) (vals : Array Int32) : BuilderM Unit :=
  writeListUInt32 p (Array.map (fun v => v.toUInt32) vals)

@[inline] def writeListInt64 (p : AnyPointerBuilder) (vals : Array Int64) : BuilderM Unit :=
  writeListUInt64 p (Array.map (fun v => v.toUInt64) vals)

@[inline] def writeListBool (p : AnyPointerBuilder) (vals : Array Bool) : BuilderM Unit := do
  let lb ← initListPointer p elemSizeBit vals.size
  let baseBit := lb.startWord * 64
  for i in [0:vals.size] do
    setBitB lb.seg (baseBit + i) (vals.getD i false)

@[inline] def writeListFloat32 (p : AnyPointerBuilder) (vals : Array Float) : BuilderM Unit :=
  writeListUInt32 p (Array.map (fun v => Float32.toBits (Float.toFloat32 v)) vals)

@[inline] def writeListFloat64 (p : AnyPointerBuilder) (vals : Array Float) : BuilderM Unit :=
  writeListUInt64 p (Array.map (fun v => Float.toBits v) vals)

@[inline] def writeData (p : AnyPointerBuilder) (data : Data) : BuilderM Unit := do
  let lb ← initListPointer p elemSizeByte data.size
  writeBytesAt lb.seg (lb.startWord * 8) data

@[inline] def writeText (p : AnyPointerBuilder) (text : Text) : BuilderM Unit :=
  writeData p (text.toUTF8.push 0)

@[inline] def writeListText (p : AnyPointerBuilder) (vals : Array Text) : BuilderM Unit := do
  let lb ← initListPointer p elemSizePointer vals.size
  let ptrs := listPointerBuilders lb
  for i in [0:vals.size] do
    let ptr := ptrs.getD i { seg := 0, word := 0 }
    writeText ptr (vals.getD i "")

@[inline] def writeListData (p : AnyPointerBuilder) (vals : Array Data) : BuilderM Unit := do
  let lb ← initListPointer p elemSizePointer vals.size
  let ptrs := listPointerBuilders lb
  for i in [0:vals.size] do
    let ptr := ptrs.getD i { seg := 0, word := 0 }
    writeData ptr (vals.getD i ByteArray.empty)

@[inline] def writeListPointer (p : AnyPointerBuilder) (count : Nat) : BuilderM (Array AnyPointerBuilder) := do
  let lb ← initListPointer p elemSizePointer count
  return listPointerBuilders lb

@[inline] def clearPointer (p : AnyPointerBuilder) : BuilderM Unit :=
  writeWordLE p.seg p.word 0

@[inline] def encodeCapabilityPointer (index : Capability) : UInt64 :=
  -- Cap'n Proto wire format: low 32 bits are exactly 3, high 32 bits store cap index.
  (3 : UInt64) ||| (index.toUInt64 <<< 32)

@[inline] def writeCapability (p : AnyPointerBuilder) (cap : Capability) : BuilderM Unit :=
  writeWordLE p.seg p.word (encodeCapabilityPointer cap)

mutual
  partial def copyAnyPointer (dst : AnyPointerBuilder) (src : AnyPointer) : BuilderM Unit := do
    let w := readWord src.msg src.seg src.word
    if w == 0 then
      clearPointer dst
    else if (w &&& 0x3) == 3 then
      writeCapability dst (readCapability src)
    else
      match decodeStructPtr src with
      | some sr =>
          let sb ← initStructPointer dst sr.dataWords sr.ptrCount
          copyStruct sb sr
      | none =>
          match decodeListPtr src with
          | some lp => copyList dst lp
          | none => clearPointer dst

  partial def copyStruct (dst : StructBuilder) (src : StructReader) : BuilderM Unit := do
    for i in [0:src.dataWords] do
      let w := readWord src.msg src.seg (src.dataOff + i)
      writeWordLE dst.seg (dst.dataOff + i) w
    for i in [0:src.ptrCount] do
      let sp : AnyPointer := { msg := src.msg, seg := src.seg, word := src.ptrOff + i }
      let dp : AnyPointerBuilder := { seg := dst.seg, word := dst.ptrOff + i }
      copyAnyPointer dp sp

  partial def copyList (dst : AnyPointerBuilder) (src : ListPointer) : BuilderM Unit := do
    if src.inlineComposite then
      let lb ← initStructListPointer dst src.structDataWords src.structPtrCount src.elemCount
      let builders := listStructBuilders lb
      let elemWords := src.structDataWords + src.structPtrCount
      let emptySb : StructBuilder :=
        { seg := 0, dataOff := 0, dataWords := 0, ptrOff := 0, ptrCount := 0 }
      for i in [0:src.elemCount] do
        let base := if elemWords == 0 then src.startWord else src.startWord + i * elemWords
        let sr : StructReader :=
          { msg := src.msg, seg := src.seg, dataOff := base, dataWords := src.structDataWords,
            ptrOff := base + src.structDataWords, ptrCount := src.structPtrCount }
        let sb := builders.getD i emptySb
        copyStruct sb sr
    else if src.elemSize == elemSizePointer then
      let ptrs ← writeListPointer dst src.elemCount
      for i in [0:src.elemCount] do
        let sp : AnyPointer := { msg := src.msg, seg := src.seg, word := src.startWord + i }
        let dp := ptrs.getD i { seg := 0, word := 0 }
        copyAnyPointer dp sp
    else
      let lb ← initListPointer dst src.elemSize src.elemCount
      let words := listDataWords src.elemSize src.elemCount
      if words > 0 then
        let bytes := readBytes src.msg src.seg (src.startWord * 8) (words * 8)
        writeBytesAt lb.seg (lb.startWord * 8) bytes
      else
        pure ()
end

@[inline] def writeCapabilityWithTable (t : CapTable) (p : AnyPointerBuilder) (cap : Capability) :
    BuilderM CapTable := do
  let (idx, t') := capTableAdd t cap
  writeCapability p idx
  return t'

@[inline] def writeListCapability (p : AnyPointerBuilder) (vals : Array Capability) : BuilderM Unit := do
  let lb ← initListPointer p elemSizePointer vals.size
  let ptrs := listPointerBuilders lb
  for i in [0:vals.size] do
    let ptr := ptrs.getD i { seg := 0, word := 0 }
    writeCapability ptr (vals.getD i 0)

end Capnp
