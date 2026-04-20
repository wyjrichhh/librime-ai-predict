#!/usr/bin/env bash
# Download the CTranslate2 model used by librime-ai-predict.
#
# Usage:
#   ./scripts/download_model.sh [DEST_DIR]
#
# DEST_DIR defaults to: $RIME_USER_DATA_DIR/predict_models/zh-base-ct2-int8
#                      (RIME_USER_DATA_DIR defaults to ~/Library/Rime on macOS)
#
# Override the source URL (e.g. for a mirror) by setting MODEL_URL.

set -euo pipefail

DEFAULT_MODEL_URL="https://github.com/wyjrichhh/librime-ai-predict/releases/download/model-v1/zh-base-ct2-int8.tar.gz"
MODEL_URL="${MODEL_URL:-${DEFAULT_MODEL_URL}}"

: "${RIME_USER_DATA_DIR:=${HOME}/Library/Rime}"
DEST="${1:-${RIME_USER_DATA_DIR}/predict_models/zh-base-ct2-int8}"

main() {
  mkdir -p "${DEST}"

  local tmp arc
  tmp="$(mktemp -d)"
  arc="${tmp}/model_archive"

  echo "Downloading model from:"
  echo "  ${MODEL_URL}"
  curl -fL --retry 3 --retry-delay 2 -o "${arc}" "${MODEL_URL}"

  mkdir -p "${tmp}/extract"
  case "${MODEL_URL}" in
    *.zip)
      unzip -q -o "${arc}" -d "${tmp}/extract"
      ;;
    *.tar.gz|*.tgz)
      tar -xzf "${arc}" -C "${tmp}/extract"
      ;;
    *)
      echo "ERROR: archive URL must end in .zip / .tar.gz / .tgz" >&2
      exit 1
      ;;
  esac

  local found parent
  found="$(find "${tmp}/extract" -name shared_vocabulary.json | head -1 || true)"
  if [[ -z "${found}" ]]; then
    echo "ERROR: archive did not contain shared_vocabulary.json" >&2
    exit 1
  fi
  parent="$(dirname "${found}")"

  cp -R "${parent}"/* "${DEST}/"
  rm -rf "${tmp}"

  echo "OK: model installed at ${DEST}"
  echo "Set in your schema: ai_predict/model_path: predict_models/zh-base-ct2-int8"
}

main "$@"
