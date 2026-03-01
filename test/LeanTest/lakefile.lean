import Lake
open Lake DSL

package LeanTest where
  version := v!"0.1.0"

@[default_target]
lean_lib LeanTest where
  roots := #[`LeanTest]
