# Lean4 Gateway Design (OpenClaw-Compatible, Cap'n Proto-First)

## Goal

Build a Lean4 gateway that:

- preserves OpenClaw-style operational behavior (handshake, request/response/event flow, session semantics),
- uses Cap'n Proto as the internal contract and primary long-term client protocol,
- keeps one runtime model across HTTP/WebSocket and RPC (`Capnp.KjAsync` + `Capnp.Rpc`).

This is an architecture design, not a full implementation spec.

## Existing Capabilities We Can Reuse

From this repo today:

- `Capnp.KjAsync` already exposes HTTP server + WebSocket server/client primitives.
- `Capnp.Rpc` already exposes two-party + multi-vat runtime/server/client APIs.
- `Capnp.RpcKjAsync` already allows running `KjAsync` actions on an RPC runtime handle.
- The Lean codegen plugin can generate typed Lean APIs from `.capnp` schemas.

From `../openclaw` behavior we should preserve:

- challenge-first connection (`connect.challenge` event),
- first valid request must be `connect`,
- `hello-ok` with advertised methods/events + snapshot/policy,
- post-handshake request-only frames,
- role/scope auth and write-rate limiting,
- session-key-based chat lifecycle (`chat.send`, `chat.abort`, streaming events).

## Recommended Architecture

Use a dual-plane architecture:

1. Compatibility Plane (edge): OpenClaw-compatible WebSocket JSON frames.
2. Capability Plane (core): typed Cap'n Proto interfaces for gateway operations and streaming events.

The compatibility plane is a thin adapter over the capability plane. Do not duplicate business logic in both.

## Runtime Topology

Single runtime owner:

- Create one `Capnp.Rpc.Runtime`.
- Borrow `Capnp.KjAsync.Runtime` view via `Capnp.Rpc.Runtime.asKjAsyncRuntime` / `runKjAsync`.

This keeps:

- one event loop semantics,
- one shutdown boundary,
- one place for diagnostics/flow control.

## OpenClaw-to-Lean Mapping

| OpenClaw concept | Lean gateway equivalent |
| --- | --- |
| `server/ws-connection.ts` handshake | `KjAsync` WebSocket accept loop + handshake state machine |
| `server-methods.ts` dispatch by `req.method` | adapter routes to typed Cap'n Proto service calls |
| `GATEWAY_EVENTS` event fanout | event-stream capability + optional WS event bridge |
| `connect.challenge`/`hello-ok` | compatibility envelope over typed auth/session bootstrap |
| `chat.send` / `chat.abort` | `ChatGateway.send` + `RunHandle.abort` methods |
| snapshot/stateVersion | typed snapshot struct, monotonic counters |

## Protocol Model

### Phase A (compatibility-first)

Expose WS JSON frames equivalent to OpenClaw:

- `event`: `connect.challenge`
- `req`/`res` pairs
- `event` stream with `seq` and `stateVersion`

Adapter internals should decode JSON -> typed request structs -> Cap'n Proto service calls.

### Phase B (native clients)

Expose Cap'n Proto-native control API in parallel:

- either Cap'n Proto RPC over socket transport, or
- Cap'n Proto messages over WS binary frames (if browser transport is required).

Keep compatibility plane available until native clients reach parity.

## Cap'n Proto Schema Shape (Core)

Suggested schema split:

- `gateway_core.capnp` (auth/bootstrap/session/chat/event contracts)
- `gateway_compat.capnp` (optional structures mirroring WS envelope for bridge/testing)

Example sketch:

