#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
build_dir="$repo_dir/build_test"
mkdir -p "$build_dir"

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$repo_dir/components/winject_proxy" \
  "$repo_dir/test/mplane_route_test.c" \
  "$repo_dir/components/winject_proxy/mplane_route.c" \
  -o "$build_dir/mplane_route_test"

# P4 -> C6 send queue. sdio_msg comes from winject-radio-esp32c6: the commit
# cmake/winject_sdio_proxy.cmake pins (fetched into build_test), or a checkout
# given as WINJECT_C6_ROOT.
c6_root=${WINJECT_C6_ROOT:-}
if [ -z "$c6_root" ]; then
  c6_tag=$(sed -n 's/^ *GIT_TAG \([0-9a-f]\{40\}\).*/\1/p' "$repo_dir/cmake/winject_sdio_proxy.cmake" | head -1)
  c6_root="$build_dir/winject-radio-esp32c6"
  if [ "$(git -C "$c6_root" rev-parse HEAD 2>/dev/null)" != "$c6_tag" ]; then
    rm -rf "$c6_root"
    git init -q "$c6_root"
    git -C "$c6_root" fetch -q --depth 1 https://github.com/winject-wireless/winject-radio-esp32c6.git "$c6_tag" \
      && git -C "$c6_root" checkout -q FETCH_HEAD || c6_root=""
  fi
fi
if [ -n "$c6_root" ] && [ -f "$c6_root/components/sdio_msg/sdio_msg.c" ]; then
  cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$repo_dir/components/sdio_link_host" -I"$c6_root/components/sdio_msg/include" \
    "$repo_dir/test/sdio_tx_ring_test.c" \
    "$repo_dir/components/sdio_link_host/sdio_tx_ring.c" \
    "$c6_root/components/sdio_msg/sdio_msg.c" \
    -o "$build_dir/sdio_tx_ring_test"
  "$build_dir/sdio_tx_ring_test"
else
  echo "sdio_tx_ring_test SKIPPED: no winject-radio-esp32c6 source (set WINJECT_C6_ROOT)" >&2
fi

"$build_dir/mplane_route_test"

# udp_ota core (components/udp_ota/core): unit tests, and a host server for
# tools/udp_ota.py (build_test/udp_ota_host_server <port> <out> [drop%] [dup%] [fs dir]).
core="$repo_dir/components/udp_ota/core"
for t in udp_ota_test udp_ota_host_server; do
  c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$core" \
    "$repo_dir/test/$t.cpp" "$core/server.cpp" "$core/fs.cpp" -o "$build_dir/$t"
done
"$build_dir/udp_ota_test"
