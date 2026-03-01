import Capnp.Rpc
import LeanTest

open LeanTest

@[extern "capnp_lean_rpc_test_verify_rpc_diagnostics_layout"]
opaque verifyRpcDiagnosticsLayout (d : Capnp.Rpc.RpcDiagnostics) : IO Bool

@[extern "capnp_lean_rpc_test_verify_remote_exception_layout"]
opaque verifyRemoteExceptionLayout (e : Capnp.Rpc.RemoteException) : IO Bool

@[test]
def testRpcDiagnosticsLayout : IO Unit := do
  let d : Capnp.Rpc.RpcDiagnostics := {
    questionCount := 0x0101010101010101
    answerCount   := 0x0202020202020202
    exportCount   := 0x0303030303030303
    importCount   := 0x0404040404040404
    embargoCount  := 0x0505050505050505
    isIdle        := true
  }
  let ok ← verifyRpcDiagnosticsLayout d
  assertTrue ok "RpcDiagnostics C++ layout mismatch"

@[test]
def testRemoteExceptionLayout : IO Unit := do
  let e : Capnp.Rpc.RemoteException := {
    description := "description"
    remoteTrace := "trace"
    detail := ByteArray.mk #[1, 2, 3]
    fileName := "file.cpp"
    lineNumber := 123
    typeTag := Capnp.Rpc.RemoteExceptionType.toUInt8 .disconnected
  }
  let ok ← verifyRemoteExceptionLayout e
  assertTrue ok "RemoteException C++ layout mismatch"
