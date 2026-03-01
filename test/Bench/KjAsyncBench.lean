import Capnp.KjAsync

private partial def waitForHttpServerRequestRaw (runtime : Capnp.KjAsync.Runtime)
    (server : Capnp.KjAsync.HttpServer) (attempts : Nat := 2000) :
    IO Capnp.KjAsync.HttpServerRequest := do
  if attempts == 0 then
    throw (IO.userError "timed out waiting for HTTP server request in benchmark")
  match (← runtime.httpServerPollRequestStreaming? server) with
  | some request => pure request
  | none =>
    runtime.pump
    waitForHttpServerRequestRaw runtime server (attempts - 1)

private partial def readHttpServerRequestBodyAll
    (requestBody : Capnp.KjAsync.HttpServerRequestBody) : IO ByteArray := do
  let mut out := ByteArray.empty
  let mut done := false
  while !done do
    let chunk ← requestBody.read (UInt32.ofNat 1) (UInt32.ofNat 16384)
    if chunk.size == 0 then
      done := true
    else
      out := ByteArray.append out chunk
  requestBody.release
  pure out

private def mkPayload (size : Nat) : ByteArray :=
  Id.run do
    let mut out := ByteArray.empty
    for i in [:size] do
      out := out.push (UInt8.ofNat (i % 251))
    pure out

private def parseNatArg0 (args : List String) (default : Nat) : Nat :=
  match args with
  | value :: _ => value.toNat?.getD default
  | [] => default

private def parseNatArg1 (args : List String) (default : Nat) : Nat :=
  match args with
  | _ :: value :: _ => value.toNat?.getD default
  | _ => default

private def runByteArrayRoundtripBench (runtime : Capnp.KjAsync.Runtime)
    (server : Capnp.KjAsync.HttpServer) (runs : Nat) (payload : ByteArray) : IO Nat := do
  let start ← IO.monoNanosNow
  for _ in [:runs] do
    let responsePromise ←
      runtime.httpRequestStart .post "127.0.0.1" "/bench-bytearray" payload server.boundPort
    let request ← waitForHttpServerRequestRaw runtime server
    let body ←
      match request.bodyStream? with
      | some requestBody => readHttpServerRequestBodyAll requestBody
      | none => pure request.body
    if body != payload then
      throw (IO.userError "benchmark byte-array request body mismatch")
    runtime.httpServerRespond server request.requestId (UInt32.ofNat 200) "OK" #[] payload
    runtime.pump
    let response ← responsePromise.await
    if response.body != payload then
      throw (IO.userError "benchmark byte-array response body mismatch")
  let stop ← IO.monoNanosNow
  pure (stop - start)

private def runBytesRefRoundtripBench (runtime : Capnp.KjAsync.Runtime)
    (server : Capnp.KjAsync.HttpServer) (runs : Nat) (payload : ByteArray) : IO Nat := do
  let payloadRef ← Capnp.KjAsync.BytesRef.ofByteArray payload
  let start ← IO.monoNanosNow
  for _ in [:runs] do
    let responsePromise ←
      runtime.httpRequestStartRef .post "127.0.0.1" "/bench-bytesref" payloadRef server.boundPort
    let request ← waitForHttpServerRequestRaw runtime server
    let body ←
      match request.bodyStream? with
      | some requestBody => readHttpServerRequestBodyAll requestBody
      | none => pure request.body
    if body != payload then
      throw (IO.userError "benchmark bytes-ref request body mismatch")
    runtime.httpServerRespondRef server request.requestId (UInt32.ofNat 200) "OK" #[] payloadRef
    runtime.pump
    let response ← responsePromise.awaitRef
    let responseBody ← Capnp.KjAsync.BytesRef.toByteArray response.body
    if responseBody != payload then
      throw (IO.userError "benchmark bytes-ref response body mismatch")
  let stop ← IO.monoNanosNow
  pure (stop - start)

def main (args : List String) : IO UInt32 := do
  let runs := max 1 (parseNatArg0 args 200)
  let payloadSize := max 1 (parseNatArg1 args 1024)
  let payload := mkPayload payloadSize

  let runtime ← Capnp.KjAsync.Runtime.init
  try
    let server ← runtime.httpServerListen "127.0.0.1" 0
    try
      let byteArrayTotalNs ← runByteArrayRoundtripBench runtime server runs payload
      let bytesRefTotalNs ← runBytesRefRoundtripBench runtime server runs payload
      let byteArrayAvgNs := byteArrayTotalNs / runs
      let bytesRefAvgNs := bytesRefTotalNs / runs

      IO.println s!"KjAsync HTTP roundtrip benchmark"
      IO.println s!"runs={runs} payload_bytes={payloadSize}"
      IO.println s!"bytearray total_ns={byteArrayTotalNs} avg_ns={byteArrayAvgNs}"
      IO.println s!"bytesref total_ns={bytesRefTotalNs} avg_ns={bytesRefAvgNs}"
      pure 0
    finally
      server.release
  finally
    runtime.shutdown
