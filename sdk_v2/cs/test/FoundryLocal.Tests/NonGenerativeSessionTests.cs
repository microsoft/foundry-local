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
    public async Task RankingSession_RejectsSuccessfulResponseAfterCancellation()
    {
        var admitted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var session = new RankingSession(async (_, _) =>
        {
            admitted.SetResult();
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
        release.SetResult();

        await Assert.That(async () => await pending).Throws<OperationCanceledException>();
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
        await Assert.That(calls).IsEqualTo(0);
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
    public async Task RankingSession_RejectsInvalidRanksAndNumbers()
    {
        using var session = new RankingSession((_, _) => Task.FromResult(
            /*lang=json*/ """
            {"model":"clm:1","ranked":[
              {"rank":1,"id":"0","index":0,"candidate":"a","prob":0.5},
              {"rank":1,"id":"1","index":1,"candidate":"b","prob":0.5}
            ]}
            """));

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
