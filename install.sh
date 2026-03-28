#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
INSTALL_DIR="${INSTALL_DIR:-/opt/Throne}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
SKIP_BUILD="${SKIP_BUILD:-0}"
CORE_PATH="${CORE_PATH:-}"
CORE_GO_TAGS="${CORE_GO_TAGS:-with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls,with_dhcp,with_tailscale,with_purego,with_naive_outbound,badlinkname,tfogo_checklinkname0}"

log() {
  printf '[install] %s\n' "$*"
}

die() {
  printf '[install][error] %s\n' "$*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Command not found: $1"
}

require_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    die "Please run this installer as root (e.g. 'sudo ./install.sh')"
  fi
}

usage() {
  cat <<'EOF'
Usage: ./install.sh [options]

Options:
  --build-dir <path>   CMake build dir (default: ./build)
  --prefix <path>      Install dir (default: /opt/Throne)
  --core <path>        Path to ThroneCore binary
  --skip-build         Do not build, only install from existing artifacts
  -h, --help           Show this help

Environment overrides:
  BUILD_DIR, INSTALL_DIR, CMAKE_BUILD_TYPE, SKIP_BUILD, CORE_PATH, CORE_GO_TAGS
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires a value"
      BUILD_DIR="$2"
      shift 2
      ;;
    --prefix)
      [[ $# -ge 2 ]] || die "--prefix requires a value"
      INSTALL_DIR="$2"
      shift 2
      ;;
    --core)
      [[ $# -ge 2 ]] || die "--core requires a value"
      CORE_PATH="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "Unknown option: $1"
      ;;
  esac
done

need_cmd cmake

if [[ "$SKIP_BUILD" != "1" ]]; then
  need_cmd go
fi

if [[ "$SKIP_BUILD" != "1" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    CMAKE_GENERATOR="Ninja"
  else
    CMAKE_GENERATOR="Unix Makefiles"
  fi

  log "[1/2] Configuring CMake (${CMAKE_BUILD_TYPE}, generator: ${CMAKE_GENERATOR})"
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "$CMAKE_GENERATOR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"

  BUILD_JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
  export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"
  log "[2/2] Building Throne with nproc=${BUILD_JOBS} jobs (progress below)"
  cmake --build "$BUILD_DIR" --target Throne --parallel "$BUILD_JOBS" --verbose

  if [[ -z "$CORE_PATH" ]]; then
    log "Building ThroneCore from core/server"
    mkdir -p "$BUILD_DIR"
    (
      cd "$ROOT_DIR/core/server"
      if [[ ! -f "gen/libcore.pb.go" || ! -f "gen/libcore_grpc.pb.go" ]]; then
        need_cmd protoc
        export PATH="$(go env GOPATH)/bin:$PATH"
        if ! command -v protoc-gen-go >/dev/null 2>&1; then
          log "Installing protoc-gen-go"
          GOWORK=off GO111MODULE=on go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
        fi
        if ! command -v protoc-gen-go-grpc >/dev/null 2>&1; then
          log "Installing protoc-gen-go-grpc"
          GOWORK=off GO111MODULE=on go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
        fi
        log "Generating Go gRPC bindings from gen/libcore.proto"
        (
          cd gen
          protoc -I . --go_out=. --go-grpc_out=. libcore.proto
        )
      fi

      # Force module mode so local imports like ThroneCore/gen resolve even if user env disables modules.
      # Keep parity with script/build_go.sh tags so Clash API and related features are available.
      GOWORK=off GO111MODULE=on go build -trimpath -ldflags "-w -s -X 'internal/godebug.defaultGODEBUG=multipathtcp=0' -checklinkname=0" -tags "$CORE_GO_TAGS" -o "$BUILD_DIR/ThroneCore" .
    )
    CORE_PATH="$BUILD_DIR/ThroneCore"
  fi
fi

THRONE_BIN="$BUILD_DIR/Throne"
ICON_SRC="$ROOT_DIR/res/public/Throne.png"

if [[ -z "$CORE_PATH" ]]; then
  for candidate in \
    "$BUILD_DIR/ThroneCore" \
    "$ROOT_DIR/ThroneCore" \
    "$ROOT_DIR/deployment/linux-amd64/ThroneCore" \
    "$ROOT_DIR/deployment/linux-arm64/ThroneCore"
  do
    if [[ -f "$candidate" ]]; then
      CORE_PATH="$candidate"
      break
    fi
  done
fi

[[ -x "$THRONE_BIN" ]] || die "Throne binary not found: $THRONE_BIN"
[[ -n "$CORE_PATH" ]] || die "ThroneCore not found. Pass explicit path: --core /path/to/ThroneCore"
[[ -x "$CORE_PATH" ]] || die "ThroneCore is missing or not executable: $CORE_PATH"
[[ -f "$ICON_SRC" ]] || die "Icon not found: $ICON_SRC"

STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT

log "Preparing install files"
cp "$THRONE_BIN" "$STAGE_DIR/Throne"
cp "$CORE_PATH" "$STAGE_DIR/ThroneCore"
cp "$ICON_SRC" "$STAGE_DIR/Throne.png"
chmod +x "$STAGE_DIR/Throne" "$STAGE_DIR/ThroneCore"

log "Overwriting installation at: $INSTALL_DIR"
[[ -n "$INSTALL_DIR" ]] || die "INSTALL_DIR is empty"
[[ "$INSTALL_DIR" != "/" ]] || die "Refusing to overwrite root directory"
require_root
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
cp -a "$STAGE_DIR/." "$INSTALL_DIR/"

chown -R root:root "$INSTALL_DIR"
find "$INSTALL_DIR" -type d -exec chmod 755 {} +
find "$INSTALL_DIR" -type f -exec chmod 644 {} +
if [[ -f "$INSTALL_DIR/Throne" ]]; then
  chmod 755 "$INSTALL_DIR/Throne"
fi
if [[ -f "$INSTALL_DIR/ThroneCore" ]]; then
  chmod 755 "$INSTALL_DIR/ThroneCore"
fi
if [[ -f "$INSTALL_DIR/updater" ]]; then
  chmod 755 "$INSTALL_DIR/updater"
fi

DESKTOP_DST="/usr/share/applications/throne.desktop"
ICON_DST="/usr/share/icons/hicolor/256x256/apps/throne.png"
BIN_LINK_DST="/usr/local/bin/throne"

log "Installing desktop entry and icon"
install -d /usr/share/icons/hicolor/256x256/apps
install -m 0644 "$ICON_SRC" "$ICON_DST"

cat > "$STAGE_DIR/throne.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Throne
Comment=Throne Proxy Utility
Exec=$INSTALL_DIR/Throne -appdata
Icon=throne
Terminal=false
Categories=Network;Utility;
StartupNotify=true
EOF
install -m 0644 "$STAGE_DIR/throne.desktop" "$DESKTOP_DST"

cat > "$STAGE_DIR/throne" <<EOF
#!/usr/bin/env bash
exec "$INSTALL_DIR/Throne" -appdata "\$@"
EOF
install -m 0755 "$STAGE_DIR/throne" "$BIN_LINK_DST"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f /usr/share/icons/hicolor || true
fi

log "Done"
log "Installed to: $INSTALL_DIR"
log "Run: throne"
