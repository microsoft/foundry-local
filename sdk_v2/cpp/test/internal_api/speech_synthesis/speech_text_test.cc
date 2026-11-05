// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/speech_synthesis/speech_text.h"
#include "util/string_utils.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace fl {
namespace {

using Chunks = std::vector<std::string>;

TEST(SpeechText, SplitsSentencesAndLines) {
  EXPECT_EQ(SplitSpeechText("One sentence here. Another one there!\r\nA new line? Yes.", 0, 400),
            (Chunks{"One sentence here.", "Another one there!", "A new line?", "Yes."}));
  EXPECT_EQ(SplitSpeechText("Wait... What?! \"Quoted.\" (Bracket.) Done\xE2\x80\xA6 End", 0, 400),
            (Chunks{"Wait...", "What?!", "\"Quoted.\"", "(Bracket.)", "Done\xE2\x80\xA6", "End"}));
  EXPECT_TRUE(SplitSpeechText(" \n\t\n", 0, 400).empty());
}

TEST(SpeechText, KeepsAbbreviationsDecimalsAndLowerCaseContinuations) {
  EXPECT_EQ(SplitSpeechText("Dr. Smith paid 3.50 dollars, e.g. a coffee. J. R. R. Tolkien wrote it. Then approx. ten.",
                            0, 400),
            (Chunks{"Dr. Smith paid 3.50 dollars, e.g. a coffee.", "J. R. R. Tolkien wrote it.", "Then approx. ten."}));
}

TEST(SpeechText, JoinsShortSentences) {
  EXPECT_EQ(SplitSpeechText("Hi. OK. This sentence is long enough. Bye.", 20, 400),
            (Chunks{"Hi. OK. This sentence is long enough. Bye."}));
  EXPECT_EQ(SplitSpeechText("This sentence is long enough. And this one too, really. Bye.", 20, 400),
            (Chunks{"This sentence is long enough.", "And this one too, really. Bye."}));
  // A short line is not joined with the next line.
  EXPECT_EQ(SplitSpeechText("Hi.\nThis sentence is long enough.", 20, 400),
            (Chunks{"Hi.", "This sentence is long enough."}));
  // A short last sentence that does not fit stays on its own.
  EXPECT_EQ(SplitSpeechText("This sentence is long enough. Bye.", 20, 32),
            (Chunks{"This sentence is long enough.", "Bye."}));
}

TEST(SpeechText, SplitsLongChunksAtClausesThenWordsThenCharacters) {
  EXPECT_EQ(SplitSpeechText("alpha beta, gamma delta epsilon", 0, 16),
            (Chunks{"alpha beta,", "gamma delta", "epsilon"}));
  EXPECT_EQ(SplitSpeechText("abcdefghij", 0, 4), (Chunks{"abcd", "efgh", "ij"}));
  // Lengths count code points, and a cut never splits a UTF-8 sequence.
  EXPECT_EQ(SplitSpeechText("\xC3\xA9\xC3\xA9\xC3\xA9", 0, 2), (Chunks{"\xC3\xA9\xC3\xA9", "\xC3\xA9"}));
}

TEST(SpeechText, CountsCodePoints) {
  EXPECT_EQ(CountCodePoints("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"), 4u);  // a é € 😀
  EXPECT_EQ(CountCodePoints(""), 0u);
  // Continuation bytes alone start no code point, so callers check IsValidUtf8 first.
  EXPECT_EQ(CountCodePoints("\x80\x80"), 0u);
}

}  // namespace
}  // namespace fl
