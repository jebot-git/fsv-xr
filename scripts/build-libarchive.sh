#!/usr/bin/env bash
# Build a pinned, private dependency as the regular user; never replace /usr/lib.
set -euo pipefail
[[ $(id -u) -ne 0 ]] || { echo 'Build libarchive as the regular user.' >&2; exit 1; }
[[ $# -eq 1 ]] || { echo 'Usage: build-libarchive.sh PRIVATE_PREFIX' >&2; exit 2; }
fsv_source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
fsv_archive_prefix="$(realpath -m -- "$1")"
fsv_archive_work="${FSVR_LIBARCHIVE_WORK:-$fsv_source_root/build-libarchive}"
mapfile -t fsv_archive_pin < <(python3 - "$fsv_source_root/third_party/libarchive-source.json" <<'PY'
import json,sys
pin=json.load(open(sys.argv[1]))
for key in ('version','url','sha256'):print(pin[key])
PY
)
mkdir -p -- "$fsv_archive_work"
fsv_archive_tar="$fsv_archive_work/libarchive-${fsv_archive_pin[0]}.tar.xz"
if [[ ! -f "$fsv_archive_tar" ]]; then curl --fail --location --retry 2 "${fsv_archive_pin[1]}" --output "$fsv_archive_tar"; fi
printf '%s  %s\n' "${fsv_archive_pin[2]}" "$fsv_archive_tar" | sha256sum --check -
tar -xf "$fsv_archive_tar" -C "$fsv_archive_work"
fsv_archive_source="$fsv_archive_work/libarchive-${fsv_archive_pin[0]}"
cmake -S "$fsv_archive_source" -B "$fsv_archive_work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$fsv_archive_prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_LZO=ON -DENABLE_TEST=OFF -DENABLE_TAR=OFF \
  -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF
cmake --build "$fsv_archive_work/build" -j4
cmake --install "$fsv_archive_work/build"
install -D -m 0644 "$fsv_archive_source/COPYING" "$fsv_archive_prefix/share/doc/libarchive/COPYING"
