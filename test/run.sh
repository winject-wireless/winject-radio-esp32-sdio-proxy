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

# P4 -> C6 send queue; sdio_msg comes from winject-radio-esp32c6
# (WINJECT_C6_ROOT, default the sibling checkout).
c6_root=${WINJECT_C6_ROOT:-$repo_dir/../winject-radio-esp32c6}
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$repo_dir/components/sdio_link_host" -I"$c6_root/components/sdio_msg/include" \
  "$repo_dir/test/sdio_tx_ring_test.c" \
  "$repo_dir/components/sdio_link_host/sdio_tx_ring.c" \
  "$c6_root/components/sdio_msg/sdio_msg.c" \
  -o "$build_dir/sdio_tx_ring_test"
"$build_dir/sdio_tx_ring_test"

"$build_dir/mplane_route_test"

# udp_ota core (components/udp_ota/core): unit tests, and a host server for
# tools/udp_ota.py (build_test/udp_ota_host_server <port> <out> [drop%] [dup%] [fs dir]).
core="$repo_dir/components/udp_ota/core"
for t in udp_ota_test udp_ota_host_server; do
  c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$core" \
    "$repo_dir/test/$t.cpp" "$core/server.cpp" "$core/fs.cpp" -o "$build_dir/$t"
done
"$build_dir/udp_ota_test"
