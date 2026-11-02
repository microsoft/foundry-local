// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Split text into the chunks that speech synthesis speaks one at a time. Each line is a paragraph. A sentence ends at
/// ".", "!", "?" or "…" (with closing quotes and brackets) before white space, but not after a common abbreviation or
/// a single letter ("Dr.", "e.g.", "J.") and not before a lower-case word. A sentence shorter than `min_characters`
/// is joined with the next sentence of its line, and a short last sentence with the previous one when it fits. A
/// chunk longer than `max_characters` is split at its last clause punctuation that fits, else between words, else
/// inside a word. Lengths count code points. Chunks are trimmed and never empty.
std::vector<std::string> SplitSpeechText(std::string_view text, size_t min_characters, size_t max_characters);

}  // namespace fl
