#pragma once

#include <string>
#include <vector>

namespace ifs {

/// Cuts long sentences at clause marks (, ; :) so the first words of an announcement are ready sooner.
///
/// Kokoro synthesises a whole sentence before any of it can play, so a long sentence delays its first
/// word. Each returned piece is synthesised separately and streamed as soon as it is done. Short sentences
/// are never cut ("Hi, I'm Heart." stays whole) and sentence ends are left to sherpa-onnx's own splitter.
/// Pieces concatenated with single spaces give back the input with surrounding whitespace trimmed.
std::vector<std::string> splitForSpeech(const std::string& text);

/// Splits at pause marks ("..." or the ellipsis character) for an exact pause in between: the voice model
/// renders "..." as only ~60 ms, a comma ~90 ms. Segments are trimmed; one that would end without
/// punctuation gets a comma so it keeps a continuing intonation ("Ah..." is spoken as "Ah,").
/// Text without a pause mark comes back as one segment.
std::vector<std::string> splitAtPauses(const std::string& text);

} // namespace ifs
