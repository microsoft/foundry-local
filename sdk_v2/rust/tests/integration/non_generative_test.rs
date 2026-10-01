//! Opt-in native integration coverage for ranking and typed-decision sessions.

use std::collections::BTreeMap;
use std::sync::Arc;

use foundry_local_sdk::{
    DecisionQuestion, DecisionQuestionType, DecisionRequest, DecisionSession, Model,
    RankingRequest, RankingSession,
};
use serde_json::json;

use super::common;

async fn cached_model(task: &str, environment: &str) -> Option<Arc<Model>> {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    if let Ok(model_id) = std::env::var(environment) {
        let model = catalog.get_model_variant(&model_id).await.ok()?;
        if model.is_cached().await.ok()? {
            return Some(model);
        }
        return None;
    }
    let models = catalog.get_cached_models().await.ok()?;
    models.into_iter().find(|model| {
        model
            .info()
            .ok()
            .and_then(|info| info.task)
            .is_some_and(|value| value == task)
    })
}

#[tokio::test]
async fn ranking_session_round_trips_native_openai_json() {
    let Some(model) = cached_model("text-ranking", "FOUNDRY_TEST_CLM_MODEL").await else {
        eprintln!("Skipping ranking integration test: no cached CLM model available");
        return;
    };
    let session = RankingSession::new(&model)
        .await
        .expect("RankingSession::new failed");
    let mut request = RankingRequest::new(
        "Which activity is more suitable?",
        vec![
            "Have a picnic outdoors".into(),
            "Visit an indoor museum".into(),
        ],
    );
    request.context = json!({"weather": "heavy rain"});
    let result = session.rank(&request).await.expect("rank failed");

    assert_eq!(result.model, model.id());
    assert_eq!(result.ranked.len(), 2);
    assert_eq!(result.ranked[0].rank, 1);
}

#[tokio::test]
async fn decision_session_round_trips_native_openai_json() {
    let Some(model) = cached_model("typed-decision", "FOUNDRY_TEST_KEV_MODEL").await else {
        eprintln!("Skipping decision integration test: no cached KEV model available");
        return;
    };
    let mut questions = BTreeMap::new();
    let mut question = DecisionQuestion::new(DecisionQuestionType::Noul);
    question.instructions = json!("Should I take an umbrella?");
    questions.insert("umbrella".into(), question);
    let mut request = DecisionRequest::new(questions);
    request.state = json!({"weather": "heavy rain"});
    let session = DecisionSession::new(&model)
        .await
        .expect("DecisionSession::new failed");
    let result = session.decide(&request).await.expect("decide failed");

    assert_eq!(result.model, model.id());
    let answer = result
        .answers
        .get("umbrella")
        .expect("umbrella answer missing");
    assert_eq!(answer.kind, DecisionQuestionType::Noul);
    assert!(answer
        .noul
        .is_some_and(|value| (0.0..=1.0).contains(&value)));
}
