#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="${1:-dist}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/Calc2KeyCE.PiRelay/build"
STAGE_DIR="${REPO_ROOT}/${OUTPUT_DIR}/pi-relay"
ARCHIVE_PATH="${REPO_ROOT}/${OUTPUT_DIR}/Calc2KeyCE-PiRelay.tar.gz"

mkdir -p "${STAGE_DIR}"
rm -rf "${STAGE_DIR:?}/"*

for file in Calc2KeyPiRelay Calc2PiConsoleRelay; do
    if [[ ! -f "${BUILD_DIR}/${file}" ]]; then
        echo "Missing build output: ${BUILD_DIR}/${file}"
        exit 1
    fi
    cp "${BUILD_DIR}/${file}" "${STAGE_DIR}/${file}"
done

cp "${REPO_ROOT}/README.md" "${STAGE_DIR}/README.md"

mkdir -p "${REPO_ROOT}/${OUTPUT_DIR}"
tar -czf "${ARCHIVE_PATH}" -C "${STAGE_DIR}" .
echo "Created ${ARCHIVE_PATH}"
