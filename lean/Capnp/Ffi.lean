import Init

namespace Capnp

structure FfiHandle where
  handle : UInt32
  deriving Inhabited, BEq, Repr

structure FfiRuntimeHandle (RuntimeT : Type) where
  runtime : RuntimeT
  handle : UInt32
  deriving Inhabited, BEq, Repr

end Capnp
