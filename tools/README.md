# tools/ — helper utilities

These tools are not part of the main app build. They run once on a developer's
machine to (re)generate assets that get committed back to the repo. End users
never run them.

Build with:

```bash
cmake -B build -S . -DIBKR_BUILD_TOOLS=ON
cmake --build build --target gen_tones -j$(nproc)
```

## gen_tones — alert-tone WAV generator

Synthesises the 11 alert-tone WAVs for `NotificationService` into
`assets/sounds/tones/`. Hand-rolled C++ (sine + ADSR + sweep + LFO + bell
synthesis). No runtime deps.

```bash
# Skip existing files (default):
./build/tools/gen_tones assets/sounds/tones/

# Overwrite all 11:
./build/tools/gen_tones --force assets/sounds/tones/
```

Output: 22.05 kHz 16-bit mono PCM, peak-normalised to ~-1.4 dBFS.

Each event's timbre is documented in the source file
(`tools/gen_tones.cpp`) and in `.claude/plans/notifications.md`.

## gen_voice.sh — voice-phrase WAV generator

Generates the 11 voice-phrase WAVs into `assets/sounds/voice/`. Uses
[piper](https://github.com/rhasspy/piper) (offline neural TTS) and
[sox](http://sox.sourceforge.net/) for normalisation/trim. The model is
**not** committed to the repo — each developer downloads it once.

### Prerequisites

```bash
# Linux (Debian/Ubuntu): piper-tts is NOT in apt on noble (24.04). Use the
# standalone release binary instead — sox is available via apt:
sudo apt install sox
mkdir -p ~/.local/share/piper ~/.local/bin
curl -sL https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz \
    | tar xz -C ~/.local/share/piper
ln -sf ~/.local/share/piper/piper/piper ~/.local/bin/piper
# (older Debian / Ubuntu jammy may have piper-tts in apt — try `apt-cache policy piper-tts`)

# macOS:
brew install piper sox

# Verify both are on PATH:
which piper sox
```

### Voice model

Download `en_US-lessac-medium.onnx` + `en_US-lessac-medium.onnx.json` from
[Hugging Face — rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices/tree/v1.0.0/en/en_US/lessac/medium)
into `tools/piper-voices/`. Both files are gitignored.

```bash
mkdir -p tools/piper-voices
cd tools/piper-voices
curl -LO https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx
curl -LO https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json
```

### Generating

```bash
# Skip existing voice WAVs:
./tools/gen_voice.sh

# Overwrite all 11:
./tools/gen_voice.sh --force
```

The phrase list lives in [`tools/voice_phrases.txt`](voice_phrases.txt) — edit
that to change wording. The format is `<event_name> | <phrase>`. Event names
must match `core::services::NotificationService::EventName(...)`.

### Swapping voices

Two easy ways to use a different voice without changing code:

1. **Different piper voice model** — download another `.onnx` from
   piper-voices, edit `MODEL=` in `tools/gen_voice.sh`, run with `--force`.
2. **Drop-in custom WAVs** — record your own `<event>.wav` files (any tool —
   Audacity, OBS, your phone), drop them into `assets/sounds/voice/`. The
   service plays whatever's there; it doesn't care how the WAVs were made.

Either way, files must be 22.05 kHz mono 16-bit PCM (sox can convert: `sox in
-r 22050 -c 1 -b 16 out.wav norm -3`).
