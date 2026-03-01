import Init
import Init.System.Promise

namespace Capnp
namespace Async

class Awaitable (promise : Type u) (α : Type) where
  await : promise -> IO α

class Cancelable (promise : Type u) where
  cancel : promise -> IO Unit

class Releasable (promise : Type u) where
  release : promise -> IO Unit

@[inline] def await {ρ : Type u} {α : Type} [inst : Awaitable ρ α] (ref : ρ) : IO α :=
  inst.await ref

@[inline] def cancel {ρ : Type u} [inst : Cancelable ρ] (ref : ρ) : IO Unit :=
  inst.cancel ref

@[inline] def release {ρ : Type u} [inst : Releasable ρ] (ref : ρ) : IO Unit :=
  inst.release ref

@[inline] def withRelease {ρ : Type u} [Releasable ρ] (ref : ρ) (action : ρ → IO α) : IO α := do
  try
    action ref
  finally
    release ref

@[inline] def cancelAndRelease {ρ : Type u} [Cancelable ρ] [Releasable ρ] (ref : ρ) : IO Unit := do
  try
    cancel ref
  finally
    release ref

@[inline] def awaitAsTask {ρ : Type u} {α : Type} [Awaitable ρ α] (ref : ρ) :
    IO (Task (Except IO.Error α)) :=
  IO.asTask (await ref)

def toIOPromise {ρ : Type u} {α : Type} [Awaitable ρ α]
    (ref : ρ) : IO (IO.Promise (Except String α)) := do
  let out ← IO.Promise.new
  let _task ← IO.asTask do
    let result ←
      try
        let value ← await ref
        pure (Except.ok value)
      catch e =>
        pure (Except.error e.toString)
    out.resolve result
  pure out

/-!
A small "promise" wrapper for Lean code that wants a KJ-style API (`then`/`catch`/`all`/`race`)
without committing to a particular backend.

Internally this is just `Task (Except IO.Error α)`, which is exactly what `IO.asTask` yields and
what the RPC advanced handler API already consumes.
-/
structure Promise (α : Type) where
  task : Task (Except IO.Error α)

namespace Promise

@[inline] def ofTask (task : Task (Except IO.Error α)) : Promise α := { task := task }

@[inline] def toTask (promise : Promise α) : Task (Except IO.Error α) := promise.task

@[inline] def awaitResult (promise : Promise α) : IO (Except IO.Error α) :=
  IO.wait promise.task

@[inline] def await (promise : Promise α) : IO α := do
  match (← promise.awaitResult) with
  | .ok value => pure value
  | .error err => throw err

@[inline] def pure (value : α) : Promise α :=
  ofTask (Task.pure (.ok value))

def map (f : α → β) (promise : Promise α) : Promise β :=
  ofTask <| Task.map (fun result => result.map f) promise.task

def bind (promise : Promise α) (next : α → Promise β) : Promise β :=
  ofTask <|
    Task.bind promise.task fun result =>
      match result with
      | .ok value => (next value).task
      | .error err => Task.pure (.error err)

instance : Functor Promise where
  map := Promise.map

instance : Monad Promise where
  pure := Promise.pure
  bind := Promise.bind

@[inline] def «then» (promise : Promise α) (next : α → Promise β) : Promise β :=
  bind promise next

def «catch» (promise : Promise α) (handler : IO.Error → Promise α) : Promise α :=
  ofTask <|
    Task.bind promise.task fun result =>
      match result with
      | .ok value => Task.pure (.ok value)
      | .error err => (handler err).task

def all (promises : Array (Promise α)) : Promise (Array α) :=
  promises.foldl
    (init := pure #[])
    (fun acc p => do
      let xs ← acc
      let x ← p
      pure (xs.push x))

def race (a b : Promise α) : IO (Promise α) := do
  let task ← IO.asTask do
    let result ← IO.waitAny [a.task, b.task]
    match result with
    | .ok value => return value
    | .error err => throw err
  return (ofTask task)

def fromAwaitable {ρ : Type u} {α : Type} [Awaitable ρ α] (ref : ρ) : IO (Promise α) := do
  return ofTask (← awaitAsTask ref)

end Promise

end Async
end Capnp
