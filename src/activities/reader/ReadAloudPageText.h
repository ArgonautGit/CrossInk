#pragma once

#include <string>

class Page;

namespace ReadAloudPageText {

// Appends the words of a laid-out page as plain text for speech: paragraphs
// separated by '\n', words by single spaces, words hyphenated across a line
// break joined back together. Control characters (including the protocol's
// field separator) are dropped. Images contribute nothing.
void append(const Page& page, std::string& out);

}  // namespace ReadAloudPageText
