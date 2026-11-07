using System.Text.Json;

using Microsoft.AI.Foundry.Local;

static async Task RankAsync(IModel model)
{
    await model.LoadAsync();
    try
    {
        using var session = new RankingSession(model);
        var result = await session.RankAsync(new RankingRequest
        {
            Model = model.Info.Id,
            Context = JsonSerializer.SerializeToElement(new { weather = "heavy rain" }),
            Question = "Which activity is more suitable?",
            Answers = ["Have a picnic outdoors", "Visit an indoor museum"],
        });
        foreach (var candidate in result.Ranked.OrderBy(candidate => candidate.Rank))
        {
            Console.WriteLine($"{candidate.Rank}: {candidate.Candidate} ({candidate.Probability:P1})");
        }
    }
    finally
    {
        await model.UnloadAsync();
    }
}

static async Task DecideAsync(IModel model)
{
    await model.LoadAsync();
    try
    {
        using var session = new DecisionSession(model);
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
            },
        });
        Console.WriteLine($"Umbrella: {result.Answers["umbrella"].Noul:P1}");
    }
    finally
    {
        await model.UnloadAsync();
    }
}

var catalog = await FoundryLocalManager.Instance.GetCatalogAsync(CatalogType.Local);
var rankingModel = await catalog.GetModelAsync("clm")
    ?? throw new InvalidOperationException("Register a cached CLM model with alias 'clm'.");
var decisionModel = await catalog.GetModelAsync("kev")
    ?? throw new InvalidOperationException("Register a cached KEV model with alias 'kev'.");

await RankAsync(rankingModel);
await DecideAsync(decisionModel);
