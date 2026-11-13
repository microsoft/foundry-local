#include "inferencing/generative/audio/asr_transcript.h"

#include "exception.h"

#include <ort_genai.h>
#include <type_traits>

namespace fl {

static_assert(std::is_same_v<decltype(std::declval<const OgaASRPostProcessorTimestampRecord&>().GetTimestampInterval()),
                             std::pair<int64_t, int64_t>>,
              "ASR timestamps require the GenAI integer-millisecond interval contract");

ASRTranscript::ASRTranscript(std::string_view timestamp_level, SegmentCallback callback)
    : timestamp_level_(timestamp_level), callback_(std::move(callback)) {}

void ASRTranscript::Append(const OgaASRPostProcessorResult& result) {
  AppendText(result.GetText());
  const auto* timestamps = result.GetTimestampResult();
  if (!timestamps) return;
  for (size_t index = 0; index < timestamps->GetWordCount(); ++index) {
    const auto* word = timestamps->GetTimestampRecord(OgaTimestampRecordKind_Words, index);
    const auto [start, end] = word->GetTimestampInterval();
    AppendWord(word->GetText(), start, end);
  }
  for (size_t index = 0; index < timestamps->GetSegmentCount(); ++index) {
    const auto* segment = timestamps->GetTimestampRecord(OgaTimestampRecordKind_Segments, index);
    const auto [start, end] = segment->GetTimestampInterval();
    AppendSegment(segment->GetText(), start, end);
  }
}

void ASRTranscript::AppendText(std::string_view text) {
  text_.append(text);
  if (timestamp_level_ == "off" && !text.empty()) {
    Publish(std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_NONE, std::string(text)));
  } else if (!text.empty()) {
    PublishPartial();
  }
}

void ASRTranscript::AppendWord(std::string_view text, int64_t start_ms, int64_t end_ms) {
  if (text.empty()) return;
  SpeechWord word{std::string(text), start_ms, end_ms, std::nullopt, {}};
  if (timestamp_level_ == "word") {
    auto segment = std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, std::string(text));
    segment->start_time_ms = start_ms;
    segment->end_time_ms = end_ms;
    segment->words.push_back(std::move(word));
    Publish(std::move(segment));
  } else if (timestamp_level_ == "all") {
    pending_words_.push_back(std::move(word));
  }
}

void ASRTranscript::AppendSegment(std::string_view text, int64_t start_ms, int64_t end_ms) {
  if (text.empty()) return;
  auto segment = std::make_unique<SpeechSegmentItem>(FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL, std::string(text));
  segment->start_time_ms = start_ms;
  segment->end_time_ms = end_ms;
  if (timestamp_level_ == "all") {
    size_t matched = 0;
    while (matched < text.size() && !pending_words_.empty()) {
      auto& word = pending_words_.front();
      if (text.substr(matched, word.text.size()) != word.text) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "ASR segment text does not match completed words");
      }
      matched += word.text.size();
      segment->words.push_back(std::move(word));
      pending_words_.pop_front();
    }
    if (matched != text.size()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "ASR segment is missing completed words");
    }
  }
  Publish(std::move(segment));
}

void ASRTranscript::Publish(std::unique_ptr<SpeechSegmentItem> segment) {
  const bool completed = segment->kind == FOUNDRY_LOCAL_SPEECH_SEGMENT_FINAL;
  if (completed) {
    if (text_.compare(completed_text_size_, segment->text.size(), segment->text) != 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "ASR completed segment does not match pending transcript text");
    }
    completed_text_size_ += segment->text.size();
  }
  segment->Finalize();
  if (callback_) callback_(*segment);
  segments_.push_back(std::move(segment));
  if (completed) PublishPartial();
}

void ASRTranscript::PublishPartial() {
  if (!callback_ || completed_text_size_ == text_.size()) return;
  SpeechSegmentItem segment(FOUNDRY_LOCAL_SPEECH_SEGMENT_PARTIAL, text_.substr(completed_text_size_));
  segment.Finalize();
  callback_(segment);
}

std::unique_ptr<SpeechResultItem> ASRTranscript::Finish() {
  auto result = std::make_unique<SpeechResultItem>(std::move(text_));
  result->segments = std::move(segments_);
  result->Finalize();
  return result;
}

std::unique_ptr<SpeechSegmentItem> CopySpeechSegment(const SpeechSegmentItem& source) {
  auto segment = std::make_unique<SpeechSegmentItem>(source.kind, source.text);
  segment->start_time_ms = source.start_time_ms;
  segment->end_time_ms = source.end_time_ms;
  segment->utterance_start = source.utterance_start;
  segment->words = source.words;
  segment->language = source.language;
  segment->Finalize();
  return segment;
}

}  // namespace fl