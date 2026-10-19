# Voices

Each `.bin` file here is one Kokoro voice. The infotainment's Settings > Voice Assistant lists them, with
a test button for each one.

- **Format:** raw float32 style tensor, shape (510, 1, 256), no header, exactly 522,240 bytes. These are
  the same files Kokoro-82M ships as `voices/<name>.bin` (onnx-community format). Files of any other
  size are skipped with a warning in the service log.
- **Name:** `<language><gender>_<name>.bin`. The first letter sets pronunciation:
  `a` American English, `b` British English, `e` Spanish, `f` French, `h` Hindi, `i` Italian,
  `j` Japanese, `p` Brazilian Portuguese, `z` Chinese. The second letter is `f` or `m`.
- **Adding or removing voices:** drop files in or delete them. The service notices on the next request
  and reloads the engine. The model has 54 slots, so at most 54 voices are loaded at a time (first 54 by name).

The 28 English voices here (`af_*`, `am_*`, `bf_*`, `bm_*`) come from Kokoro v1.0 (Apache-2.0).
