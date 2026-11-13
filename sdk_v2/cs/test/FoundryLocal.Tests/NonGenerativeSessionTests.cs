// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Text.Json;

internal sealed class NonGenerativeSessionTests
{
    [Test]
    public async Task RankingSession_SendsRestContractAndReadsTypedResult()
    {
        string? captured = null;
        using var session = new RankingSession((json, _) =>
        {
            captured = json;
            return Task.FromResult(
                /*lang=json*/ """
                {"model":"clm-test:1","ranked":[
                  {"rank":2,"id":"0","index":0,"candidate":"outside","prob":0.25},
                  {"rank":1,"id":"1","index":1,"candidate":"inside","prob":0.75}
                ]}
                """);
        });

        var result = await session.RankAsync(new RankingRequest
        {
            Context = JsonSerializer.SerializeToElement(new { weather = "rain" }),
            Question = "Where should I go?",
            Answers = ["outside", "inside"],
            Model = "clm-test:1",
            Temperature = 0.5f,
        });

        using var payload = JsonDocument.Parse(captured!);
        var root = payload.RootElement;
        await Assert.That(root.GetProperty("context").GetProperty("weather").GetString()).IsEqualTo("rain");
        await Assert.That(root.GetProperty("question").GetString()).IsEqualTo("Where should I go?");
        await Assert.That(root.GetProperty("answers").GetArrayLength()).IsEqualTo(2);
        await Assert.That(root.GetProperty("model").GetString()).IsEqualTo("clm-test:1");
        await Assert.That(root.GetProperty("temperature").GetSingle()).IsEqualTo(0.5f);
        await Assert.That(result.Model).IsEqualTo("clm-test:1");
        await Assert.That(result.Ranked[1].Candidate).IsEqualTo("inside");
        await Assert.That(result.Ranked[1].Probability).IsEqualTo(0.75);
    }

    [Test]
    public async Task DecisionSession_SendsRestContractAndReadsTypedResult()
    {
        string? captured = null;
        using var session = new DecisionSession((json, _) =>
        {
            captured = json;
            return Task.FromResult(
                /*lang=json*/ """
                {"model":"kev-test:1","answers":{"umbrella":{"type":"noul","noul":0.9,"confidence":0.8}},"usage":{"billing_units":0}}
                """);
        });

        var result = await session.DecideAsync(new DecisionRequest
        {
            State = JsonSerializer.SerializeToElement(new { weather = "rain" }),
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["umbrella"] = new()
                {
                    Type = "noul",
                    Instructions = JsonSerializer.SerializeToElement("Should I take an umbrella?"),
                },
            },
            Model = "kev-test:1",
            Temperature = 1.0f,
        });

