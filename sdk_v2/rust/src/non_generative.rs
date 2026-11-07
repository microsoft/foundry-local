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

    fn validate(&self) -> Result<()> {
        if self.model.is_empty() {
            return validation_error("ranking model must not be empty");
        }
        if self.answers.is_empty()
            || self
                .answers
                .iter()
                .any(|answer| answer.is_empty() || answer.contains('\0'))
        {
            return validation_error(
                "ranking answers must be non-empty strings without embedded NUL bytes",
            );
        }
        validate_structured_value(&self.context, "ranking context")?;
        if !self.temperature.is_finite() || self.temperature <= 0.0 || self.temperature > 100.0 {
            return validation_error("ranking temperature must be in (0, 100]");
        }
        Ok(())
    }
}

/// One ranked candidate returned by a ranking model.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RankedCandidate {
    /// One-based rank.
    pub rank: usize,
    /// String form of the zero-based candidate index.
    pub id: String,
    /// Zero-based index into [`RankingRequest::answers`].
    pub index: usize,
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

    fn validate(&self) -> Result<()> {
        if self.model.is_empty() {
            return validation_error("decision model must not be empty");
        }
        if self.questions.is_empty() {
            return validation_error("decision questions must not be empty");
        }
        validate_structured_value(&self.state, "decision state")?;
        if !self.temperature.is_finite() || self.temperature != 1.0 {
            return validation_error("decision temperature must be exactly 1.0");
        }

        for (id, question) in &self.questions {
            if id.contains('\0') {
                return validation_error(
                    "decision question IDs must not contain embedded NUL bytes",
                );
            }
            validate_structured_value(
                &question.criteria,
                &format!("decision question {id:?} criteria"),
            )?;
            validate_structured_value(
                &question.instructions,
                &format!("decision question {id:?} instructions"),
            )?;
            match question.kind {
                DecisionQuestionType::Noul => {}
                DecisionQuestionType::Choice => match &question.criteria {
                    Value::Object(values) if (1..=255).contains(&values.len()) => {}
                    _ => {
                        return validation_error(
                            "choice criteria must be an object with 1 to 255 entries",
                        );
                    }
                },
                DecisionQuestionType::Score => match &question.criteria {
                    Value::Array(values) if (1..=255).contains(&values.len()) => {}
                    _ => {
                        return validation_error(
                            "score criteria must be an array with 1 to 255 entries",
                        );
                    }
                },
            }
        }
        Ok(())
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

fn validation_error<T>(reason: impl Into<String>) -> Result<T> {
    Err(FoundryLocalError::Validation {
        reason: reason.into(),
    })
}

fn invalid_response<T>(reason: impl Into<String>) -> Result<T> {
    Err(FoundryLocalError::Internal {
        reason: format!(
            "non-generative session returned an invalid response: {}",
            reason.into()
        ),
    })
}

fn validate_structured_value(value: &Value, name: &str) -> Result<()> {
    match value {
        Value::Number(number) if number.as_u64().is_some_and(|value| value > i64::MAX as u64) => {
            validation_error(format!("{name} integers must fit in signed 64 bits"))
        }
        Value::Array(values) => {
            for value in values {
                validate_structured_value(value, name)?;
            }
            Ok(())
        }
        Value::Object(values) => {
            for value in values.values() {
                validate_structured_value(value, name)?;
            }
            Ok(())
        }
        _ => Ok(()),
    }
}

impl RankingResult {
    fn validate(&self, request: &RankingRequest) -> Result<()> {
        if self.model.is_empty() {
            return invalid_response("ranking model must not be empty");
        }
        if self.ranked.len() != request.answers.len() {
            return invalid_response("ranking result must contain every submitted answer");
        }

        let mut matched_indexes = vec![false; request.answers.len()];
        for (index, candidate) in self.ranked.iter().enumerate() {
            if candidate.rank != index + 1 {
                return invalid_response(
                    "ranking entries must be ordered with unique consecutive ranks",
                );
            }
            if !candidate.probability.is_finite() || !(0.0..=1.0).contains(&candidate.probability) {
                return invalid_response("ranking probabilities must be finite and in [0, 1]");
            }
            if candidate.index >= request.answers.len()
                || matched_indexes[candidate.index]
                || candidate.id != candidate.index.to_string()
                || candidate.candidate != request.answers[candidate.index]
            {
                return invalid_response(
                    "ranking IDs and indexes must be unique, in range, and match submitted answers",
                );
            }
            matched_indexes[candidate.index] = true;
        }
        Ok(())
    }
}

impl DecisionResult {
    fn validate(&self, request: &DecisionRequest) -> Result<()> {
        if self.model.is_empty() {
            return invalid_response("decision model must not be empty");
        }
        if self.answers.keys().ne(request.questions.keys()) {
            return invalid_response("decision answer keys must match request question keys");
        }

        for (id, question) in &request.questions {
            let answer = &self.answers[id];
            if answer.kind != question.kind {
                return invalid_response(format!(
                    "decision answer {id:?} type does not match its question"
                ));
            }
            if answer
                .confidence
                .is_some_and(|value| !value.is_finite() || !(0.0..=1.0).contains(&value))
                || answer
                    .probabilities
                    .values()
                    .any(|value| !value.is_finite() || !(0.0..=1.0).contains(value))
            {
                return invalid_response(
                    "decision confidence and probabilities must be finite and in [0, 1]",
                );
            }

            match question.kind {
                DecisionQuestionType::Noul => {
                    if !answer
                        .noul
                        .is_some_and(|value| value.is_finite() && (0.0..=1.0).contains(&value))
                        || answer.choice.is_some()
                        || answer.score.is_some()
                    {
                        return invalid_response(
                            "noul answers require only a finite noul value in [0, 1]",
                        );
                    }
                }
                DecisionQuestionType::Choice => {
                    let valid_choice = match (&question.criteria, &answer.choice) {
                        (Value::Object(criteria), Some(choice)) => criteria.contains_key(choice),
                        _ => false,
                    };
                    if !valid_choice || answer.noul.is_some() || answer.score.is_some() {
                        return invalid_response(
                            "choice answers require only a choice present in the request criteria",
                        );
                    }
                }
                DecisionQuestionType::Score => {
                    let maximum = match &question.criteria {
                        Value::Array(criteria) => criteria.len().saturating_sub(1) as f64,
                        _ => 0.0,
                    };
                    if !answer
                        .score
                        .is_some_and(|value| value.is_finite() && (0.0..=maximum).contains(&value))
                        || answer.noul.is_some()
                        || answer.choice.is_some()
                    {
                        return invalid_response(format!(
                            "score answers require only a finite score in [0, {maximum}]"
                        ));
                    }
                }
            }
        }
        Ok(())
    }
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
    /// Open a ranking session on a loaded text-ranking model.
    ///
    /// Clones share one underlying native session. Drop every clone before
    /// calling [`Model::unload`].
    pub async fn new(model: &Model) -> Result<Self> {
        super::session::validate_session_task(model, "RankingSession", &["text-ranking"])?;
        Ok(Self {
            session: Session::new(model).await?,
        })
    }

    /// Rank a typed request.
    ///
    /// Dropping this future after native submission cancels the in-flight
    /// request through [`Session::process_request`].
    pub async fn rank(&self, request: &RankingRequest) -> Result<RankingResult> {
        request.validate()?;
        let result: RankingResult = process_json(&self.session, request).await?;
        result.validate(request)?;
        Ok(result)
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
    /// Open a decision session on a loaded typed-decision model.
    ///
    /// Clones share one underlying native session. Drop every clone before
    /// calling [`Model::unload`].
    pub async fn new(model: &Model) -> Result<Self> {
        super::session::validate_session_task(model, "DecisionSession", &["typed-decision"])?;
        Ok(Self {
            session: Session::new(model).await?,
        })
    }

    /// Evaluate a typed-decision request.
    ///
    /// Dropping this future after native submission cancels the in-flight
    /// request through [`Session::process_request`].
    pub async fn decide(&self, request: &DecisionRequest) -> Result<DecisionResult> {
        request.validate()?;
        let result: DecisionResult = process_json(&self.session, request).await?;
        result.validate(request)?;
        Ok(result)
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

    fn assert_validation_error(result: Result<()>, expected: &str) {
        match result {
            Err(FoundryLocalError::Validation { reason }) => {
                assert!(
                    reason.contains(expected),
                    "{reason:?} did not contain {expected:?}"
                );
            }
            other => panic!("expected validation error, got {other:?}"),
        }
    }

    fn assert_invalid_response(result: Result<()>, expected: &str) {
        match result {
            Err(FoundryLocalError::Internal { reason }) => {
                assert!(
                    reason.contains(expected),
                    "{reason:?} did not contain {expected:?}"
                );
            }
            other => panic!("expected invalid response error, got {other:?}"),
        }
    }

    #[test]
    fn ranking_contract_uses_endpoint_field_names() {
        let request = RankingRequest::new("best?", vec!["a".into(), "b".into()]);
        let json = serde_json::to_value(request).unwrap();
        assert_eq!(json["model"], "clm");
        assert_eq!(json["temperature"], 1.0);

        let result: RankingResult = serde_json::from_value(serde_json::json!({
            "model": "clm:1",
            "ranked": [{"rank": 1, "id": "1", "index": 1, "candidate": "b", "prob": 0.75}]
        }))
        .unwrap();
        assert_eq!(result.ranked[0].probability, 0.75);
    }

    #[test]
    fn ranking_request_validates_answers_and_temperature_boundaries() {
        let mut request = RankingRequest::new("best?", vec!["a".into()]);
        request.temperature = 100.0;
        request.validate().unwrap();

        request.temperature = 0.0;
        assert_validation_error(request.validate(), "temperature");
        request.temperature = f32::NAN;
        assert_validation_error(request.validate(), "temperature");
        request.temperature = 1.0;
        request.answers = vec!["".into()];
        assert_validation_error(request.validate(), "answers");
        request.answers = vec!["a\0b".into()];
        assert_validation_error(request.validate(), "answers");
        request.answers.clear();
        assert_validation_error(request.validate(), "answers");
        request.answers.push("a".into());
        request.model.clear();
        assert_validation_error(request.validate(), "model");
    }

    #[test]
    fn ranking_result_rejects_malformed_order_candidates_and_probabilities() {
        let request = RankingRequest::new("best?", vec!["a".into(), "b".into()]);
        let mut result = RankingResult {
            model: "clm:1".into(),
            ranked: vec![
                RankedCandidate {
                    rank: 1,
                    id: "1".into(),
                    index: 1,
                    candidate: "b".into(),
                    probability: 0.75,
                },
                RankedCandidate {
                    rank: 2,
                    id: "0".into(),
                    index: 0,
                    candidate: "a".into(),
                    probability: 0.25,
                },
            ],
        };
        result.validate(&request).unwrap();

        result.ranked[1].rank = 1;
        assert_invalid_response(result.validate(&request), "unique consecutive ranks");
        result.ranked[1].rank = 2;
        result.ranked[1].index = 1;
        result.ranked[1].id = "1".into();
        result.ranked[1].candidate = "b".into();
        assert_invalid_response(result.validate(&request), "unique");
        result.ranked[1].index = 0;
        result.ranked[1].id = "0".into();
        result.ranked[1].candidate = "a".into();
        result.ranked[1].probability = f64::NAN;
        assert_invalid_response(result.validate(&request), "probabilities");
        result.ranked[1].probability = 1.01;
        assert_invalid_response(result.validate(&request), "probabilities");
        result.ranked.pop();
        assert_invalid_response(result.validate(&request), "every submitted answer");
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

    #[test]
    fn decision_request_validates_shapes_and_criteria_boundaries() {
        let mut questions = BTreeMap::new();
        let mut choice = DecisionQuestion::new(DecisionQuestionType::Choice);
        choice.criteria = Value::Object(
            (0..255)
                .map(|index| (index.to_string(), Value::String(index.to_string())))
                .collect(),
        );
        questions.insert("choice".into(), choice);
        let mut request = DecisionRequest::new(questions);
        request.validate().unwrap();

        let Value::Object(criteria) = &mut request.questions.get_mut("choice").unwrap().criteria
        else {
            unreachable!()
        };
        criteria.insert("overflow".into(), Value::Null);
        assert_validation_error(request.validate(), "1 to 255");
        request.questions.get_mut("choice").unwrap().criteria = Value::Array(vec![]);
        assert_validation_error(request.validate(), "choice criteria");

        request.questions.get_mut("choice").unwrap().kind = DecisionQuestionType::Score;
        request.questions.get_mut("choice").unwrap().criteria =
            Value::Array(vec![Value::Null; 255]);
        request.validate().unwrap();
        request.questions.get_mut("choice").unwrap().criteria =
            Value::Array(vec![Value::Null; 256]);
        assert_validation_error(request.validate(), "1 to 255");
        request.temperature = 2.0;
        assert_validation_error(request.validate(), "exactly 1.0");
        request.temperature = f32::INFINITY;
        assert_validation_error(request.validate(), "exactly 1.0");
    }

    #[test]
    fn decision_request_rejects_empty_questions_nul_ids_and_large_integers() {
        let mut request = DecisionRequest::new(BTreeMap::new());
        assert_validation_error(request.validate(), "questions");

        request.questions.insert(
            "bad\0id".into(),
            DecisionQuestion::new(DecisionQuestionType::Noul),
        );
        assert_validation_error(request.validate(), "NUL");
        request.questions.clear();
        request.questions.insert(
            "q".into(),
            DecisionQuestion {
                kind: DecisionQuestionType::Noul,
                criteria: serde_json::from_str("18446744073709551615").unwrap(),
                instructions: Value::Null,
            },
        );
        assert_validation_error(request.validate(), "signed 64 bits");
    }

    fn decision_fixture() -> (DecisionRequest, DecisionResult) {
        let mut questions = BTreeMap::new();
        let mut noul = DecisionQuestion::new(DecisionQuestionType::Noul);
        noul.instructions = Value::String("Is it valid?".into());
        questions.insert("valid".into(), noul);
        let mut choice = DecisionQuestion::new(DecisionQuestionType::Choice);
        choice.criteria = serde_json::json!({"inside": "Inside", "outside": "Outside"});
        questions.insert("activity".into(), choice);
        let mut score = DecisionQuestion::new(DecisionQuestionType::Score);
        score.criteria = serde_json::json!(["low", "medium", "high"]);
        questions.insert("urgency".into(), score);

        let mut answers = BTreeMap::new();
        answers.insert(
            "valid".into(),
            DecisionAnswer {
                kind: DecisionQuestionType::Noul,
                noul: Some(0.8),
                choice: None,
                score: None,
                confidence: Some(0.9),
                probabilities: BTreeMap::new(),
                legend: BTreeMap::new(),
            },
        );
        answers.insert(
            "activity".into(),
            DecisionAnswer {
                kind: DecisionQuestionType::Choice,
                noul: None,
                choice: Some("inside".into()),
                score: None,
                confidence: None,
                probabilities: BTreeMap::from([("inside".into(), 0.8), ("outside".into(), 0.2)]),
                legend: BTreeMap::new(),
            },
        );
        answers.insert(
            "urgency".into(),
            DecisionAnswer {
                kind: DecisionQuestionType::Score,
                noul: None,
                choice: None,
                score: Some(1.5),
                confidence: None,
                probabilities: BTreeMap::new(),
                legend: BTreeMap::new(),
            },
        );
        (
            DecisionRequest::new(questions),
            DecisionResult {
                model: "kev:1".into(),
                answers,
                usage: DecisionUsage { billing_units: 0 },
            },
        )
    }

    #[test]
    fn decision_result_validates_keys_discriminants_and_required_fields() {
        let (request, mut result) = decision_fixture();
        result.validate(&request).unwrap();

        let missing = result.answers.remove("valid").unwrap();
        assert_invalid_response(result.validate(&request), "answer keys");
        result.answers.insert("valid".into(), missing);
        result.answers.get_mut("valid").unwrap().kind = DecisionQuestionType::Score;
        assert_invalid_response(result.validate(&request), "does not match");
        result.answers.get_mut("valid").unwrap().kind = DecisionQuestionType::Noul;
        result.answers.get_mut("valid").unwrap().noul = None;
        assert_invalid_response(result.validate(&request), "noul answers");
        result.answers.get_mut("valid").unwrap().noul = Some(0.8);
        result.answers.get_mut("activity").unwrap().choice = Some("missing".into());
        assert_invalid_response(result.validate(&request), "request criteria");
    }

    #[test]
    fn decision_result_validates_numeric_ranges_and_exclusive_values() {
        let (request, mut result) = decision_fixture();
        result.answers.get_mut("valid").unwrap().noul = Some(-0.01);
        assert_invalid_response(result.validate(&request), "noul value");
        result.answers.get_mut("valid").unwrap().noul = Some(0.8);
        result.answers.get_mut("valid").unwrap().confidence = Some(f64::NAN);
        assert_invalid_response(result.validate(&request), "confidence");
        result.answers.get_mut("valid").unwrap().confidence = Some(0.9);
        result
            .answers
            .get_mut("activity")
            .unwrap()
            .probabilities
            .insert("inside".into(), 1.01);
        assert_invalid_response(result.validate(&request), "probabilities");
        result
            .answers
            .get_mut("activity")
            .unwrap()
            .probabilities
            .insert("inside".into(), 0.8);
        result.answers.get_mut("urgency").unwrap().score = Some(2.01);
        assert_invalid_response(result.validate(&request), "score answers");
        result.answers.get_mut("urgency").unwrap().score = Some(2.0);
        result.answers.get_mut("urgency").unwrap().noul = Some(0.1);
        assert_invalid_response(result.validate(&request), "score answers");
    }

    #[test]
    fn malformed_decision_discriminant_fails_deserialization() {
        let result = serde_json::from_value::<DecisionResult>(serde_json::json!({
            "model": "kev:1",
            "answers": {"q": {"type": "unknown", "score": 0.0}},
            "usage": {"billing_units": 0}
        }));
        assert!(result.is_err());
    }
}
