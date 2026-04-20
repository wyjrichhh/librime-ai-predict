#!/usr/bin/env bash
# Download a CTranslate2-exported model into Rime user data (default: macOS).
#
# Priority:
#   1) MODEL_URL — HTTP(S) URL to a .tar.gz / .tgz / .zip of the model directory
#      (recommended: GitHub Release asset, e.g. ~300MB; no Hugging Face required)
#   2) GITHUB_REPO + GITHUB_RELEASE_TAG + GITHUB_ASSET — builds MODEL_URL automatically
#   3) HUGGINGFACE_REPO — Hugging Face Hub (huggingface-cli or per-file curl)
#
# GitHub Releases: https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
# Single release assets can be up to ~2GB; plain git commits must stay under 100MB per file.

set -euo pipefail

: "${RIME_USER_DATA_DIR:=${HOME}/Library/Rime}"
DEST="${1:-${RIME_USER_DATA_DIR}/predict_models/zh-base-ct2-int8}"

# Optional: set these after you publish a Release on your plugin repo (or any repo).
: "${GITHUB_REPO:=}"
: "${GITHUB_RELEASE_TAG:=}"
: "${GITHUB_ASSET:=zh-base-ct2-int8.tar.gz}"

# Hugging Face (only if MODEL_URL / GITHUB_* are unset and this is non-empty).
: "${HUGGINGFACE_REPO:=}"

FILES=(
  config.json
  model.bin
  shared_vocabulary.json
  tokenizer.json
)

resolve_model_url() {
  if [[ -n "${MODEL_URL:-}" ]]; then
    echo "${MODEL_URL}"
    return
  fi
  if [[ -n "${GITHUB_REPO}" && -n "${GITHUB_RELEASE_TAG}" && -n "${GITHUB_ASSET}" ]]; then
    echo "https://github.com/${GITHUB_REPO}/releases/download/${GITHUB_RELEASE_TAG}/${GITHUB_ASSET}"
    return
  fi
  echo ""
}

download_hf() {
  if command -v huggingface-cli >/dev/null 2>&1; then
    echo "Using huggingface-cli to download ${HUGGINGFACE_REPO} -> ${DEST}"
    huggingface-cli download "${HUGGINGFACE_REPO}" \
      --local-dir "${DEST}" \
      --local-dir-use-symlinks False
  else
    echo "huggingface-cli not found; using curl per file (install: pip install huggingface_hub)"
    local base="https://huggingface.co/${HUGGINGFACE_REPO}/resolve/main"
    for f in "${FILES[@]}"; do
      echo "Downloading ${f} ..."
      curl -fL --retry 3 --retry-delay 2 -o "${DEST}/${f}" "${base}/${f}"
    done
  fi
}

download_and_extract_archive() {
  local url="$1"
  local tmp
  tmp="$(mktemp -d)"
  local arc="${tmp}/model_archive"
  echo "Downloading archive from:"
  echo "  ${url}"
  curl -fL --retry 3 --retry-delay 2 -o "${arc}" "${url}"

  mkdir -p "${tmp}/extract"
  case "${url}" in
    *.zip)
      unzip -q -o "${arc}" -d "${tmp}/extract"
      ;;
    *.tar.gz|*.tgz)
      tar -xzf "${arc}" -C "${tmp}/extract"
      ;;
    *)
      echo "ERROR: URL must end with .zip, .tar.gz, or .tgz" >&2
      exit 1
      ;;
  esac

  local found
  found="$(find "${tmp}/extract" -name shared_vocabulary.json | head -1 || true)"
  if [[ -z "${found}" ]]; then
    echo "ERROR: archive does not contain shared_vocabulary.json" >&2
    exit 1
  fi
  local parent
  parent="$(dirname "${found}")"
  mkdir -p "${DEST}"
  # shellcheck disable=SC2086
  cp -R "${parent}"/* "${DEST}/"
  rm -rf "${tmp}"
}

print_usage() {
  cat >&2 <<'EOF'
Set one of:
  MODEL_URL=https://github.com/OWNER/REPO/releases/download/TAG/zh-base-ct2-int8.tar.gz
  GITHUB_REPO=OWNER/REPO GITHUB_RELEASE_TAG=TGA GITHUB_ASSET=zh-base-ct2-int8.tar.gz
  HUGGINGFACE_REPO=USER/MODEL-REPO

See docs/MODEL_GITHUB_RELEASE.md (GitHub Releases, ~300MB OK) or docs/HUGGINGFACE_MODEL_CARD.md.
EOF
}

main() {
  mkdir -p "${DEST}"

  local url
  url="$(resolve_model_url)"

  if [[ -n "${url}" ]]; then
    download_and_extract_archive "${url}"
  elif [[ -n "${HUGGINGFACE_REPO}" ]]; then
    echo "Using Hugging Face: ${HUGGINGFACE_REPO}"
    download_hf
  else
    echo "ERROR: No download source configured." >&2
    print_usage
    exit 1
  fi

  if [[ ! -f "${DEST}/shared_vocabulary.json" ]]; then
    echo "ERROR: shared_vocabulary.json missing under ${DEST}" >&2
    exit 1
  fi

  echo "OK: model files under ${DEST}"
  echo "Set in schema: ai_predict/model_path: predict_models/zh-base-ct2-int8"
}

main "$@"
