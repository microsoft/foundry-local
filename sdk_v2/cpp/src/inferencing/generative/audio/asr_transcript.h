#pragma once

#include "items/speech_result_item.h"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct OgaASRPostProcessorResult;

namespace fl {

class ASRTranscript {
 public:
  using SegmentCallback = std::function<void(const SpeechSegmentItem&)>;

  explicit ASRTranscript(std::string_view timestamp_level = "off", SegmentCallback callback = {});
  void Append(const OgaASRPostProcessorResult& result);
  void AppendText(std::string_view text);
  void AppendWord(std::string_view text, int64_t start_ms, int64_t end_ms);
  void AppendSegment(std::string_view text, int64_t start_ms, int64_t end_ms);
  std::unique_ptr<SpeechResultItem> Finish();
  const std::string& Text() const { return text_; }

 private:
  void Publish(std::unique_ptr<SpeechSegmentItem> segment);
  void PublishPartial();

  std::string timestamp_level_;
  SegmentCallback callback_;
  std::string text_;
  size_t completed_text_size_ = 0;
  std::deque<SpeechWord> pending_words_;
  std::vector<std::unique_ptr<SpeechSegmentItem>> segments_;
};

std::unique_ptr<SpeechSegmentItem> CopySpeechSegment(const SpeechSegmentItem& source);

}  // namespace fl