import Capnp.Rpc

@[extern "capnp_lean_rpc_test_new_socketpair"]
opaque ffiNewSocketPairImpl : IO (UInt32 × UInt32)

@[extern "capnp_lean_rpc_test_close_fd"]
opaque ffiCloseFdImpl (fd : UInt32) : IO Unit

@[extern "capnp_lean_rpc_test_new_listen_socket_fd"]
opaque ffiNewListenSocketFdImpl : IO (UInt32 × UInt32)

@[extern "capnp_lean_rpc_test_new_datagram_socket_fd"]
opaque ffiNewDatagramSocketFdImpl : IO (UInt32 × UInt32)

def mkCapabilityPayload (cap : Capnp.Capability) : Capnp.Rpc.Payload := Id.run do
  let (capTable, builder) :=
    (do
      let root ← Capnp.getRootPointer
      Capnp.writeCapabilityWithTable Capnp.emptyCapTable root cap
    ).run (Capnp.initMessageBuilder 16)
  { msg := Capnp.buildMessage builder, capTable := capTable }

def mkNullPayload : Capnp.Rpc.Payload := Id.run do
  let (_, builder) :=
    (do
      let root ← Capnp.getRootPointer
      Capnp.clearPointer root
    ).run (Capnp.initMessageBuilder 16)
  { msg := Capnp.buildMessage builder, capTable := Capnp.emptyCapTable }

def mkUInt64Payload (n : UInt64) : Capnp.Rpc.Payload := Id.run do
  let (_, builder) :=
    (do
      let root ← Capnp.getRootPointer
      let s ← Capnp.initStructPointer root 1 0
      Capnp.setUInt64 s 0 n
    ).run (Capnp.initMessageBuilder 16)
  { msg := Capnp.buildMessage builder, capTable := Capnp.emptyCapTable }

def readUInt64Payload (payload : Capnp.Rpc.Payload) : IO UInt64 := do
  let root := Capnp.getRoot payload.msg
  match Capnp.readStructChecked root with
  | .ok s =>
      pure (Capnp.getUInt64 s 0)
  | .error err =>
      throw (IO.userError s!"invalid uint64 RPC payload: {err}")

def mkLargePayload (size : Nat) : Capnp.Rpc.Payload := Id.run do
  let (_, builder) :=
    (do
      let root ← Capnp.getRootPointer
      Capnp.writeData root (ByteArray.mk (Array.ofFn (n := size) (fun _ => 0)))
    ).run (Capnp.initMessageBuilder (size / 8 + 8))
  { msg := Capnp.buildMessage builder, capTable := Capnp.emptyCapTable }

def mkUnixTestAddress : IO (String × String) := do
  let n ← IO.rand 0 1000000000
  let path := s!"/tmp/capnp-lean4-rpc-{n}.sock"
  pure (s!"unix:{path}", path)
