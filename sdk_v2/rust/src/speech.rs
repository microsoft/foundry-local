//! Text to speech: [`SpeechSynthesisSession`] and its audio and voice types.
//!
//! Speech synthesis runs on models with task `"text-to-speech"` (for example
//! VibeVoice-Realtime-0.5B). The session takes text and returns mono 16-bit PCM
//! audio at 24 kHz. [`SpeechSynthesisSession::synthesize_streaming`] yields one
//! chunk per sentence so that playback can start before the whole text is
//! synthesized.

use std::ops::Deref;
use std::path::Path;
use std::pin::Pin;
use std::task::{Context, Poll};

use serde::Deserialize;

use crate::detail::ffi::{FOUNDRY_LOCAL_PARAM_SPEED, FOUNDRY_LOCAL_PARAM_VOICE};
use crate::detail::model::Model;
use crate::error::{FoundryLocalError, Result};
use crate::item::{Item, MediaSource};
use crate::request::{Request, RequestOptions};
use crate::response::Response;
use crate::session::{validate_session_task, ItemStream, Session};

/// The model task served by [`SpeechSynthesisSession`].
pub const TEXT_TO_SPEECH_TASK: &str = "text-to-speech";

/// Options for one synthesis call. Unset fields use the model package defaults.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SpeechOptions {
    /// Voice name, e.g. `"Emma"`. See [`Model::speech_voices`].
    pub voice: Option<String>,
    /// Speaking rate. Default `1.0`. VibeVoice supports only `1.0` and rejects other values.
    pub speed: Option<f32>,
}

impl SpeechOptions {
    /// Default options.
    pub fn new() -> Self {
        Self::default()
    }

    /// Select a voice (builder-style).
    pub fn voice(mut self, voice: impl Into<String>) -> Self {
        self.voice = Some(voice.into());
        self
    }

    /// Set the speaking rate (builder-style).
    pub fn speed(mut self, speed: f32) -> Self {
        self.speed = Some(speed);
        self
    }

    fn to_request_options(&self) -> RequestOptions {
        let voice = self
            .voice
            .clone()
            .map(|v| (FOUNDRY_LOCAL_PARAM_VOICE.to_string(), v));
        let speed = self
            .speed
            .map(|s| (FOUNDRY_LOCAL_PARAM_SPEED.to_string(), s.to_string()));
        RequestOptions {
            additional_options: voice.into_iter().chain(speed).collect(),
            ..RequestOptions::default()
        }
    }
}

/// Synthesized speech: mono signed 16-bit little-endian PCM.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpeechAudio {
    /// The PCM bytes, two per sample.
    pub data: Vec<u8>,
    /// Sample rate in Hz (24000 for VibeVoice).
    pub sample_rate: u32,
}

impl SpeechAudio {
    /// Convert a PCM [`Item::Audio`] produced by a speech-synthesis session.
    pub fn from_item(item: &Item) -> Option<SpeechAudio> {
        let Item::Audio(audio) = item else {
            return None;
        };
        let MediaSource::Data(data) = &audio.source else {
            return None;
        };
        (audio.format.as_deref() == Some("pcm")).then(|| SpeechAudio {
            data: data.clone(),
            sample_rate: u32::try_from(audio.sample_rate).unwrap_or(0),
        })
    }

    /// The samples.
    pub fn samples(&self) -> Vec<i16> {
        self.data
            .chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]))
            .collect()
    }

    /// Encode as a mono 16-bit PCM WAV file.
    pub fn to_wav(&self) -> Vec<u8> {
        let data_len = (self.data.len() & !1) as u32;
        let mut out = Vec::with_capacity(44 + data_len as usize);
        out.extend_from_slice(b"RIFF");
        out.extend_from_slice(&(36 + data_len).to_le_bytes());
        out.extend_from_slice(b"WAVEfmt ");
        out.extend_from_slice(&16u32.to_le_bytes());
        out.extend_from_slice(&1u16.to_le_bytes()); // PCM
        out.extend_from_slice(&1u16.to_le_bytes()); // mono
        out.extend_from_slice(&self.sample_rate.to_le_bytes());
        out.extend_from_slice(&(self.sample_rate * 2).to_le_bytes());
        out.extend_from_slice(&2u16.to_le_bytes());
        out.extend_from_slice(&16u16.to_le_bytes());
        out.extend_from_slice(b"data");
        out.extend_from_slice(&data_len.to_le_bytes());
        out.extend_from_slice(&self.data[..data_len as usize]);
        out
    }
}

