#!/usr/bin/env bash
# Download a CTranslate2-exported model into Rime user data (default: macOS).
# Prefer: huggingface-cli (from pip package huggingface_hub).
# Fallback: curl each file from Hugging Face raw URLs.

set -euo pipefail

# Override with your Hugging Face model repo after you upload weights.
: "${HUGGINGFACE_REPO:=wyjrichhh/rime-ai-predict-zh-base}"
: "${RIME_USER_DATA_DIR:=${HOME}/Library/Rime}"
DEST="${1:-${RIME_USER_DATA_DIR}/predict_models/zh-base-ct2-int8}"

# Files typically present in a CT2 seq2seq export used by librime-ai-predict.
FILES=(
  config.json
  model.bin
  shared_vocabulary.json
  tokenizer.json
)

mkdir -p "${DEST}"

if command -v huggingface-cli >/dev/null 2>&1; then
  echo "Using huggingface-cli to download ${HUGGINGFACE_REPO} -> ${DEST}"
  huggingface-cli download "${HUGGINGFACE_REPO}" \
    --local-dir "${DEST}" \
    --local-dir-use-symlinks False
else
  echo "huggingface-cli not found; using curl (install: pip install huggingface_hub)"
  BASE_URL="https://huggingface.co/${HUGGINGFACE_REPO}/resolve/main"
  for f in "${FILES[@]}"; do
    echo "Downloading ${f} ..."
    curl -fL --retry 3 --retry-delay 2 -o "${DEST}/${f}" "${BASE_URL}/${f}"
  done
fi

if [[ ! -f "${DEST}/shared_vocabulary.json" ]]; then
  echo "ERROR: shared_vocabulary.json missing under ${DEST}" >&2
  exit 1
fi

echo "OK: model files under ${DEST}"
echo "Set in schema: ai_predict/model_path: predict_models/zh-base-ct2-int8"