        using var payload = JsonDocument.Parse(captured!);
        var root = payload.RootElement;
        await Assert.That(root.GetProperty("state").GetProperty("weather").GetString()).IsEqualTo("rain");
        var question = root.GetProperty("questions").GetProperty("umbrella");
        await Assert.That(question.GetProperty("type").GetString()).IsEqualTo("noul");
        await Assert.That(question.GetProperty("instructions").GetString())
            .IsEqualTo("Should I take an umbrella?");
        await Assert.That(root.GetProperty("model").GetString()).IsEqualTo("kev-test:1");
        await Assert.That(root.GetProperty("temperature").GetSingle()).IsEqualTo(1.0f);
        await Assert.That(result.Answers["umbrella"].Noul).IsEqualTo(0.9);
        await Assert.That(result.Answers["umbrella"].Confidence).IsEqualTo(0.8);
        await Assert.That(result.Usage.BillingUnits).IsEqualTo(0);
    }

    [Test]
    public async Task RankingSession_PropagatesCancellationToNativeTransport()
    {
        CancellationToken observed = default;
        using var session = new RankingSession((_, ct) =>
        {
            observed = ct;
            return Task.FromCanceled<string>(ct);
        });
        using var cts = new CancellationTokenSource();
        cts.Cancel();

        await Assert.That(async () => await session.RankAsync(
            new RankingRequest { Question = "Choose", Answers = ["one"] },
            cts.Token)).Throws<TaskCanceledException>();
        await Assert.That(observed).IsEqualTo(cts.Token);
    }

    [Test]
    public async Task RankingSession_ReturnsSuccessfulResponseAfterCancellation()
    {
        var admitted = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var session = new RankingSession(async (_, _) =>
        {
            admitted.SetResult(true);
            await release.Task;
            return /*lang=json*/ """
                {"model":"clm-test:1","ranked":[
                  {"rank":1,"id":"0","index":0,"candidate":"one","prob":1.0}
                ]}
                """;
        });
        using var cts = new CancellationTokenSource();

        var pending = session.RankAsync(
            new RankingRequest { Question = "Choose", Answers = ["one"] },
            cts.Token);
        await admitted.Task;
        cts.Cancel();
        release.SetResult(true);

        var result = await pending;
        await Assert.That(result.Ranked[0].Candidate).IsEqualTo("one");
    }

    [Test]
    public async Task TypedSessions_OmitOptionalModelAndTemperature()
    {
        string? rankingJson = null;
        string? decisionJson = null;
        using var ranking = new RankingSession((json, _) =>
        {
            rankingJson = json;
            return Task.FromResult(
                """{"model":"bound-clm","ranked":[{"rank":1,"id":"0","index":0,"candidate":"one","prob":1.0}]}""");
        });
        using var decision = new DecisionSession((json, _) =>
        {
            decisionJson = json;
            return Task.FromResult(
                """{"model":"bound-kev","answers":{"q":{"type":"noul","noul":0.5}},"usage":{"billing_units":0}}""");
        });

        await ranking.RankAsync(new RankingRequest { Question = "Choose", Answers = ["one"] });
        await decision.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = DecisionQuestion.Noul(),
            },
        });

        using var rankingPayload = JsonDocument.Parse(rankingJson!);
        using var decisionPayload = JsonDocument.Parse(decisionJson!);
        await Assert.That(rankingPayload.RootElement.TryGetProperty("model", out _)).IsFalse();
        await Assert.That(rankingPayload.RootElement.TryGetProperty("temperature", out _)).IsFalse();
        await Assert.That(decisionPayload.RootElement.TryGetProperty("model", out _)).IsFalse();
        await Assert.That(decisionPayload.RootElement.TryGetProperty("temperature", out _)).IsFalse();
    }

    [Test]
    public async Task RankingSession_SnapshotsAnswersBeforeDispatch()
    {
        var answers = new List<string> { "one" };
        using var session = new RankingSession((_, _) =>
        {
            answers[0] = "changed";
            answers.Add("two");
            return Task.FromResult(
                """{"model":"clm","ranked":[{"rank":1,"id":"0","index":0,"candidate":"one","prob":1.0}]}""");
        });

        var result = await session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = answers,
        });

        await Assert.That(result.Ranked[0].Candidate).IsEqualTo("one");
    }

    [Test]
    public async Task DecisionSession_SnapshotsQuestionsBeforeDispatch()
    {
        using var criteriaDocument = JsonDocument.Parse("""{"a":"First","b":"Second"}""");
        var questions = new Dictionary<string, DecisionQuestion>
        {
            ["q"] = DecisionQuestion.Choice(criteriaDocument.RootElement),
        };
        using var session = new DecisionSession((_, _) =>
        {
            criteriaDocument.Dispose();
            questions.Clear();
            questions["changed"] = DecisionQuestion.Noul();
            return Task.FromResult(
                """{"model":"kev","answers":{"q":{"type":"choice","choice":"a"}},"usage":{"billing_units":0}}""");
        });

        var result = await session.DecideAsync(new DecisionRequest { Questions = questions });

        await Assert.That(result.Answers["q"].Choice).IsEqualTo("a");
    }

    [Test]
    public async Task RankingSession_ValidatesRequestBeforeTransport()
    {
        var calls = 0;
        using var session = new RankingSession((_, _) =>
        {
            calls++;
            return Task.FromResult("{}");
        });

        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Question = null!,
            Answers = ["candidate"],
        })).Throws<ArgumentException>();
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = [],
        })).Throws<ArgumentException>();
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = ["candidate"],
            Temperature = float.NaN,
        })).Throws<ArgumentOutOfRangeException>();
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = [""],
        })).Throws<ArgumentException>();
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = ["before\0after"],
        })).Throws<ArgumentException>();
        await Assert.That(calls).IsEqualTo(0);
    }

    [Test]
    public async Task RankingSession_AllowsWhitespaceOnlyAnswers()
    {
        using var session = new RankingSession((_, _) => Task.FromResult(
            """{"model":"clm","ranked":[{"rank":1,"id":"0","index":0,"candidate":"   ","prob":1.0}]}"""));

        var result = await session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = ["   "],
        });

        await Assert.That(result.Ranked[0].Candidate).IsEqualTo("   ");
    }

    [Test]
    public async Task RankingSession_ValidatesStructuredJsonBoundariesBeforeTransport()
    {
        var calls = 0;
        using var session = new RankingSession((_, _) =>
        {
            calls++;
            return Task.FromResult(
                """{"model":"clm","ranked":[{"rank":1,"id":"0","index":0,"candidate":"a","prob":1.0}]}""");
        });
        using var maxInteger = JsonDocument.Parse("""{"value":9223372036854775807}""");
        var result = await session.RankAsync(new RankingRequest
        {
            Context = maxInteger.RootElement,
            Answers = ["a"],
        });
        await Assert.That(result.Ranked.Count).IsEqualTo(1);

        var maximumEmptyDepth = new string('[', NonGenerativeValidation.MaxStructuredJsonDepth) +
            new string(']', NonGenerativeValidation.MaxStructuredJsonDepth);
        using var acceptedEmptyDepth = JsonDocument.Parse(maximumEmptyDepth);
        result = await session.RankAsync(new RankingRequest
        {
            Context = acceptedEmptyDepth.RootElement,
            Answers = ["a"],
        });
        await Assert.That(result.Ranked.Count).IsEqualTo(1);

        using var oversizedInteger = JsonDocument.Parse("""{"value":9223372036854775808}""");
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Context = oversizedInteger.RootElement,
            Answers = ["a"],
        })).Throws<ArgumentException>();

        var excessiveEmptyDepth = new string('[', NonGenerativeValidation.MaxStructuredJsonDepth + 1) +
            new string(']', NonGenerativeValidation.MaxStructuredJsonDepth + 1);
        using var rejectedEmptyDepth = JsonDocument.Parse(excessiveEmptyDepth);
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Context = rejectedEmptyDepth.RootElement,
            Answers = ["a"],
        })).Throws<ArgumentException>();

        var nested = new string('[', NonGenerativeValidation.MaxStructuredJsonDepth + 1) +
            "0" +
            new string(']', NonGenerativeValidation.MaxStructuredJsonDepth + 1);
        using var excessiveDepth = JsonDocument.Parse(nested);
        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Context = excessiveDepth.RootElement,
            Answers = ["a"],
        })).Throws<ArgumentException>();

        await Assert.That(calls).IsEqualTo(2);
    }

    [Test]
    public async Task DecisionSession_ValidatesQuestionBoundariesBeforeTransport()
    {
        var calls = 0;
        using var session = new DecisionSession((_, _) =>
        {
            calls++;
            return Task.FromResult("{}");
        });

        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>(),
        })).Throws<ArgumentException>();
        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new() { Type = "unknown" },
            },
        })).Throws<ArgumentException>();
        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new() { Type = "choice", Criteria = JsonSerializer.SerializeToElement(Array.Empty<string>()) },
            },
        })).Throws<ArgumentException>();
        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new()
                {
                    Type = "score",
                    Criteria = JsonSerializer.SerializeToElement(Enumerable.Range(0, 256).ToArray()),
                },
            },
        })).Throws<ArgumentException>();
        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new() { Type = "noul" },
            },
            Temperature = 2,
        })).Throws<ArgumentOutOfRangeException>();
        await Assert.That(calls).IsEqualTo(0);
    }

    [Test]
    public async Task DecisionSession_AcceptsCriteriaBoundary()
    {
        var criteria = Enumerable.Range(0, 255).Select(index => $"level-{index}").ToArray();
        using var session = new DecisionSession((_, _) => Task.FromResult(
            /*lang=json*/ """
            {"model":"kev-test:1","answers":{"q":{"type":"score","score":1.5}},"usage":{"billing_units":1}}
            """));

        var result = await session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new()
                {
                    Type = "score",
                    Criteria = JsonSerializer.SerializeToElement(criteria),
                },
            },
        });

        await Assert.That(result.Answers["q"].Score).IsEqualTo(1.5);
    }

    [Test]
    public async Task DecisionSession_TreatsJsonNullNoulCriteriaAsOmitted()
    {
        using var session = new DecisionSession((_, _) => Task.FromResult(
            """{"model":"kev","answers":{"q":{"type":"noul","noul":0.5}},"usage":{"billing_units":0}}"""));

        var result = await session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = DecisionQuestion.Noul(JsonSerializer.SerializeToElement("Decide"))
                    with
                { Criteria = JsonSerializer.SerializeToElement<object?>(null) },
            },
        });

        await Assert.That(result.Answers["q"].Noul).IsEqualTo(0.5);
    }

    [Test]
    public async Task DecisionQuestion_FactoriesUseNamedKinds()
    {
        var choices = JsonSerializer.SerializeToElement(new Dictionary<string, string> { ["a"] = "A" });
        var scores = JsonSerializer.SerializeToElement(new[] { "low", "high" });

        await Assert.That(DecisionQuestion.Noul().Type).IsEqualTo(DecisionQuestionKind.Noul);
        await Assert.That(DecisionQuestion.Choice(choices).Type).IsEqualTo(DecisionQuestionKind.Choice);
        await Assert.That(DecisionQuestion.Score(scores).Type).IsEqualTo(DecisionQuestionKind.Score);
    }

    [Test]
    [Arguments("""
        {"model":"clm:1","ranked":[
          {"rank":1,"id":"0","index":0,"candidate":"a","prob":0.5},
          {"rank":1,"id":"1","index":1,"candidate":"b","prob":0.5}
        ]}
        """)]
    [Arguments("""
        {"model":"clm:1","ranked":[
          {"rank":1,"id":"0","index":0,"candidate":"a","prob":0.5},
          {"rank":2,"id":"0","index":0,"candidate":"a","prob":0.5}
        ]}
        """)]
    [Arguments("""
        {"model":"clm:1","ranked":[
          {"rank":1,"id":"00","index":0,"candidate":"a","prob":0.5},
          {"rank":2,"id":"1","index":1,"candidate":"b","prob":0.5}
        ]}
        """)]
    [Arguments("""
        {"model":"clm:1","ranked":[
          {"rank":1,"id":"0","index":0,"candidate":"a"},
          {"rank":2,"id":"1","index":1,"candidate":"b","prob":0.5}
        ]}
        """)]
    public async Task RankingSession_RejectsInvalidRanksAndNumbers(string response)
    {
        using var session = new RankingSession((_, _) => Task.FromResult(response));

        await Assert.That(() => session.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = ["a", "b"],
        })).Throws<FoundryLocalException>();
    }

    [Test]
    public async Task DecisionSession_RejectsMismatchedKeysAndVariants()
    {
        using var wrongKey = new DecisionSession((_, _) => Task.FromResult(
            /*lang=json*/ """
            {"model":"kev:1","answers":{"other":{"type":"noul","noul":0.5}},"usage":{"billing_units":0}}
            """));
        using var wrongType = new DecisionSession((_, _) => Task.FromResult(
            /*lang=json*/ """
            {"model":"kev:1","answers":{"q":{"type":"score","score":0.5}},"usage":{"billing_units":0}}
            """));
        var request = new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new() { Type = "noul" },
            },
        };

        await Assert.That(() => wrongKey.DecideAsync(request)).Throws<FoundryLocalException>();
        await Assert.That(() => wrongType.DecideAsync(request)).Throws<FoundryLocalException>();
    }

    [Test]
    public async Task DecisionSession_RejectsInvalidNumericMetadata()
    {
        using var session = new DecisionSession((_, _) => Task.FromResult(
            /*lang=json*/ """
            {"model":"kev:1","answers":{"q":{"type":"choice","choice":"a","confidence":1.1}},"usage":{"billing_units":0}}
            """));

        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new()
                {
                    Type = "choice",
                    Criteria = JsonSerializer.SerializeToElement(new Dictionary<string, string> { ["a"] = "A" }),
                },
            },
        })).Throws<FoundryLocalException>();
    }

    [Test]
    public async Task DecisionSession_RejectsAnswerOutsideCriteriaRange()
    {
        using var session = new DecisionSession((_, _) => Task.FromResult(
            /*lang=json*/ """
            {"model":"kev:1","answers":{"q":{"type":"score","score":2.0}},"usage":{"billing_units":0}}
            """));

        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new()
                {
                    Type = "score",
                    Criteria = JsonSerializer.SerializeToElement(new[] { "low", "high" }),
                },
            },
        })).Throws<FoundryLocalException>();
    }

    [Test]
    public async Task TypedSessions_RejectUseAfterDispose()
    {
        var ranking = new RankingSession((_, _) => Task.FromResult("{}"));
        var decision = new DecisionSession((_, _) => Task.FromResult("{}"));
        ranking.Dispose();
        decision.Dispose();

        await Assert.That(() => ranking.RankAsync(new RankingRequest
        {
            Question = "Choose",
            Answers = ["a"],
        })).Throws<ObjectDisposedException>();
        await Assert.That(() => decision.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new() { Type = "noul" },
            },
        })).Throws<ObjectDisposedException>();
    }

    [Test]
    public async Task DecisionSession_PropagatesCancellationToTransport()
    {
        CancellationToken observed = default;
        using var session = new DecisionSession((_, ct) =>
        {
            observed = ct;
            return Task.FromCanceled<string>(ct);
        });
        using var cts = new CancellationTokenSource();
        cts.Cancel();

        await Assert.That(() => session.DecideAsync(new DecisionRequest
        {
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["q"] = new() { Type = "noul" },
            },
        }, cts.Token)).Throws<TaskCanceledException>();
        await Assert.That(observed).IsEqualTo(cts.Token);
    }
}