/// A voice of a speech-synthesis model package.
#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
pub struct SpeechVoice {
    /// Voice name, used as [`SpeechOptions::voice`].
    pub name: String,
    /// BCP 47 language tag, for example `"en"` or `"en-in"`.
    pub language: String,
    /// `"female"` or `"male"`, when the package declares it.
    #[serde(default)]
    pub gender: Option<String>,
}

#[derive(Deserialize)]
struct GenAIConfig {
    model: SpeechModelConfig,
}

#[derive(Deserialize)]
struct SpeechModelConfig {
    #[serde(default)]
    default_voice: Option<String>,
    #[serde(default)]
    voices: Vec<SpeechVoice>,
}

/// The voices in `model.voices` of a `genai_config.json`, default voice (`model.default_voice`) first.
fn parse_speech_voices(genai_config: &str) -> Result<Vec<SpeechVoice>> {
    let GenAIConfig {
        model: SpeechModelConfig {
            default_voice,
            mut voices,
        },
    } = serde_json::from_str(genai_config)?;
    if voices.is_empty() {
        return Err(FoundryLocalError::ModelOperation {
            reason:
                "genai_config.json has no model.voices; the model is not a speech-synthesis package"
                    .to_string(),
        });
    }
    if let Some(index) = default_voice.and_then(|d| voices.iter().position(|v| v.name == d)) {
        let voice = voices.remove(index);
        voices.insert(0, voice);
    }
    Ok(voices)
}

/// The voices in the `genai_config.json` of a speech-synthesis package, default voice first.
pub(crate) fn read_speech_voices(package_dir: &Path) -> Result<Vec<SpeechVoice>> {
    let path = package_dir.join("genai_config.json");
    let text = std::fs::read_to_string(&path).map_err(|e| FoundryLocalError::ModelOperation {
        reason: format!("cannot read {}: {e}", path.display()),
    })?;
    parse_speech_voices(&text)
}

/// The complete audio of a speech-synthesis response.
fn response_audio(response: &Response) -> Result<SpeechAudio> {
    response
        .items
        .iter()
        .find_map(SpeechAudio::from_item)
        .ok_or_else(|| FoundryLocalError::Validation {
            reason: "speech synthesis response contained no audio".to_string(),
        })
}

impl Model {
    /// The voices of a downloaded (cached) `"text-to-speech"` model, default voice first.
    ///
    /// Reads `model.voices` in the `genai_config.json` of the package, so the model does not need to be loaded.
    /// Returns an error when the model is not cached or is not a speech-synthesis package.
    pub async fn speech_voices(&self) -> Result<Vec<SpeechVoice>> {
        validate_session_task(self, "speech_voices", &[TEXT_TO_SPEECH_TASK])?;
        let path = self.path().await?;
        crate::detail::task::spawn_blocking(move || read_speech_voices(&path)).await
    }
}

/// A text-to-speech [`Session`] for models with task `"text-to-speech"`.
///
/// Stateless and safe to share across tasks. Dereferences to [`Session`] for
/// the generic request API (send [`Item::text`] items; the response holds one
/// [`Item::Audio`]).
#[derive(Clone)]
pub struct SpeechSynthesisSession {
    session: Session,
}

impl SpeechSynthesisSession {
    /// Open a speech-synthesis session on a loaded model.
    ///
    /// Returns [`FoundryLocalError::Validation`] if the model's task is not `"text-to-speech"`.
    pub async fn new(model: &Model) -> Result<SpeechSynthesisSession> {
        validate_session_task(model, "SpeechSynthesisSession", &[TEXT_TO_SPEECH_TASK])?;
        Ok(SpeechSynthesisSession {
            session: Session::new(model).await?,
        })
    }

    /// Synthesize `text` and return the complete audio.
    pub async fn synthesize(
        &self,
        text: impl Into<String>,
        options: SpeechOptions,
    ) -> Result<SpeechAudio> {
        let request = Request::from_items(vec![Item::text(text.into())])
            .with_options(options.to_request_options());
        let response = self.session.process_request(request).await?;
        response_audio(&response)
    }

