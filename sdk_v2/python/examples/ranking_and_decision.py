#!/usr/bin/env python3
# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

"""Typed ranking and decision requests with cached native model packages."""

from foundry_local_sdk import (
    DecisionRequest,
    DecisionSession,
    IModel,
    RankingRequest,
    RankingSession,
)


def rank_activities(model: IModel) -> None:
    with RankingSession(model) as session:
        result = session.rank(
            RankingRequest(
                context={"weather": "heavy rain"},
                question="Which activity is more suitable?",
                answers=["Have a picnic outdoors", "Visit an indoor museum"],
            )
        )
        for item in result.ranked:
            print(item.rank, item.candidate, item.prob)


def decide_about_umbrella(model: IModel) -> None:
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
