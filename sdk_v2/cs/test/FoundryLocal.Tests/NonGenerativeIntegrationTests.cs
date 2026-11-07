// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Text.Json;

using TUnit.Core.Exceptions;

[NotInParallel]
[SkipUnlessIntegration]
internal sealed class NonGenerativeIntegrationTests
{
    [Test]
    public async Task RankingModel_ExplicitLifecycleRoundTrips()
    {
        var model = await GetRequiredModelAsync("clm-v0.1-8b-mixed-generic-cpu:1");
        await model.LoadAsync();
        await Assert.That(await model.IsLoadedAsync()).IsTrue();
        try
        {
            using (var session = new RankingSession(model))
            {
                await Assert.That(() => model.UnloadAsync()).Throws<FoundryLocalException>();
                await Assert.That(await model.IsLoadedAsync()).IsTrue();
                var result = await session.RankAsync(new RankingRequest
                {
                    Model = model.Info.Id,
                    Context = JsonSerializer.SerializeToElement(new { weather = "heavy rain" }),
                    Question = "Which activity is more suitable?",
                    Answers = ["Have a picnic outdoors", "Visit an indoor museum"],
                });
                await Assert.That(result.Model).IsEqualTo(model.Info.Id);
                await Assert.That(result.Ranked.Count).IsEqualTo(2);
                await Assert.That(result.Ranked.Select(item => item.Rank).Distinct().Count()).IsEqualTo(2);
            }
        }
        finally
        {
            await model.UnloadAsync();
        }

        await Assert.That(await model.IsLoadedAsync()).IsFalse();
        await Assert.That(() => new RankingSession(model)).Throws<FoundryLocalException>();
    }

    [Test]
    public async Task DecisionModel_ExplicitLifecycleRoundTrips()
    {
        var model = await GetRequiredModelAsync("kev-0.8b-mixed-generic-cpu:1");
        await model.LoadAsync();
        await Assert.That(await model.IsLoadedAsync()).IsTrue();
        try
        {
            using (var session = new DecisionSession(model))
            {
                await Assert.That(() => model.UnloadAsync()).Throws<FoundryLocalException>();
                await Assert.That(await model.IsLoadedAsync()).IsTrue();
                var result = await session.DecideAsync(new DecisionRequest
                {
                    Model = model.Info.Id,
                    State = JsonSerializer.SerializeToElement(new { weather = "heavy rain" }),
                    Questions = new Dictionary<string, DecisionQuestion>
                    {
                        ["umbrella"] = new()
                        {
                            Type = "noul",
                            Instructions = JsonSerializer.SerializeToElement("Should I take an umbrella?"),
                        },
                        ["activity"] = new()
                        {
                            Type = "choice",
                            Criteria = JsonSerializer.SerializeToElement(
                                new Dictionary<string, string>
                                {
                                    ["inside"] = "Stay inside",
                                    ["outside"] = "Go outside",
                                }),
                        },
                        ["urgency"] = new()
                        {
                            Type = "score",
                            Criteria = JsonSerializer.SerializeToElement(new[] { "low", "medium", "high" }),
                        },
                    },
                });
                await Assert.That(result.Model).IsEqualTo(model.Info.Id);
                await Assert.That(result.Answers["umbrella"].Type).IsEqualTo("noul");
                await Assert.That(result.Answers["activity"].Type).IsEqualTo("choice");
                await Assert.That(result.Answers["urgency"].Type).IsEqualTo("score");
            }
        }
        finally
        {
            await model.UnloadAsync();
        }

        await Assert.That(await model.IsLoadedAsync()).IsFalse();
        await Assert.That(() => new DecisionSession(model)).Throws<FoundryLocalException>();
    }

    private static async Task<IModel> GetRequiredModelAsync(string modelId)
    {
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync(CatalogType.Local);
        var model = await catalog.GetModelVariantAsync(modelId);
        if (model != null)
        {
            return model;
        }

        var message = $"Required model '{modelId}' was not found under FOUNDRY_TEST_DATA_DIR.";
        if (string.Equals(
                Environment.GetEnvironmentVariable("FOUNDRY_REQUIRE_NON_GENERATIVE_MODELS"),
                "true",
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(message);
        }

        throw new SkipTestException(message);
    }
}
