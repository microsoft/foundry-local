// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/** Explicit opt-in native coverage for the ranking and typed-decision bridge. */
class NativeNonGenerativeTest {
    @TempDir Path temporary;

    @Test void realRankingAndDecisionSessionsProcessTypedRequests() {
        String runtime = setting("foundry.test.runtime", "FOUNDRY_LOCAL_NATIVE_BIN_DIR");
        String cache = setting("foundry.test.cache", "FOUNDRY_TEST_DATA_DIR");
        String clmModel = setting("foundry.test.clm", "FOUNDRY_TEST_CLM_MODEL");
        String kevModel = setting("foundry.test.kev", "FOUNDRY_TEST_KEV_MODEL");
        if (Boolean.getBoolean("foundry.test.native.required")) {
            assertNotNull(runtime, "Missing native test runtime");
            assertNotNull(cache, "Missing native test cache");
            assertNotNull(clmModel, "Missing native CLM model ID");
            assertNotNull(kevModel, "Missing native KEV model ID");
        } else {
            assumeTrue(
                    runtime != null && cache != null && clmModel != null && kevModel != null,
                    "Configure the native runtime, cache, CLM model, and KEV model");
        }

        Configuration config = Configuration.builder("java-non-generative-test")
                .runtimeDirectory(Path.of(runtime))
                .modelCacheDirectory(Path.of(cache))
                .appDataDirectory(temporary)
                .build();
        try (FoundryLocalManager manager = new FoundryLocalManager(config)) {
            Model clm = manager.catalog().getModelVariant(clmModel);
            assertTrue(clm.isCached(), "Explicitly prepare the CLM package first");
            try (RankingSession session = clm.createRankingSession()) {
                RankingResult result = session.rank(new RankingRequest(
                        Map.of("weather", "heavy rain"),
                        "Which activity is more suitable?",
                        List.of("Have a picnic outdoors", "Visit an indoor museum")));
                assertEquals(clm.info().id(), result.model());
                assertEquals(2, result.ranked().size());
                assertEquals(1, result.ranked().get(0).rank());
            }

            Model kev = manager.catalog().getModelVariant(kevModel);
            assertTrue(kev.isCached(), "Explicitly prepare the KEV package first");
            try (DecisionSession session = kev.createDecisionSession()) {
                DecisionResult result = session.decide(new DecisionRequest(
                        Map.of("weather", "heavy rain"),
                        Map.of(
                                "umbrella",
                                new DecisionQuestion(
                                        DecisionQuestionType.NOUL,
                                        null,
                                        "Should I take an umbrella?"))));
                assertEquals(kev.info().id(), result.model());
                assertTrue(result.answers().containsKey("umbrella"));
                assertNotNull(result.answers().get("umbrella").noul());
            }
        }
    }

    private static String setting(String property, String environmentVariable) {
        String value = System.getProperty(property);
        if (value == null) value = System.getenv(environmentVariable);
        return value == null || value.isBlank() ? null : value;
    }
}
