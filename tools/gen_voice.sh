#!/usr/bin/env bash
# gen_voice.sh — generate the 11 voice-phrase WAVs for NotificationService.
#
# Reads tools/voice_phrases.txt (event_name | phrase) and writes one WAV per
# line into assets/sounds/voice/<event_name>.wav.
#
# Pipeline per phrase:
#   piper → 22.05 kHz mono 16-bit WAV → sox (trim leading/trailing silence,
#   resample to 22.05 kHz, normalise to -3 dBFS).
#
# Run from the repo root:
#   ./tools/gen_voice.sh             # skips files that already exist
#   ./tools/gen_voice.sh --force     # overwrites every WAV
#
# Prerequisites:
#   apt:   sudo apt install piper-tts sox
#   brew:  brew install piper sox
#   model: download en_US-lessac-medium.onnx + .json into tools/piper-voices/
#          https://huggingface.co/rhasspy/piper-voices/tree/v1.0.0/en/en_US/lessac/medium
#   The model is .gitignored so each developer downloads their own copy.
#
# See tools/README.md for full setup.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PHRASES_FILE="${REPO_ROOT}/tools/voice_phrases.txt"
VOICE_DIR="${REPO_ROOT}/assets/sounds/voice"
MODEL="${REPO_ROOT}/tools/piper-voices/en_US-lessac-medium.onnx"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force|-f) FORCE=1 ;;
        -h|--help)
            grep -E '^# ' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# ── Tool checks ──────────────────────────────────────────────────────────────
for cmd in piper sox; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: '$cmd' not found in PATH. See tools/README.md for setup." >&2
        exit 1
    fi
done
if [[ ! -f "$MODEL" ]]; then
    echo "ERROR: piper model not found at $MODEL" >&2
    echo "Download en_US-lessac-medium.onnx + .json from"  >&2
    echo "  https://huggingface.co/rhasspy/piper-voices/tree/v1.0.0/en/en_US/lessac/medium"  >&2
    echo "into tools/piper-voices/." >&2
    exit 1
fi
if [[ ! -f "$PHRASES_FILE" ]]; then
    echo "ERROR: phrases file missing: $PHRASES_FILE" >&2
    exit 1
fi
mkdir -p "$VOICE_DIR"

# ── Generate ─────────────────────────────────────────────────────────────────
written=0
skipped=0
while IFS='|' read -r raw_name raw_phrase; do
    # Trim whitespace.
    name="$(echo "$raw_name" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    phrase="$(echo "${raw_phrase:-}" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

    # Skip blanks + comments.
    [[ -z "$name" || "$name" == \#* ]] && continue
    if [[ -z "$phrase" ]]; then
        echo "WARN: empty phrase for $name; skipping" >&2
        continue
    fi

    out="${VOICE_DIR}/${name}.wav"
    if [[ -f "$out" && "$FORCE" -ne 1 ]]; then
        printf '[skip]  %s (use --force)\n' "$out"
        skipped=$((skipped + 1))
        continue
    fi

    raw="$(mktemp --suffix=.wav)"
    trap 'rm -f "$raw"' EXIT

    # piper writes its output WAV to --output_file when stdin is the phrase.
    echo "$phrase" | piper --model "$MODEL" --output_file "$raw" >/dev/null 2>&1

    # sox: trim leading silence (>2% threshold), trim trailing silence,
    # resample to 22.05 kHz mono 16-bit, normalise peak to -3 dBFS.
    sox "$raw" -r 22050 -c 1 -b 16 "$out" \
        silence 1 0.05 2% reverse silence 1 0.05 2% reverse \
        norm -3

    rm -f "$raw"
    trap - EXIT

    printf '[write] %s  ("%s")\n' "$out" "$phrase"
    written=$((written + 1))
done < "$PHRASES_FILE"

printf '\nDone: %d written, %d skipped.\n' "$written" "$skipped"
