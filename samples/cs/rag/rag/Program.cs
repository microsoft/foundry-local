using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;
using Microsoft.AI.Foundry.Local;
using Microsoft.ML.OnnxRuntimeGenAI;
using static Betalgo.Ranul.OpenAI.ObjectModels.StaticValues.AssistantsStatics.MessageStatics;
using static System.Runtime.InteropServices.JavaScript.JSType;

internal class Program
{
    private static async Task Main(string[] args)
    {
        CancellationToken ct = new CancellationToken();

        var config = new Configuration
        {
            AppName = "foundry_local_rag",
            LogLevel = LogLevel.Information
        };

        // Initialize the singleton instance.
        await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
        var mgr = FoundryLocalManager.Instance;

        // Download and register all execution providers.
        var currentEp = "";
        await mgr.DownloadAndRegisterEpsAsync((epName, percent) =>
        {
            if (epName != currentEp)
            {
                if (currentEp != "") Console.WriteLine();
                currentEp = epName;
            }
            Console.Write($"\r  {epName.PadRight(30)}  {percent,6:F1}%");
        });
        if (currentEp != "") Console.WriteLine();


        // Get the model catalog
        var catalog = await mgr.GetCatalogAsync();

        // Get an embedding model
        var embeddingModel = await catalog.GetModelAsync("qwen3-embedding-0.6b") ?? throw new Exception("Embedding model not found");

        // Download the model (the method skips download if already cached)
        await embeddingModel.DownloadAsync(progress =>
        {
            Console.Write($"\rDownloading model: {progress:F2}%");
            if (progress >= 100f)
            {
                Console.WriteLine();
            }
        });

        // Load the model
        Console.Write($"Loading embedding model {embeddingModel.Id}...");
        await embeddingModel.LoadAsync();


        // Get an embedding client
        var embeddingClient = await embeddingModel.GetEmbeddingClientAsync();

        // Generate embeddings for multiple inputs

        // Knowledge base — each string represents a document
        var documents = new List<string>
        {
            "Foundry Local runs AI models directly on your device without cloud connectivity.",
            "The Foundry Local SDK supports Python, C#, JavaScript, and Rust.",
            "Embedding models convert text into numerical vectors for similarity search.",
            "Foundry Local uses ONNX Runtime for efficient model inference on CPUs and GPUs.",
            "The model catalog provides pre-optimized models that you can download and run locally.",
            "Retrieval-augmented generation grounds model responses in your own data.",
            "Vector similarity search finds documents that are semantically close to a query.",
            "Chat completions generate natural language responses from a prompt and context.",
        };

        Console.WriteLine("\n--- Batch Embeddings ---");
        var response = await embeddingClient.GenerateEmbeddingsAsync(documents);

        Console.WriteLine($"Number of embeddings: {response.Data.Count}");
        for (var i = 0; i < response.Data.Count; i++)
        {
            Console.WriteLine($"  [{i}] Dimensions: {response.Data[i].Embedding.Count}");
        }

        Console.WriteLine($"Indexed {response.Data.Count} documents.");


        // Get a model using an alias.
        var chatModel = await catalog.GetModelAsync("qwen2.5-0.5b") ?? throw new Exception("Model not found");

        // Download the model (the method skips download if already cached)
        await chatModel.DownloadAsync(progress =>
        {
            Console.Write($"\rDownloading model: {progress:F2}%");
            if (progress >= 100f)
            {
                Console.WriteLine();
            }
        });

        // Load the model
        Console.Write($"Loading model {chatModel.Id}...");
        await chatModel.LoadAsync();

        // <chat_completion>
        // Get a chat client
        var chatClient = await chatModel.GetChatClientAsync();

        Console.WriteLine("\nModels loaded. Ready for questions.");
        Console.WriteLine("\nThe knowledge base contains information about:");
        Console.WriteLine("  - Foundry Local features and architecture");
        Console.WriteLine("  - Supported programming languages");
        Console.WriteLine("  - Embedding models and vector search");
        Console.WriteLine("  - ONNX Runtime inference");
        Console.WriteLine("  - The model catalog");
        Console.WriteLine("  - RAG and chat completions");
        Console.WriteLine("\nExample questions:");
        Console.WriteLine("  \"What programming languages does the SDK support?\"");
        Console.WriteLine("  \"How does Foundry Local run models?\"");
        Console.WriteLine("  \"What is retrieval-augmented generation?\"");
        Console.WriteLine("\nType \"quit\" to exit.\n");

        // Interactive query loop
        while (true)
        {
            Console.WriteLine("Question:");
            var query = Console.ReadLine()?.Trim();
            if (string.IsNullOrEmpty(query) || query.ToLower() == "quit")
            {
                break;
            }

            // Embed the query
            var queryResponse = await embeddingClient.GenerateEmbeddingAsync(query);
            var queryEmbedding = queryResponse.Data[0].Embedding;

            // Retrieve the most relevant documents
            var results = FindRelevant(queryEmbedding, response.Data, topK: 2);
            string context = string.Join("\n", results.Select(r => $"- {documents[r.Item1]}"));

            // Build the prompt with retrieved context
            string content =
                $$"""
                Answer the user's question using only the provided context. 
                If the context doesn't contain enough information, say so.

                Context:
                {{context}}
                """;

            // Create chat messages
            List<ChatMessage> messages = new()
            {
                new ChatMessage { Role = "system", Content = content },
                new ChatMessage { Role = "user", Content = query }
            };

            // Get a streaming chat completion response
            Console.WriteLine("Answer:");
            var streamingResponse = chatClient.CompleteChatStreamingAsync(messages, ct);
            await foreach (var chunk in streamingResponse)
            {
                Console.Write(chunk.Choices[0].Message.Content);
                Console.Out.Flush();
            }
            Console.WriteLine();
        }

        // Tidy up - unload the models
        await chatModel.UnloadAsync();
        await embeddingModel.UnloadAsync();       
    }

    private static double CosineSimilarity(List<double> a, List<double> b)
    {
        double dot = 0;
        double norm_a = 0;
        double norm_b = 0;

        for (int i = 0; i < a.Count; i++)
        {
            dot += a[i] * b[i];
            norm_a += a[i] * a[i];
            norm_b += b[i] * b[i];
        }

        norm_a = Math.Sqrt(norm_a);
        norm_b = Math.Sqrt(norm_b);

        return norm_a * norm_b != 0 ? dot / (norm_a * norm_b) : 0.0;
    }


    private static (int, double)[] FindRelevant(List<double> queryEmbedding, List<EmbeddingResponse> docEmbeddings, int topK = 2)
    {
        var scores = new List<(int, double)>();
        for (int i = 0; i < docEmbeddings.Count; i++)
        {
            double score = CosineSimilarity(queryEmbedding, docEmbeddings[i].Embedding);
            scores.Add((i, score));
        }
        scores.Sort((x, y) => y.Item2.CompareTo(x.Item2));
        return scores.Take(topK).ToArray();
    }
}