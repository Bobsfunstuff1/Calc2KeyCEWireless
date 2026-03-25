#!/usr/bin/env bash
set -euo pipefail

output_dir="${1:-dist}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage_dir="${repo_root}/${output_dir}/calculator"
zip_path="${repo_root}/${output_dir}/Calc2KeyCE-Calculator.zip"

files=(
  "${repo_root}/Calc2KeyCE.Calc/bin/Calc2Key.8xp"
  "${repo_root}/README.md"
)

for file in "${files[@]}"; do
  if [[ ! -f "${file}" ]]; then
    echo "Required calculator artifact not found: ${file}" >&2
    exit 1
  fi
done

mkdir -p "${stage_dir}"
rm -f "${zip_path}"
find "${stage_dir}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +

cp "${repo_root}/Calc2KeyCE.Calc/bin/Calc2Key.8xp" "${stage_dir}/"
cp "${repo_root}/README.md" "${stage_dir}/"

(
  cd "${stage_dir}"
  zip -r "${zip_path}" .
)

echo "Created ${zip_path}"
