#!/usr/bin/env python3
# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

"""Typed requests using registered, cached, and explicitly loaded task models."""

from foundry_local_sdk import (
    DecisionRequest,
    DecisionSession,
    IModel,
    RankingRequest,
    RankingSession,
)


def rank_activities(model: IModel) -> None:
    """Load a ranking model and reuse one session for several requests."""
    model.load()
    try:
        with RankingSession(model) as session:
            for weather in ("heavy rain", "snow"):
                result = session.rank(
                    RankingRequest(
                        context={"weather": weather},
                        question="Which activity is more suitable?",
                        answers=["Have a picnic outdoors", "Visit an indoor museum"],
                    )
                )
                for item in result.ranked:
                    print(item.rank, item.id, item.index, item.candidate, item.prob)
    finally:
        model.unload()


def decide_about_umbrella(model: IModel) -> None:
    """Load and use a decision model registered in the local catalog."""
    model.load()
    try:
        with DecisionSession(model) as session:
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
            print(result.answers["umbrella"])
    finally:
        model.unload()
