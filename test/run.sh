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

"$build_dir/mplane_route_test"
