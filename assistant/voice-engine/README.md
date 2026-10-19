# Voice engine (Kokoro TTS model)

The assistant uses a single engine: **Kokoro v1.0, full precision**, in sherpa-onnx format.
The model weights are not in git. Developers download them; the release zip includes them.

## Setup

1. Download `kokoro-multi-lang-v1_0.tar.bz2` (350 MB) from
   <https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models>.
2. Extract it here, so that this folder contains `kokoro-multi-lang-v1_0/model.onnx`.

Expected files in `kokoro-multi-lang-v1_0/`: `model.onnx`, `voices.bin`, `tokens.txt`, `espeak-ng-data/`,
`lexicon-zh.txt` (plus the package's other lexicons and dictionaries).

The service reads the folder from `assistant.tts.engine_dir` in `config/config.default.json`. When files
are missing it says which ones, both in its log and in the infotainment's Assistant status.

`voices.bin` here only tells the service how many speaker slots the model has (54). The voices that are
actually used come from `../voices/`.

## Why not the int8 model

On the reference laptop (i5-1135G7, with BeamNG running), int8 was about 2.5 times slower than fp32.
It needed roughly 3.1 s of compute per second of speech; fp32 needs about 1.3 s.
