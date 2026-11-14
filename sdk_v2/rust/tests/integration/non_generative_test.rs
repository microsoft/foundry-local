//! Opt-in native integration coverage for ranking and typed-decision sessions.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use foundry_local_sdk::{
    DecisionAnswerValue, DecisionQuestion, DecisionQuestionType, DecisionRequest, DecisionSession,
    FoundryLocalError, Model, RankingRequest, RankingSession,
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
            "clm-v0.1-8b-mixed-generic-cpu-2",
            "clm-v0.1-8b-mixed-generic-cpu:2",
        ),
        "typed-decision" => (
            "kev-0.8b-mixed-generic-cpu-2",
            "kev-0.8b-mixed-generic-cpu:2",
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

async fn unload_after_drain(model: &Model) {
    tokio::time::timeout(Duration::from_secs(30), async {
        loop {
            match model.unload().await {
                Ok(()) => break,
                Err(_) => tokio::time::sleep(Duration::from_millis(50)).await,
            }
        }
    })
    .await
    .expect("model remained in use after all predictive session handles were dropped");
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
    let retained = session.clone();
    drop(session);
    assert!(
        model.unload().await.is_err(),
        "unload must reject a retained ranking session clone"
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
    let mut mismatched = request.clone();
    mismatched.model = Some("not-the-bound-ranking-model:1".into());
    assert!(
        retained.rank(&mismatched).await.is_err(),
        "explicit mismatched ranking model must be rejected"
    );
    request.model = Some(model.id().to_owned());
    assert!(
        tokio::time::timeout(Duration::from_millis(1), retained.rank(&request))
            .await
            .is_err(),
        "ranking inference unexpectedly completed before cancellation"
    );
    let result = tokio::time::timeout(Duration::from_secs(30), retained.rank(&request))
        .await
        .expect("canceled ranking request did not drain")
        .expect("ranking session was not reusable after cancellation");

    assert_eq!(result.model, model.id());
    assert_eq!(result.ranked.len(), 2);
    assert_eq!(result.ranked[0].rank, 1);
    drop(retained);
    unload_after_drain(&model).await;
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
    let mut noul = DecisionQuestion::new(DecisionQuestionType::Noul);
    noul.instructions = json!("Should I take an umbrella?");
    questions.insert("umbrella".into(), noul);
    questions.insert(
        "activity".into(),
        DecisionQuestion::choice(BTreeMap::from([
            ("indoors".into(), json!("Visit a museum")),
            ("outdoors".into(), json!("Have a picnic")),
        ])),
    );
    questions.insert(
        "urgency".into(),
        DecisionQuestion::score(vec![json!("low"), json!("medium"), json!("high")]),
    );
    let mut request = DecisionRequest::new(questions);
    request.state = json!({
        "weather": "heavy rain",
        "question": "Which activity is suitable and how urgent is the decision?"
    });
    let session = DecisionSession::new(&model)
        .await
        .expect("DecisionSession::new failed");
    let retained = session.clone();
    drop(session);
    assert!(
        model.unload().await.is_err(),
        "unload must reject a retained decision session clone"
    );
    assert!(
        model.is_loaded().await.expect("model.is_loaded() failed"),
        "failed unload must preserve the loaded decision model"
    );
    let mut mismatched = request.clone();
    mismatched.model = Some("not-the-bound-decision-model:1".into());
    assert!(
        retained.decide(&mismatched).await.is_err(),
        "explicit mismatched decision model must be rejected"
    );
    request.model = Some(model.id().to_owned());
    assert!(
        tokio::time::timeout(Duration::from_millis(1), retained.decide(&request))
            .await
            .is_err(),
        "decision inference unexpectedly completed before cancellation"
    );
    let result = tokio::time::timeout(Duration::from_secs(30), retained.decide(&request))
        .await
        .expect("canceled decision request did not drain")
        .expect("decision session was not reusable after cancellation");

    assert_eq!(result.model, model.id());
    assert_eq!(result.usage.billing_units, 0);

    let umbrella = &result.answers["umbrella"];
    assert_eq!(umbrella.kind, DecisionQuestionType::Noul);
    assert!(matches!(
        umbrella.value(),
        Some(DecisionAnswerValue::Noul(value)) if (0.0..=1.0).contains(&value)
    ));

    let activity = &result.answers["activity"];
    assert_eq!(activity.kind, DecisionQuestionType::Choice);
    assert!(matches!(
        activity.value(),
        Some(DecisionAnswerValue::Choice("indoors" | "outdoors"))
    ));
    assert!(activity
        .probabilities
        .iter()
        .all(
            |(key, value)| ["indoors", "outdoors"].contains(&key.as_str())
                && (0.0..=1.0).contains(value)
        ));
    assert!(activity
        .legend
        .values()
        .all(|key| ["indoors", "outdoors"].contains(&key.as_str())));

    let urgency = &result.answers["urgency"];
    assert_eq!(urgency.kind, DecisionQuestionType::Score);
    assert!(matches!(
        urgency.value(),
        Some(DecisionAnswerValue::Score(value)) if (0.0..=2.0).contains(&value)
    ));
    assert!(result.answers.values().all(|answer| answer
        .confidence
        .is_none_or(|value| (0.0..=1.0).contains(&value))));

    drop(retained);
    unload_after_drain(&model).await;
}
