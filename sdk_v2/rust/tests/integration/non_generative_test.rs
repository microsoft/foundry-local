//! Opt-in native integration coverage for ranking and typed-decision sessions.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;

use foundry_local_sdk::{
    DecisionQuestion, DecisionQuestionType, DecisionRequest, DecisionSession, FoundryLocalError,
    Model, RankingRequest, RankingSession,
};
use serde_json::json;

use super::common;

async fn cached_model(task: &str, environment: &str) -> Option<Arc<Model>> {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    if let Ok(model_id) = std::env::var(environment) {
        let model = match catalog.get_model_variant(&model_id).await {
            Ok(model) => model,
            Err(_) => manager
                .local_catalog()
                .ok()?
                .get_model_variant(&model_id)
                .await
                .ok()?,
        };
        if model.is_cached().await.ok()? {
            return Some(model);
        }
        return None;
    }
    let models = catalog.get_cached_models().await.ok()?;
    if let Some(model) = models.into_iter().find(|model| {
        model
            .info()
            .ok()
            .and_then(|info| info.task)
            .is_some_and(|value| value == task)
    }) {
        return Some(model);
    }

    register_staged_model(task).await
}

async fn register_staged_model(task: &str) -> Option<Arc<Model>> {
    let (directory, model_id) = match task {
        "text-ranking" => (
            "clm-v0.1-8b-mixed-generic-cpu-1",
            "clm-v0.1-8b-mixed-generic-cpu:1",
        ),
        "typed-decision" => (
            "kev-0.8b-mixed-generic-cpu-1",
            "kev-0.8b-mixed-generic-cpu:1",
        ),
        _ => return None,
    };
    let root = std::env::var("FOUNDRY_TEST_DATA_DIR").ok()?;
    let package = Path::new(&root).join("Microsoft").join(directory);
    if !package.is_dir() {
        return None;
    }

    let manager = common::get_test_manager();
    let catalog = manager
        .local_catalog()
        .expect("get local catalog for staged predictive model");
    if let Ok(model) = catalog.get_model_variant(model_id).await {
        return Some(model);
    }

    let mut metadata = catalog
        .create_model_info()
        .expect("create staged predictive model metadata");
    metadata
        .set_string_property("task", task)
        .expect("set staged predictive model task");
    Some(
        catalog
            .register_model(
                package
                    .to_str()
                    .expect("staged predictive model path must be UTF-8"),
                model_id,
                metadata,
            )
            .await
            .expect("register staged predictive model"),
    )
}

fn missing_model(task: &str) {
    if std::env::var("FOUNDRY_REQUIRE_NON_GENERATIVE_MODELS")
        .is_ok_and(|value| value.eq_ignore_ascii_case("true"))
    {
        panic!("required cached {task} model was not discovered");
    }
    eprintln!("Skipping {task} integration test: no cached model available");
}

#[tokio::test]
async fn ranking_session_round_trips_native_openai_json() {
    let Some(model) = cached_model("text-ranking", "FOUNDRY_TEST_CLM_MODEL").await else {
        missing_model("text-ranking");
        return;
    };
    model.load().await.expect("ranking model.load() failed");
    assert!(matches!(
        DecisionSession::new(&model).await,
        Err(FoundryLocalError::Validation { .. })
    ));
    let session = RankingSession::new(&model)
        .await
        .expect("RankingSession::new failed");
    assert!(
        model.unload().await.is_err(),
        "unload must reject an active ranking session"
    );
    assert!(
        model.is_loaded().await.expect("model.is_loaded() failed"),
        "failed unload must preserve the loaded ranking model"
    );
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
    drop(session);
    model.unload().await.expect("ranking model.unload() failed");
}

#[tokio::test]
async fn decision_session_round_trips_native_openai_json() {
    let Some(model) = cached_model("typed-decision", "FOUNDRY_TEST_KEV_MODEL").await else {
        missing_model("typed-decision");
        return;
    };
    model.load().await.expect("decision model.load() failed");
    assert!(matches!(
        RankingSession::new(&model).await,
        Err(FoundryLocalError::Validation { .. })
    ));
    let mut questions = BTreeMap::new();
    let mut question = DecisionQuestion::new(DecisionQuestionType::Noul);
    question.instructions = json!("Should I take an umbrella?");
    questions.insert("umbrella".into(), question);
    let mut request = DecisionRequest::new(questions);
    request.state = json!({"weather": "heavy rain"});
    let session = DecisionSession::new(&model)
        .await
        .expect("DecisionSession::new failed");
    assert!(
        model.unload().await.is_err(),
        "unload must reject an active decision session"
    );
    assert!(
        model.is_loaded().await.expect("model.is_loaded() failed"),
        "failed unload must preserve the loaded decision model"
    );
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
    drop(session);
    model
        .unload()
        .await
        .expect("decision model.unload() failed");
}
