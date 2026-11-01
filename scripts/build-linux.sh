#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_args=()

if [[ ! -f /usr/include/mysql/mysql.h && ! -f /usr/include/mariadb/mysql.h ]]; then
    if ! command -v apt >/dev/null || ! command -v dpkg-deb >/dev/null; then
        echo 'Install MySQL or MariaDB client development files before building LoginSvr.' >&2
        exit 1
    fi

    deps_root="${XDG_CACHE_HOME:-$HOME/.cache}/enet-vscode-build/deps"
    include_dir="$deps_root/root/usr/include/mariadb"
    client_library="$(find "$deps_root/root/usr/lib" -name libmariadb.so -print -quit 2>/dev/null || true)"

    if [[ ! -f "$include_dir/mysql.h" || -z "$client_library" ]]; then
        mkdir -p "$deps_root/packages" "$deps_root/root"
        (
            cd "$deps_root/packages"
            apt download libmariadb-dev libmariadb3
        )
        for package in "$deps_root"/packages/libmariadb-dev_*.deb "$deps_root"/packages/libmariadb3_*.deb; do
            dpkg-deb -x "$package" "$deps_root/root"
        done
        client_library="$(find "$deps_root/root/usr/lib" -name libmariadb.so -print -quit)"
    fi

    cmake_args+=("-DENET_MYSQL_INCLUDE_DIR=$include_dir")
    cmake_args+=("-DENET_MYSQL_CLIENT_LIBRARY=$client_library")
fi

cmake -S "$project_root" --preset linux-make-debug -DENET_BUILD_LOGIN_SERVER=ON "${cmake_args[@]}"
cmake --build "$project_root/build/make-debug" --parallel 4
