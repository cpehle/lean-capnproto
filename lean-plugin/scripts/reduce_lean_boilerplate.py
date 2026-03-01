import re

with open('lean/Capnp/KjAsync.lean', 'r') as f:
    content = f.read()

# Replace redundant `xxxAsPromise` wrappers which just wrap `xxxAsTask`
# Typical pattern:
# @[inline] def xxxAsPromise (args...) : IO (Capnp.Async.Promise ReturnType) := do
#   let task <- xxxAsTask (args...)
#   pure (Capnp.Async.Promise.ofTask task)

def remove_as_promise(content):
    # This might be tricky via regex since arguments can span multiple lines.
    # Instead, we can add a macro in lean/Capnp/Async.lean:
    # macro "derive_promise_from_task" name:ident taskName:ident : command => `(...)
    pass

print("Need a more robust Lean macro for this, string replace might be fragile.")
