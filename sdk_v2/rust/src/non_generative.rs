//! Typed requests, results, and sessions for non-generative models.

use std::collections::BTreeMap;
use std::ops::Deref;

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::detail::model::Model;
use crate::error::{FoundryLocalError, Result};
use crate::{Item, Request, Session, TextKind};

fn default_rank_model() -> String {
    "clm".into()
}

fn default_decision_model() -> String {
    "kev".into()
}

fn default_temperature() -> f32 {
    1.0
}

/// Request accepted by `POST /v1/rank`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RankingRequest {
    /// Structured context used to compare the candidate answers.
    #[serde(default)]
    pub context: Value,
    /// The question whose answers should be ranked.
    #[serde(default)]
    pub question: String,
    /// Candidate answers.
    pub answers: Vec<String>,
    /// HTTP-contract model name. The native session remains bound to its model.
    #[serde(default = "default_rank_model")]
    pub model: String,
    /// Ranking temperature in `(0, 100]`.
    #[serde(default = "default_temperature")]
    pub temperature: f32,
}

impl RankingRequest {
    /// Create a request with the endpoint defaults (`model = "clm"`,
    /// `temperature = 1`).
    pub fn new(question: impl Into<String>, answers: Vec<String>) -> Self {
        Self {
            context: Value::Null,
            question: question.into(),
            answers,
            model: default_rank_model(),
            temperature: default_temperature(),
        }
    }
}

/// One ranked candidate returned by a ranking model.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RankedCandidate {
    /// One-based rank.
    pub rank: usize,
    /// Original candidate text.
    pub candidate: String,
    /// Model probability for this candidate.
    #[serde(rename = "prob")]
    pub probability: f64,
}

/// Result returned by `POST /v1/rank`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RankingResult {
    /// Resolved model identifier.
    pub model: String,
    /// Candidates ordered by rank.
    pub ranked: Vec<RankedCandidate>,
}

/// Supported typed-decision question kinds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DecisionQuestionType {
    /// A numeric no/unknown/likely probability.
    Noul,
    /// Select one of a set of choices.
    Choice,
    /// Produce a numeric score.
    Score,
}

/// A question in a [`DecisionRequest`].
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DecisionQuestion {
    /// Question kind.
    #[serde(rename = "type")]
    pub kind: DecisionQuestionType,
    /// Optional structured criteria.
    #[serde(default, skip_serializing_if = "Value::is_null")]
    pub criteria: Value,
    /// Optional structured instructions.
    #[serde(default, skip_serializing_if = "Value::is_null")]
    pub instructions: Value,
}

impl DecisionQuestion {
    /// Create a question of the given kind.
    pub fn new(kind: DecisionQuestionType) -> Self {
        Self {
            kind,
            criteria: Value::Null,
            instructions: Value::Null,
        }
    }
}

/// Request accepted by `POST /v1/systemone`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DecisionRequest {
    /// Structured state on which the decisions are based.
    #[serde(default)]
    pub state: Value,
    /// Questions keyed by caller-defined identifier.
    pub questions: BTreeMap<String, DecisionQuestion>,
    /// HTTP-contract model name. The native session remains bound to its model.
    #[serde(default = "default_decision_model")]
    pub model: String,
    /// Decision temperature in `(0, 100]`.
    #[serde(default = "default_temperature")]
    pub temperature: f32,
}

impl DecisionRequest {
    /// Create a request with the endpoint defaults (`model = "kev"`,
    /// `temperature = 1`).
    pub fn new(questions: BTreeMap<String, DecisionQuestion>) -> Self {
        Self {
            state: Value::Null,
            questions,
            model: default_decision_model(),
            temperature: default_temperature(),
        }
    }
}

/// A typed answer returned for one decision question.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DecisionAnswer {
    /// Answer kind.
    #[serde(rename = "type")]
    pub kind: DecisionQuestionType,
    /// Probability for a `noul` question.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub noul: Option<f64>,
    /// Selected value for a `choice` question.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub choice: Option<String>,
    /// Numeric result for a `score` question.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub score: Option<f64>,
    /// Model confidence, when supplied.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub confidence: Option<f64>,
    /// Choice probabilities keyed by choice.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub probabilities: BTreeMap<String, f64>,
    /// Model-provided labels keyed by encoded choice.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub legend: BTreeMap<String, String>,
}

