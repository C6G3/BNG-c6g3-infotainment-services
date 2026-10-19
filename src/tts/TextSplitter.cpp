#include "tts/TextSplitter.hpp"

#include <cstddef>

namespace ifs {

namespace {

// A clause mark only cuts when the piece before it and the rest of its sentence are both at least this
// long, so a cut never leaves a fragment too short to be spoken naturally.
constexpr std::size_t MIN_PIECE_CHARS = 40;
constexpr std::size_t MIN_REST_CHARS  = 25;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isClauseMark(char c) {
    return c == ',' || c == ';' || c == ':';
}

// '.', '!' or '?' followed by whitespace or the end of the text, or a line break.
bool endsSentenceAt(const std::string& text, std::size_t i) {
    char c = text[i];
    if (c == '\n') return true;
    if (c != '.' && c != '!' && c != '?') return false;
    return i + 1 == text.size() || isSpace(text[i + 1]);
}

// Characters from `from` up to the end of the sentence that contains it.
std::size_t restOfSentence(const std::string& text, std::size_t from) {
    std::size_t i = from;
    while (i < text.size() && !endsSentenceAt(text, i)) ++i;
    return i - from;
}

std::string trimmed(const std::string& text, std::size_t begin, std::size_t end) {
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

} // anonymous namespace

std::vector<std::string> splitAtPauses(const std::string& text) {
    static const std::string ELLIPSIS = "…";  // U+2026 in UTF-8
    std::vector<std::string> segments;
    std::size_t start = 0;
    std::size_t i = 0;

    auto push = [&](std::size_t end) {
        auto seg = trimmed(text, start, end);
        if (seg.empty()) return;
        char last = seg.back();
        if (last != '.' && last != ',' && last != '!' && last != '?' && last != ';' && last != ':') seg += ',';
        segments.push_back(std::move(seg));
    };

    while (i < text.size()) {
        std::size_t markLen = 0;
        if (text.compare(i, 3, "...") == 0) {
            markLen = 3;
            while (i + markLen < text.size() && text[i + markLen] == '.') ++markLen;
        } else if (text.compare(i, ELLIPSIS.size(), ELLIPSIS) == 0) {
            markLen = ELLIPSIS.size();
        }
        if (markLen == 0) {
            ++i;
            continue;
        }
        push(i);
        i += markLen;
        start = i;
    }

    auto last = trimmed(text, start, text.size());
    if (!last.empty()) segments.push_back(std::move(last));
    return segments;
}

std::vector<std::string> splitForSpeech(const std::string& text) {
    std::vector<std::string> pieces;
    std::size_t pieceStart  = 0;
    std::size_t clauseStart = 0;  // start of the current sentence, or of the text after the last cut in it

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (endsSentenceAt(text, i)) {
            clauseStart = i + 1;
            continue;
        }
        // Cut after "word, " only: "1,000" and "10:30" have no space after the mark
        if (!isClauseMark(text[i]) || i + 1 >= text.size() || !isSpace(text[i + 1]))
            continue;

        if (i + 1 - clauseStart < MIN_PIECE_CHARS || restOfSentence(text, i + 1) < MIN_REST_CHARS)
            continue;

        auto piece = trimmed(text, pieceStart, i + 1);
        if (!piece.empty()) pieces.push_back(std::move(piece));
        pieceStart  = i + 1;
        clauseStart = i + 1;
    }

    auto last = trimmed(text, pieceStart, text.size());
    if (!last.empty()) pieces.push_back(std::move(last));
    return pieces;
}

} // namespace ifs
