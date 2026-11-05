# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Real native coverage for ranking and typed-decision SDK sessions."""

from foundry_local_sdk import (
    DecisionRequest,
    DecisionSession,
    RankingRequest,
    RankingSession,
)


def test_ranking_session_round_trips_native_json(ranking_model):
    try:
        with RankingSession(ranking_model) as session:
            result = session.rank(
                RankingRequest(
                    context={"weather": "heavy rain"},
                    question="Which activity is more suitable?",
                    answers=[
                        "Have a picnic outdoors",
                        "Visit an indoor museum",
                    ],
                )
            )
        assert result.model == ranking_model.info.id
        assert len(result.ranked) == 2
        assert result.ranked[0].rank == 1
        assert result.ranked[0].id == str(result.ranked[0].index)
    finally:
        ranking_model.unload()


def test_decision_session_round_trips_native_json(decision_model):
    try:
        with DecisionSession(decision_model) as session:
            result = session.decide(
                DecisionRequest(
                    state={"weather": "heavy rain"},
                    questions={
                        "umbrella": {
                            "type": "noul",
                            "instructions": "Should I take an umbrella?",
                        },
                        "activity": {
                            "type": "choice",
                            "criteria": {"inside": "Stay inside", "outside": "Go outside"},
                        },
                        "urgency": {
                            "type": "score",
                            "criteria": ["low", "medium", "high"],
                        },
                    },
                )
            )
        assert result.model == decision_model.info.id
        assert result.answers["umbrella"]["type"] == "noul"
        assert result.answers["activity"]["type"] == "choice"
        assert result.answers["urgency"]["type"] == "score"
    finally:
        decision_model.unload()
