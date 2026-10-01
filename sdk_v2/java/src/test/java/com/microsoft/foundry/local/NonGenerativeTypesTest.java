// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;

import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class NonGenerativeTypesTest {
    @Test void rankingTypesMatchEndpointJson() {
        RankingRequest request =
                new RankingRequest(Map.of("weather", "rain"), "best?", List.of("picnic", "museum"));
        String json = JsonSession.writeJson(request);
        assertTrue(json.contains("\"model\":\"clm\""));
        assertTrue(json.contains("\"temperature\":1.0"));

        RankingResult result = JsonSession.readJson(
                "{\"model\":\"clm:1\",\"ranked\":[{\"rank\":1,\"candidate\":\"museum\","
                        + "\"prob\":0.8,\"future_candidate_field\":true}],"
                        + "\"future_result_field\":\"ignored\"}",
                RankingResult.class);
        assertEquals("museum", result.ranked().get(0).candidate());
        assertEquals(0.8, result.ranked().get(0).prob());
    }

    @Test void decisionTypesMatchEndpointJson() {
        DecisionRequest request = new DecisionRequest(
                Map.of("weather", "rain"),
                Map.of("umbrella", new DecisionQuestion(
                        DecisionQuestionType.NOUL, null, "Take an umbrella?")));
        String json = JsonSession.writeJson(request);
        assertTrue(json.contains("\"type\":\"noul\""));
        assertTrue(json.contains("\"model\":\"kev\""));

        DecisionResult result = JsonSession.readJson(
                "{\"model\":\"kev:1\",\"answers\":{\"umbrella\":{\"type\":\"noul\","
                        + "\"noul\":0.9,\"future_answer_field\":true}},"
                        + "\"usage\":{\"billing_units\":0,\"future_usage_field\":1},"
                        + "\"future_result_field\":\"ignored\"}",
                DecisionResult.class);
        assertEquals(DecisionQuestionType.NOUL, result.answers().get("umbrella").type());
        assertEquals(0.9, result.answers().get("umbrella").noul());
    }

    @Test void requestsRejectInvalidEndpointValues() {
        assertThrows(IllegalArgumentException.class,
                () -> new RankingRequest(null, "q", List.of(), "clm", 1));
        assertThrows(IllegalArgumentException.class,
                () -> new DecisionRequest(null, Map.of(), "kev", 1));
    }
}
