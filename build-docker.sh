#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$ROOT_DIR/.build/zmk-docker"
FIRMWARE_DIR="$ROOT_DIR/firmware"
IMAGE="${ZMK_DOCKER_IMAGE:-zmkfirmware/zmk-build-arm:stable}"
TARGET="all"
UPDATE=0

usage() {
    cat <<'EOF'
Usage: ./build-docker.sh [all|left|right] [--update]

  all       Build both halves (default)
  left      Build the left/central half only
  right     Build the right/peripheral half only
  --update  Update ZMK and its west dependencies before building
EOF
}

for arg in "$@"; do
    case "$arg" in
    all | left | right)
        TARGET="$arg"
        ;;
    --update)
        UPDATE=1
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown argument: $arg" >&2
        usage >&2
        exit 2
        ;;
    esac
done

docker_ready() {
    docker info >/dev/null 2>&1
}

start_docker() {
    if docker_ready; then
        return
    fi

    if command -v open >/dev/null 2>&1 && open -Ra OrbStack >/dev/null 2>&1; then
        echo "Starting OrbStack..."
        open -ga OrbStack
    elif command -v open >/dev/null 2>&1 && open -Ra Docker >/dev/null 2>&1; then
        echo "Starting Docker Desktop..."
        open -ga Docker
    else
        echo "Docker is not running. Start Docker and try again." >&2
        exit 1
    fi

    for _ in $(seq 1 60); do
        if docker_ready; then
            return
        fi
        sleep 1
    done

    echo "Docker did not become ready within 60 seconds." >&2
    exit 1
}

start_docker
mkdir -p "$WORKSPACE_DIR" "$FIRMWARE_DIR"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker pull "$IMAGE"
fi

docker run --rm -i \
    -v "$ROOT_DIR:/config:ro" \
    -v "$WORKSPACE_DIR:/work" \
    -w /work \
    "$IMAGE" \
    bash -s -- "$TARGET" "$UPDATE" <<'CONTAINER_SCRIPT'
set -euo pipefail

target="$1"
update="$2"
initialized=0

if [[ ! -d /work/zmk/.git ]]; then
    echo "Cloning ZMK..."
    git clone --depth 1 https://github.com/zmkfirmware/zmk.git /work/zmk
    initialized=1
fi

cd /work/zmk

if [[ "$update" == "1" ]]; then
    echo "Updating ZMK..."
    git pull --ff-only
fi

if [[ ! -d .west ]]; then
    west init -l app
    initialized=1
fi

if [[ "$initialized" == "1" || "$update" == "1" ]]; then
    west update -o=--depth=1 -n
    west zephyr-export
fi

cd app

build_left() {
    west build -p auto -d build/cornix-left -b cornix_left -- \
        -DSHIELD=cornix_indicator \
        -DZMK_CONFIG=/config/config \
        -DZMK_EXTRA_MODULES=/config
}

build_right() {
    west build -p auto -d build/cornix-right -b cornix_right -- \
        -DSHIELD=cornix_indicator \
        -DZMK_CONFIG=/config/config \
        -DZMK_EXTRA_MODULES=/config
}

case "$target" in
left)
    build_left
    ;;
right)
    build_right
    ;;
all)
    build_left
    build_right
    ;;
esac
CONTAINER_SCRIPT

if [[ "$TARGET" == "all" || "$TARGET" == "left" ]]; then
    cp "$WORKSPACE_DIR/zmk/app/build/cornix-left/zephyr/zmk.uf2" \
        "$FIRMWARE_DIR/cornix_left_central_usb_ble_nosd.uf2"
    echo "Built: firmware/cornix_left_central_usb_ble_nosd.uf2"
fi

if [[ "$TARGET" == "all" || "$TARGET" == "right" ]]; then
    cp "$WORKSPACE_DIR/zmk/app/build/cornix-right/zephyr/zmk.uf2" \
        "$FIRMWARE_DIR/cornix_right_peripheral_nosd.uf2"
    echo "Built: firmware/cornix_right_peripheral_nosd.uf2"
fi
