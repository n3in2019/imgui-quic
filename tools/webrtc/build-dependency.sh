#!/usr/bin/env bash
# Build an unmodified, pinned libdatachannel with bundled pinned submodules.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
source_dir="$root/third_party/libdatachannel"
revision=a02b751917ac8afc8c58dc6f4461d25ff9465d48
cmake_bin=${CMAKE_BIN:-cmake}
if [ ! -d "$source_dir/.git" ]; then
  git init "$source_dir"
  git -C "$source_dir" fetch --depth 1 https://github.com/paullouisageneau/libdatachannel.git "$revision"
  git -C "$source_dir" checkout --detach FETCH_HEAD
fi
[ "$(git -C "$source_dir" rev-parse HEAD)" = "$revision" ] || { echo 'Unexpected libdatachannel revision' >&2; exit 1; }
git -C "$source_dir" submodule update --init --depth 1 deps/json deps/libjuice deps/plog deps/usrsctp
"$cmake_bin" -S "$source_dir" -B "$root/build/rtc-dependency" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$root/build/rtc-prefix" \
  -DNO_MEDIA=ON -DNO_TESTS=ON -DNO_EXAMPLES=ON -DBUILD_SHARED_LIBS=ON
"$cmake_bin" --build "$root/build/rtc-dependency" --target install --parallel "${JOBS:-4}"
notices="$root/build/rtc-prefix/share/doc/imgui_quic/webrtc"
mkdir -p "$notices"
cp "$source_dir/LICENSE" "$notices/libdatachannel-LICENSE"
for dep in libjuice plog usrsctp json; do
  mkdir -p "$notices/$dep"
  find "$source_dir/deps/$dep" -maxdepth 1 -type f \( -iname '*license*' -o -iname '*copying*' \) -exec cp '{}' "$notices/$dep/" \;
done
