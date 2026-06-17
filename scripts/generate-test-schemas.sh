#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

capnp_bin="${CAPNP_BIN:-capnp}"
plugin_spec="${CAPNP_LEAN4_PLUGIN:-lean4}"
out_dir="${CAPNP_LEAN4_OUT_DIR:-test/out}"

if [[ "${capnp_bin}" == */* ]]; then
  capnp_bin="$(cd "$(dirname "${capnp_bin}")" && pwd)/$(basename "${capnp_bin}")"
fi

if [[ "${plugin_spec}" == */* ]]; then
  plugin_spec="$(cd "$(dirname "${plugin_spec}")" && pwd)/$(basename "${plugin_spec}")"
fi

mkdir -p "${out_dir}"
out_dir="$(cd "$(dirname "${out_dir}")" && pwd)/$(basename "${out_dir}")"

case "${out_dir}" in
  ""|"/")
    echo "refusing to use unsafe output directory: ${out_dir}" >&2
    exit 2
    ;;
esac
rm -rf "${out_dir}"
mkdir -p "${out_dir}"

(
  cd test
  "${capnp_bin}" compile \
    "-o${plugin_spec}:${out_dir}" \
    --src-prefix . \
    -I . \
    -I "${repo_root}/extern/capnproto/c++/src" \
    addressbook.capnp \
    fixtures/defaults.capnp \
    fixtures/capability.capnp \
    fixtures/rpc_echo.capnp
)

(
  cd extern/capnproto/c++/src
  "${capnp_bin}" compile \
    "-o${plugin_spec}:${out_dir}" \
    --src-prefix . \
    -I . \
    capnp/test.capnp \
    capnp/rpc.capnp \
    capnp/rpc-twoparty.capnp \
    capnp/stream.capnp
)

expected_outputs=(
  "Capnp/Gen/addressbook.lean"
  "Capnp/Gen/fixtures/defaults.lean"
  "Capnp/Gen/fixtures/capability.lean"
  "Capnp/Gen/fixtures/rpc_echo.lean"
  "Capnp/Gen/capnp/test.lean"
  "Capnp/Gen/capnp/rpc.lean"
  "Capnp/Gen/capnp/rpc_twoparty.lean"
  "Capnp/Gen/capnp/stream.lean"
)

missing=0
for generated in "${expected_outputs[@]}"; do
  if [[ ! -f "${out_dir}/${generated}" ]]; then
    echo "missing generated schema output: ${out_dir}/${generated}" >&2
    missing=1
  fi
done
exit "${missing}"
