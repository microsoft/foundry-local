// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/speech_synthesis/speech_text.h"

#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace fl {

namespace {

constexpr std::string_view kSpace = " \t\f\v";

// Words that end with a period but rarely end a sentence.
constexpr std::array<std::string_view, 22> kAbbreviations = {
    "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc", "e.g", "i.e",
    "no", "fig", "inc", "ltd", "co", "mt", "approx", "dept", "est", "gen"};

bool IsSpace(char c) {
  return kSpace.find(c) != std::string_view::npos;
}

std::string_view Trim(std::string_view text) {
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }

  return text.substr(first, text.find_last_not_of(kSpace) + 1 - first);
}

/// The length of the sentence-ending mark at `at` ("…" is three bytes), or 0.
size_t EndMark(std::string_view text, size_t at) {
  if (text[at] == '.' || text[at] == '!' || text[at] == '?') {
    return 1;
  }

  return text.substr(at, 3) == "\xE2\x80\xA6" ? 3 : 0;
}

/// The length of a closing quote or bracket at `at`, or 0.
size_t Closer(std::string_view text, size_t at) {
  if (std::string_view(")]}\"'").find(text[at]) != std::string_view::npos) {
    return 1;
  }

  for (std::string_view closer : {"\xE2\x80\x9D", "\xE2\x80\x99", "\xC2\xBB"}) {  // ” ’ »
    if (text.substr(at, closer.size()) == closer) {
      return closer.size();
    }
  }

  return 0;
}

/// True when the period at `at` follows an abbreviation or a single letter.
bool FollowsAbbreviation(std::string_view text, size_t at) {
  size_t start = at;
  while (start > 0 && !IsSpace(text[start - 1]) && text[start - 1] != '(' && text[start - 1] != '"') {
    --start;
  }

  std::string word(text.substr(start, at - start));
  std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::tolower(c); });
  if (word.size() == 1 && std::isalpha(static_cast<unsigned char>(word[0]))) {
    return true;
  }

  return std::find(kAbbreviations.begin(), kAbbreviations.end(), word) != kAbbreviations.end();
}

/// The sentences of one line.
std::vector<std::string_view> Sentences(std::string_view line) {
  std::vector<std::string_view> sentences;
  size_t begin = 0;
  for (size_t at = 0; at < line.size();) {
    const auto mark = EndMark(line, at);
    if (mark == 0) {
      ++at;
      continue;
    }

    const bool single_period = line[at] == '.' && (at + 1 >= line.size() || line[at + 1] != '.');
    const auto period_at = at;
    auto end = at + mark;
    while (end < line.size() && EndMark(line, end) > 0) {
      end += EndMark(line, end);
    }

    while (end < line.size() && Closer(line, end) > 0) {
      end += Closer(line, end);
    }

    auto next = end;
    while (next < line.size() && IsSpace(line[next])) {
      ++next;
    }

    const bool at_space = end == line.size() || IsSpace(line[end]);
    const bool lower_next = next < line.size() && std::islower(static_cast<unsigned char>(line[next]));
    if (at_space && !lower_next && !(single_period && FollowsAbbreviation(line, period_at))) {
      sentences.push_back(line.substr(begin, end - begin));
      begin = end;
    }

    at = end;
  }

  sentences.push_back(line.substr(begin));
  return sentences;
}

/// The byte offset after the first `count` code points of `text`.
size_t PrefixBytes(std::string_view text, size_t count) {
  size_t at = 0;
  for (size_t seen = 0; at < text.size(); ++at) {
    if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80 && seen++ == count) {
      break;
    }
  }

  return at;
}

void AppendSplit(std::string_view chunk, size_t max_characters, std::vector<std::string>& chunks) {
  chunk = Trim(chunk);
  while (CountCodePoints(chunk) > max_characters) {
    const auto limit = PrefixBytes(chunk, max_characters);
    const auto head = chunk.substr(0, limit);
    size_t cut = std::string_view::npos;
    for (const auto* marks : {",;:", " \t"}) {
      const auto found = head.find_last_of(marks);
      if (found != std::string_view::npos && found > 0) {
        cut = found + 1;
        break;
      }
    }

    cut = cut == std::string_view::npos ? limit : cut;
    if (const auto part = Trim(chunk.substr(0, cut)); !part.empty()) {
      chunks.emplace_back(part);
    }

    chunk = Trim(chunk.substr(cut));
  }

  if (!chunk.empty()) {
    chunks.emplace_back(chunk);
  }
}

}  // namespace

std::vector<std::string> SplitSpeechText(std::string_view text, size_t min_characters, size_t max_characters) {
  std::vector<std::string> chunks;
  max_characters = std::max<size_t>(max_characters, 1);
  for (size_t start = 0; start < text.size();) {
    const auto end = std::min(text.find_first_of("\r\n", start), text.size());
    const auto line_begin = chunks.size();
    std::string pending;
    for (const auto sentence : Sentences(text.substr(start, end - start))) {
      const auto trimmed = Trim(sentence);
      if (trimmed.empty()) {
        continue;
      }

      pending += (pending.empty() ? "" : " ") + std::string(trimmed);
      if (CountCodePoints(pending) >= min_characters) {
        AppendSplit(pending, max_characters, chunks);
        pending.clear();
      }
    }

    if (!pending.empty()) {
      if (chunks.size() > line_begin &&
          CountCodePoints(chunks.back()) + 1 + CountCodePoints(pending) <= max_characters) {
        chunks.back() += " " + pending;
      } else {
        AppendSplit(pending, max_characters, chunks);
      }
    }

    start = end + 1;
  }

  return chunks;
}

}  // namespace fl
