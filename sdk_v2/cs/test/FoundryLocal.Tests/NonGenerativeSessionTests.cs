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
                /*lang=json*/ """{"model":"clm-test:1","ranked":[{"rank":1,"candidate":"inside","prob":0.75}]}""");
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
        await Assert.That(result.Ranked[0].Candidate).IsEqualTo("inside");
        await Assert.That(result.Ranked[0].Probability).IsEqualTo(0.75);
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
            Temperature = 0.25f,
        });

        using var payload = JsonDocument.Parse(captured!);
        var root = payload.RootElement;
        await Assert.That(root.GetProperty("state").GetProperty("weather").GetString()).IsEqualTo("rain");
        var question = root.GetProperty("questions").GetProperty("umbrella");
        await Assert.That(question.GetProperty("type").GetString()).IsEqualTo("noul");
        await Assert.That(question.GetProperty("instructions").GetString())
            .IsEqualTo("Should I take an umbrella?");
        await Assert.That(root.GetProperty("model").GetString()).IsEqualTo("kev-test:1");
        await Assert.That(root.GetProperty("temperature").GetSingle()).IsEqualTo(0.25f);
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
            new RankingRequest { Answers = ["one"] },
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
            return /*lang=json*/ """{"model":"clm-test:1","ranked":[]}""";
        });
        using var cts = new CancellationTokenSource();

        var pending = session.RankAsync(
            new RankingRequest { Answers = ["one"] },
            cts.Token);
        await admitted.Task;
        cts.Cancel();
        release.SetResult();

        await Assert.That(async () => await pending).Throws<OperationCanceledException>();
    }
}
