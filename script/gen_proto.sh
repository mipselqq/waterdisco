#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
GEN_DIR="$ROOT_DIR/core/server/gen"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "[gen_proto] missing command: $1" >&2
    exit 1
  }
}

need_cmd protoc
export PATH="$(go env GOPATH 2>/dev/null)/bin:${PATH:-}"

if ! command -v protoc-gen-go >/dev/null 2>&1; then
  echo "[gen_proto] installing protoc-gen-go"
  GOWORK=off GO111MODULE=on go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
fi
if ! command -v protoc-gen-go-grpc >/dev/null 2>&1; then
  echo "[gen_proto] installing protoc-gen-go-grpc"
  GOWORK=off GO111MODULE=on go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
fi

echo "[gen_proto] generating Go bindings from libcore.proto"
(
  cd "$GEN_DIR"
  protoc -I . --go_out=. --go-grpc_out=. libcore.proto
)
