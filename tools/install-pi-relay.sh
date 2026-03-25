#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <windows-host-or-ip> [port]"
    exit 1
fi

BRIDGE_HOST="$1"
BRIDGE_PORT="${2:-28400}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/Calc2KeyCE.PiRelay/build"
INSTALL_DIR="/opt/calc2key"
BIN_NAME="Calc2KeyPiRelay"

sudo apt update
sudo apt install -y build-essential cmake pkg-config libusb-1.0-0-dev

cmake -S "${REPO_ROOT}/Calc2KeyCE.PiRelay" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j

sudo mkdir -p "${INSTALL_DIR}/bin"
sudo cp "${BUILD_DIR}/${BIN_NAME}" "${INSTALL_DIR}/bin/${BIN_NAME}"

sudo mkdir -p /etc/calc2key
cat <<EOF | sudo tee /etc/calc2key/pirelay.env >/dev/null
BRIDGE_HOST=${BRIDGE_HOST}
BRIDGE_PORT=${BRIDGE_PORT}
EOF

echo "Installed ${BIN_NAME} to ${INSTALL_DIR}/bin/${BIN_NAME}"
echo "Bridge config written to /etc/calc2key/pirelay.env"
echo "Next step: install the systemd unit from tools/calc2key-pirelay.service.example"
