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


def test_decision_session_round_trips_native_json(decision_model):
    with DecisionSession(decision_model) as session:
        result = session.decide(
            DecisionRequest(
                state={"weather": "heavy rain"},
                questions={
                    "umbrella": {
                        "type": "noul",
                        "instructions": "Should I take an umbrella?",
                    }
                },
            )
        )

    assert result.model == decision_model.info.id
    assert result.answers["umbrella"]["type"] == "noul"
    assert 0 <= result.answers["umbrella"]["noul"] <= 1