/// Usage returned by a typed-decision request.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct DecisionUsage {
    /// Billing units consumed by local inference.
    pub billing_units: u64,
}

/// Result returned by `POST /v1/systemone`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct DecisionResult {
    /// Resolved model identifier.
    pub model: String,
    /// Answers keyed by request question identifier.
    pub answers: BTreeMap<String, DecisionAnswer>,
    /// Request usage.
    pub usage: DecisionUsage,
}

async fn process_json<T: Serialize, U: for<'de> Deserialize<'de>>(
    session: &Session,
    request: &T,
) -> Result<U> {
    let json = serde_json::to_string(request)?;
    let response = session
        .process_request(Request::from_items(vec![Item::Text {
            text: json,
            kind: TextKind::OpenAiJson,
        }]))
        .await?;
    match response.items.as_slice() {
        [Item::Text {
            text,
            kind: TextKind::OpenAiJson,
        }] => Ok(serde_json::from_str(text)?),
        _ => Err(FoundryLocalError::Internal {
            reason: "non-generative session returned an invalid JSON response".into(),
        }),
    }
}

/// Session for models whose task is `text-ranking`.
#[derive(Clone)]
pub struct RankingSession {
    session: Session,
}

impl RankingSession {
    /// Open a ranking session on a cached text-ranking model.
    pub async fn new(model: &Model) -> Result<Self> {
        super::session::validate_session_task(model, "RankingSession", &["text-ranking"])?;
        Ok(Self {
            session: Session::new(model).await?,
        })
    }

    /// Rank a typed request.
    pub async fn rank(&self, request: &RankingRequest) -> Result<RankingResult> {
        process_json(&self.session, request).await
    }

    /// Consume this handle, yielding the underlying generic session.
    pub fn into_session(self) -> Session {
        self.session
    }
}

impl Deref for RankingSession {
    type Target = Session;

    fn deref(&self) -> &Self::Target {
        &self.session
    }
}

/// Session for models whose task is `typed-decision`.
#[derive(Clone)]
pub struct DecisionSession {
    session: Session,
}

impl DecisionSession {
    /// Open a decision session on a cached typed-decision model.
    pub async fn new(model: &Model) -> Result<Self> {
        super::session::validate_session_task(model, "DecisionSession", &["typed-decision"])?;
        Ok(Self {
            session: Session::new(model).await?,
        })
    }

    /// Evaluate a typed-decision request.
    pub async fn decide(&self, request: &DecisionRequest) -> Result<DecisionResult> {
        process_json(&self.session, request).await
    }

    /// Consume this handle, yielding the underlying generic session.
    pub fn into_session(self) -> Session {
        self.session
    }
}

impl Deref for DecisionSession {
    type Target = Session;

    fn deref(&self) -> &Self::Target {
        &self.session
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ranking_contract_uses_endpoint_field_names() {
        let request = RankingRequest::new("best?", vec!["a".into(), "b".into()]);
        let json = serde_json::to_value(request).unwrap();
        assert_eq!(json["model"], "clm");
        assert_eq!(json["temperature"], 1.0);

        let result: RankingResult = serde_json::from_value(serde_json::json!({
            "model": "clm:1",
            "ranked": [{"rank": 1, "candidate": "b", "prob": 0.75}]
        }))
        .unwrap();
        assert_eq!(result.ranked[0].probability, 0.75);
    }

    #[test]
    fn decision_contract_round_trips_typed_answers() {
        let result: DecisionResult = serde_json::from_value(serde_json::json!({
            "model": "kev:1",
            "answers": {"go": {"type": "noul", "noul": 0.8}},
            "usage": {"billing_units": 0}
        }))
        .unwrap();
        assert_eq!(result.answers["go"].kind, DecisionQuestionType::Noul);
        assert_eq!(result.answers["go"].noul, Some(0.8));
    }
}
