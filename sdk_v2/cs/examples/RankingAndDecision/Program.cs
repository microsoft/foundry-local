using System.Text.Json;

using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

static JsonElement Json(string value)
{
    using var document = JsonDocument.Parse(value);
    return document.RootElement.Clone();
}

static async Task RankAsync(IModel model)
{
    await model.LoadAsync();
    try
    {
        using var session = new RankingSession(model);
        var result = await session.RankAsync(new RankingRequest
        {
            Model = model.Info.Id,
            Context = Json("""{"weather":"heavy rain"}"""),
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
            State = Json("""{"weather":"heavy rain"}"""),
            Questions = new Dictionary<string, DecisionQuestion>
            {
                ["umbrella"] = DecisionQuestion.Noul(Json("\"Should I take an umbrella?\"")),
            },
        });
        Console.WriteLine($"Umbrella: {result.Answers["umbrella"].Noul:P1}");
    }
    finally
    {
        await model.UnloadAsync();
    }
}

var configuration = new Configuration { AppName = "FoundryLocalRankingAndDecision" };
await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance);
using var manager = FoundryLocalManager.Instance;

// These aliases come from predictive packages previously registered in the local catalog.
// Registration points at the package's cached model directory; it does not download the model.
var catalog = await manager.GetCatalogAsync(CatalogType.Local);
var rankingModel = await catalog.GetModelAsync("clm")
    ?? throw new InvalidOperationException("Register a cached CLM model with alias 'clm'.");
var decisionModel = await catalog.GetModelAsync("kev")
    ?? throw new InvalidOperationException("Register a cached KEV model with alias 'kev'.");

await RankAsync(rankingModel);
await DecideAsync(decisionModel);
