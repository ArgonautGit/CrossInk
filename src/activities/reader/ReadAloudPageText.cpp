#include "ReadAloudPageText.h"

#include <Epub/Page.h>

#include <cctype>

namespace ReadAloudPageText {
namespace {

// A first word pushed right by text-indent marks a paragraph start in
// left-aligned and justified text. Centred and right-aligned lines start
// right of the margin anyway, so for them only the vertical gap counts.
constexpr int MIN_INDENT_PX = 2;
// Paragraph spacing shows up as a line pitch noticeably larger than the line
// height.
constexpr int PARAGRAPH_GAP_PERCENT = 140;

struct State {
  std::string* out = nullptr;
  size_t start = 0;  // where this page's text begins in *out
  bool hasPreviousLine = false;
  int previousY = 0;
  int previousLineHeight = 0;
  // The last word written ended in a hyphen at the end of its line, so the
  // next line's first word continues it.
  bool joinNextWord = false;
};

bool isSpaceLikeAt(const char* text, const size_t length, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < length && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;  // no-break space
    return true;
  }
  if (c == 0xE2 && index + 2 < length && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if ((c2 >= 0x80 && c2 <= 0x8B) || c2 == 0xAF) {
      advance = 3;  // en/em/thin/narrow no-break spaces and zero-width space
      return true;
    }
  }
  if (c < 0x20 || c == 0x7F) {
    advance = 1;  // control characters, including the 0x1F field separator
    return true;
  }
  return false;
}

// Appends the word's visible characters, folding inner spacing to a single
// space and trimming it at the ends. Returns false when nothing was written.
bool appendCleanWord(std::string& out, const char* text, const size_t length, const bool dropTrailingHyphen) {
  size_t end = length;
  if (dropTrailingHyphen && end > 0 && text[end - 1] == '-') end--;
  const size_t start = out.size();
  bool pendingSpace = false;
  for (size_t i = 0; i < end;) {
    size_t advance = 0;
    if (isSpaceLikeAt(text, end, i, advance)) {
      pendingSpace = out.size() > start;
      i += advance;
      continue;
    }
    if (pendingSpace) {
      out += ' ';
      pendingSpace = false;
    }
    out += text[i++];
  }
  return out.size() > start;
}

void appendSeparator(std::string& out, const size_t start, const char separator) {
  if (out.size() <= start || out.back() == '\n' || out.back() == separator) return;
  if (separator == '\n' && out.back() == ' ')
    out.back() = '\n';
  else
    out += separator;
}

bool visitLine(const PageTextLine& line, void* context) {
  auto& state = *static_cast<State*>(context);
  std::string& out = *state.out;
  const TextBlock* block = line.block;
  if (!block || !block->valid() || block->wordCount() == 0) return true;

  const BlockStyle& style = block->getBlockStyle();
  const bool indentAligned = style.alignment == CssTextAlign::Justify || style.alignment == CssTextAlign::Left;
  const bool indented = indentAligned && !style.isRtl && block->wordXpos(0) >= MIN_INDENT_PX;
  const bool gap = state.hasPreviousLine && state.previousLineHeight > 0 &&
                   (line.yPos - state.previousY) * 100 > state.previousLineHeight * PARAGRAPH_GAP_PERCENT;
  const bool paragraphStart = indented || gap || style.fromBrElement;

  for (uint16_t i = 0; i < block->wordCount(); ++i) {
    const char* text = block->wordText(i);
    const uint16_t length = block->wordTextLen(i);
    const bool lastOnLine = i + 1 == block->wordCount();
    const bool insertedHyphen = block->wordEndsWithInsertedHyphen(i);

    if (state.joinNextWord) {
      state.joinNextWord = false;
    } else if (i == 0) {
      appendSeparator(out, state.start, paragraphStart ? '\n' : ' ');
    } else if (block->wordHasSpaceBefore(i)) {
      appendSeparator(out, state.start, ' ');
    }

    if (!appendCleanWord(out, text, length, insertedHyphen)) continue;

    if (lastOnLine) {
      // A layout hyphen always joins; a hyphen from the text joins only when the
      // next line carries on the same compound.
      state.joinNextWord = insertedHyphen || (length > 1 && text[length - 1] == '-' &&
                                              std::isalpha(static_cast<unsigned char>(text[length - 2])));
    }
  }

  state.hasPreviousLine = true;
  state.previousY = line.yPos;
  state.previousLineHeight = line.lineHeight;
  return true;
}

}  // namespace

void append(const Page& page, std::string& out) {
  State state;
  state.out = &out;
  state.start = out.size();
  page.forEachTextLine(&visitLine, &state);
  while (out.size() > state.start && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
}

}  // namespace ReadAloudPageText
