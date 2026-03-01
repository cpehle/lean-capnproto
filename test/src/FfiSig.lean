@[extern "my_ffi"]
opaque myFfi (a : UInt32) (b : UInt16) (bytes : @& ByteArray) : IO ByteArray

def t : IO ByteArray := myFfi 1 2 (ByteArray.mk #[1,2,3])