```capnp
@0xc0ffee00c0ffee00;

struct ConnectRequest {
  minProtocol @0 :UInt16;
  maxProtocol @1 :UInt16;
  clientId @2 :Text;
  clientVersion @3 :Text;
  role @4 :Text;
  scopes @5 :List(Text);
  nonce @6 :Text;
  token @7 :Text;
}

struct Hello {
  protocol @0 :UInt16;
  connId @1 :Text;
  snapshot @2 :Snapshot;
  policy @3 :Policy;
}

struct Snapshot {
  presenceVersion @0 :UInt64;
  healthVersion @1 :UInt64;
}

struct Policy {
  maxPayloadBytes @0 :UInt32;
  maxBufferedBytes @1 :UInt32;
  tickIntervalMs @2 :UInt32;
}

struct ChatEvent {
  runId @0 :Text;
  sessionKey @1 :Text;
  seq @2 :UInt64;
  state @3 :Text;
  message @4 :Text;
}

struct PresenceEvent {
  stateVersion @0 :UInt64;
}

struct HealthEvent {
  stateVersion @0 :UInt64;
}

struct ShutdownEvent {
  reason @0 :Text;
}

interface EventStream {
  next @0 () -> (event :GatewayEvent);
  cancel @1 ();
}

struct GatewayEvent {
  seq @0 :UInt64;
  union {
    chat @1 :ChatEvent;
    presence @2 :PresenceEvent;
    health @3 :HealthEvent;
    shutdown @4 :ShutdownEvent;
  }
}

interface ChatRun {
  abort @0 ();
}

interface Gateway {
  connect @0 (req :ConnectRequest) -> (hello :Hello, events :EventStream);
  chatSend @1 (sessionKey :Text, message :Text, idempotencyKey :Text)
      -> (runId :Text, run :ChatRun);
  chatHistory @2 (sessionKey :Text, limit :UInt32) -> (events :List(ChatEvent));
}
```

Lean codegen then gives typed method IDs + wrappers; this replaces stringly-typed method dispatch internally.

## Handshake and Lifecycle Semantics

Keep these semantics exactly (matching OpenClaw behavior):

1. Server accepts socket, emits challenge nonce.
2. First valid request must be `connect`.
3. Reject protocol mismatch / invalid role / auth failure before entering connected state.
4. On success, return `hello-ok` equivalent with:
   - protocol version
   - connId
   - feature advertisement
   - initial snapshot
   - policy limits
5. After handshake, accept only request frames (compat mode) or typed RPC calls (native mode).

Important rule: define a strict state machine (`pending -> connected -> closed`) and keep all auth checks in `pending`.

## Auth and Authorization

Carry over OpenClaw policy ideas, but model as typed data:

- role enum + explicit scope list in connect context,
- method/capability authorization table,
- per-actor write budget (e.g. `config.*`, `update.run` equivalent controls),
- optional device identity + nonce signature validation in handshake.

Longer-term, move from scope checks to capability attenuation where practical, but keep scope checks initially for compatibility.

## Session and ACP Semantics

Preserve the session-key model from OpenClaw ACP bridge:

- ACP session id maps to gateway `sessionKey`,
- `chat.send` supports idempotency keys,
- `chat.abort` targets active run (optionally by run id),
- events carry `runId`, `sessionKey`, `seq`, terminal states (`final`, `aborted`, `error`).

This allows existing ACP behavior to stay stable while backend moves to typed interfaces.

## Backpressure and Reliability

Required controls:

- bounded outbound event queue per connection,
- drop/close policy for slow consumers (explicit and observable),
- monotonic `seq` per stream for gap detection,
- idempotency key dedupe window for `chat.send`,
- explicit cancellation surface (`RunHandle.abort`, event-stream cancel).

Use `Capnp.Async.Promise` and runtime task helpers for composable cancellation boundaries.

## Implementation Plan

### Milestone 1: Typed Core Contracts

- Add gateway core `.capnp` schema.
- Generate Lean modules via existing plugin.
- Implement minimal in-process typed service skeleton + tests.

### Milestone 2: Compatibility Adapter

- Implement WS endpoint using `KjAsync` HTTP/WebSocket server.
- Match OpenClaw handshake + frame semantics.
- Route methods into typed core service.

### Milestone 3: Native Cap'n Proto Endpoint

- Expose typed endpoint for non-WS clients.
- Add feature parity tests against compatibility plane.

### Milestone 4: Policy + Ops Parity

- role/scope auth parity,
- write-rate limiting,
- snapshot/stateVersion parity,
- shutdown and disconnect behavior parity.

## Test Strategy

- Unit tests for handshake state machine and auth failure modes.
- Golden compatibility tests for frame-level parity (`connect.challenge`, `hello-ok`, request errors).
- Property tests for sequence monotonicity and idempotency.
- Cross-runtime tests proving one-runtime operation (`Rpc` + `KjAsync` in same loop).

## Key Design Decisions

- Keep OpenClaw WS behavior as an adapter contract, not the core model.
- Make Cap'n Proto interfaces the source of truth for business operations.
- Use one runtime handle to avoid split-loop lifecycle bugs.
- Preserve session/run semantics so ACP clients do not need behavioral rewrites.
