@[extern "my_ffi2"]
opaque myFfi2 (a : UInt32) (b : UInt64) (c : UInt16) (bytes : @& ByteArray) : IO ByteArray

def t2 : IO ByteArray := myFfi2 1 2 3 (ByteArray.mk #[1,2,3])