    /// Synthesize `text`, yielding one self-contained [`SpeechAudio`] chunk per sentence.
    /// A sentence shorter than 24 characters is joined with the next one, and a sentence longer
    /// than 400 characters gives one chunk per part.
    ///
    /// Dropping the stream cancels synthesis. Call [`SpeechStream::complete_audio`]
    /// to get the complete audio after (or instead of) draining the chunks.
    pub fn synthesize_streaming(
        &self,
        text: impl Into<String>,
        options: SpeechOptions,
    ) -> SpeechStream {
        let request = Request::from_items(vec![Item::text(text.into())])
            .with_options(options.to_request_options());
        SpeechStream {
            inner: self.session.process_streaming_request(request),
        }
    }

    /// Consume this handle, yielding the underlying base [`Session`].
    pub fn into_session(self) -> Session {
        self.session
    }
}

impl Deref for SpeechSynthesisSession {
    type Target = Session;

    fn deref(&self) -> &Session {
        &self.session
    }
}

/// Per-sentence audio chunks from [`SpeechSynthesisSession::synthesize_streaming`].
///
/// Implements [`futures_core::Stream`] of `Result<SpeechAudio>`.
pub struct SpeechStream {
    inner: ItemStream,
}

impl SpeechStream {
    /// Wait for synthesis to finish and return the complete audio.
    pub async fn complete_audio(&mut self) -> Result<SpeechAudio> {
        let response = self.inner.response().await?;
        response_audio(&response)
    }
}

impl futures_core::Stream for SpeechStream {
    type Item = Result<SpeechAudio>;

    fn poll_next(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        loop {
            match Pin::new(&mut self.inner).poll_next(cx) {
                Poll::Ready(Some(Ok(item))) => {
                    if let Some(audio) = SpeechAudio::from_item(&item) {
                        return Poll::Ready(Some(Ok(audio)));
                    }
                }
                Poll::Ready(Some(Err(error))) => return Poll::Ready(Some(Err(error))),
                Poll::Ready(None) => return Poll::Ready(None),
                Poll::Pending => return Poll::Pending,
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::item::Audio;

    #[test]
    fn options_map_to_native_keys() {
        let options = SpeechOptions::new().voice("Emma").speed(1.25);
        assert_eq!(
            options.to_request_options().additional_options,
            vec![
                ("voice".to_string(), "Emma".to_string()),
                ("speed".to_string(), "1.25".to_string())
            ]
        );
        assert!(SpeechOptions::default()
            .to_request_options()
            .additional_options
            .is_empty());
    }

    #[test]
    fn converts_pcm_items_and_encodes_wav() {
        let item = Item::Audio(Audio {
            source: MediaSource::Data(vec![1, 0, 0xfe, 0xff]),
            format: Some("pcm".into()),
            sample_rate: 24000,
            channels: 1,
        });
        let speech = SpeechAudio::from_item(&item).expect("pcm audio");
        assert_eq!(speech.samples(), vec![1, -2]);
        let wav = speech.to_wav();
        assert_eq!(wav.len(), 48);
        assert_eq!(&wav[0..4], b"RIFF");
        assert_eq!(u32::from_le_bytes(wav[24..28].try_into().unwrap()), 24000);
        assert_eq!(&wav[44..], &[1, 0, 0xfe, 0xff]);
        assert!(SpeechAudio::from_item(&Item::text("x")).is_none());
    }

    #[test]
    fn reads_voices_from_genai_config_default_first() {
        let config = r#"{"model": {"type": "vibevoice", "default_voice": "Emma", "voices": [
            {"name": "Carter", "file": "voices/en-Carter_man.onnx", "language": "en", "gender": "male"},
            {"name": "Emma", "file": "voices/en-Emma_woman.onnx", "language": "en", "gender": "female"},
            {"name": "Samuel", "file": "voices/in-Samuel_man.onnx", "language": "en-in"}]}}"#;
        let voices = parse_speech_voices(config).expect("voices");
        let names: Vec<_> = voices.iter().map(|v| v.name.as_str()).collect();
        assert_eq!(names, ["Emma", "Carter", "Samuel"]);
        assert_eq!(voices[0].gender.as_deref(), Some("female"));
        assert_eq!(voices[2].language, "en-in");
        assert!(voices[2].gender.is_none());

        assert!(parse_speech_voices(r#"{"model": {"type": "qwen2"}}"#).is_err());
        assert!(parse_speech_voices("{}").is_err());
    }
}
