<script lang="ts">
	import { Copy, Check, Terminal, Code2 } from 'lucide-svelte';
	import { toast } from 'svelte-sonner';
	import { cubicOut } from 'svelte/easing';

	type SdkLanguage = 'python' | 'javascript' | 'csharp' | 'rust' | 'c' | 'cpp';

	type Snippet = {
		raw: string;
		html: string;
	};

	function nativeSnippet(raw: string): Snippet {
		const escaped = raw.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;');
		const tokens =
			/("(?:\\.|[^"\\])*"|&lt;[\w./]+&gt;)|(#include\b|\b(?:auto|catch|const|false|for|if|namespace|nullptr|return|static|throw|true|try|using)\b)|(\b(?:fl[A-Z]\w*|[A-Z][A-Z0-9_]*|Configuration|Manager|ChatSession|Request|Response|Item|char|exception|int|runtime_error|size_t|void)\b)|(\b[A-Za-z_]\w*(?=\s*\())/g;

		return {
			raw,
			html: escaped.replace(
				tokens,
				(
					token: string,
					stringToken: string | undefined,
					keywordToken: string | undefined,
					typeToken: string | undefined
				) => {
					const style = stringToken
						? 'code-string'
						: keywordToken
							? 'code-keyword'
							: typeToken
								? 'code-type'
								: 'code-call';
					return `<span class="${style}">${token}</span>`;
				}
			)
		};
	}

	const tabs: Array<{ key: SdkLanguage; label: string }> = [
		{ key: 'python', label: 'Python' },
		{ key: 'javascript', label: 'JS' },
		{ key: 'csharp', label: 'C#' },
		{ key: 'rust', label: 'Rust' },
		{ key: 'c', label: 'C' },
		{ key: 'cpp', label: 'C++' }
	];

	const sdkCommands: Record<SdkLanguage, string> = {
		python: 'pip install foundry-local-sdk',
		javascript: 'npm install foundry-local-sdk',
		csharp: 'dotnet add package Microsoft.AI.Foundry.Local',
		rust: 'cargo add foundry-local-sdk tokio --features tokio/macros,tokio/rt-multi-thread',
		c: 'nuget install Microsoft.AI.Foundry.Local.Runtime',
		cpp: 'nuget install Microsoft.AI.Foundry.Local.Runtime'
	};

	const filenames: Record<SdkLanguage, string> = {
		python: 'main.py',
		javascript: 'index.mjs',
		csharp: 'Program.cs',
		rust: 'src/main.rs',
		c: 'main.c',
		cpp: 'main.cpp'
	};

	const setupSnippets: Record<SdkLanguage, Snippet> = {
		c: nativeSnippet(String.raw`#include <foundry_local/foundry_local_c.h>
#include <stdio.h>
#include <stdlib.h>

static const flApi* api;

static void check(flStatus* status) {
    if (status) {
        fprintf(stderr, "%s\n", api->Status_GetErrorMessage(status));
        api->Status_Release(status);
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    api = FoundryLocalGetApi(FOUNDRY_LOCAL_API_VERSION);
    if (!api) {
        fputs("Unsupported Foundry Local API version.\n", stderr);
        return EXIT_FAILURE;
    }
    const flConfigurationApi* config_api = api->GetConfigurationApi();
    const flModelApi* models = api->GetModelApi();
    const flInferenceApi* inference = api->GetInferenceApi();
    const flItemApi* items = api->GetItemApi();
    flConfiguration* config = NULL;
    flManager* manager = NULL;
    flCatalog* catalog = NULL;
    flModel* model = NULL;
    flSession* session = NULL;
    check(config_api->Create("my-app", &config));
    check(api->Manager_Create(config, &manager));
    check(api->Manager_GetCatalog(manager, &catalog));
    check(api->GetCatalogApi()->GetModel(catalog, "qwen2.5-0.5b", &model));
    if (!model) {
        fputs("Model not found.\n", stderr);
        api->Manager_Release(manager);
        config_api->Configuration_Release(config);
        return EXIT_FAILURE;
    }
    check(models->Download(model, NULL, NULL));
    check(models->Load(model));
    check(inference->Session_Create(model, &session));`),
		cpp: nativeSnippet(String.raw`#include <foundry_local/foundry_local_cpp.h>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace foundry_local;

int main() {
    try {
        Manager manager{Configuration("my-app")};
        auto model = manager.GetCatalog().GetModel("qwen2.5-0.5b");
        if (!model) {
            throw std::runtime_error("Model not found.");
        }
        model->Download();
        model->Load();`),
		python: {
			raw: `from foundry_local_sdk import Configuration, FoundryLocalManager
	
FoundryLocalManager.initialize(Configuration(app_name="my-app"))
model = FoundryLocalManager.instance.catalog.get_model("qwen2.5-0.5b")
model.download(); model.load()
client = model.get_chat_client()`,
			html: `<span class="code-keyword">from</span> <span class="code-type">foundry_local_sdk</span> <span class="code-keyword">import</span> <span class="code-type">Configuration</span>, <span class="code-type">FoundryLocalManager</span>

<span class="code-type">FoundryLocalManager</span>.<span class="code-call">initialize</span>(<span class="code-type">Configuration</span>(app_name=<span class="code-string">"my-app"</span>))
model = <span class="code-type">FoundryLocalManager</span>.instance.catalog.<span class="code-call">get_model</span>(<span class="code-string">"qwen2.5-0.5b"</span>)
model.<span class="code-call">download</span>(); model.<span class="code-call">load</span>()
client = model.<span class="code-call">get_chat_client</span>()`
		},
		javascript: {
			raw: `import { FoundryLocalManager } from 'foundry-local-sdk';

const manager = FoundryLocalManager.create({ appName: 'my-app' });
const model = await manager.catalog.getModel('qwen2.5-0.5b');
await model.download(); await model.load();
const chat = model.createChatClient();`,
			html: `<span class="code-keyword">import</span> { <span class="code-type">FoundryLocalManager</span> } <span class="code-keyword">from</span> <span class="code-string">'foundry-local-sdk'</span>;

<span class="code-keyword">const</span> manager = <span class="code-type">FoundryLocalManager</span>.<span class="code-call">create</span>({ appName: <span class="code-string">'my-app'</span> });
<span class="code-keyword">const</span> model = <span class="code-keyword">await</span> manager.catalog.<span class="code-call">getModel</span>(<span class="code-string">'qwen2.5-0.5b'</span>);
<span class="code-keyword">await</span> model.<span class="code-call">download</span>(); <span class="code-keyword">await</span> model.<span class="code-call">load</span>();
<span class="code-keyword">const</span> chat = model.<span class="code-call">createChatClient</span>();`
		},
		csharp: {
			raw: `using System;
using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;
using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
await FoundryLocalManager.CreateAsync(
    new Configuration { AppName = "my-app" },
    NullLogger.Instance);
var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();
var model = await catalog.GetModelAsync("qwen2.5-0.5b")
    ?? throw new Exception("Model not found.");
await model.DownloadAsync(); await model.LoadAsync();
var chat = await model.GetChatClientAsync();`,
			html: `<span class="code-keyword">using</span> <span class="code-type">System</span>;
<span class="code-keyword">using</span> <span class="code-type">Microsoft.AI.Foundry.Local</span>;
<span class="code-keyword">using</span> <span class="code-type">Microsoft.Extensions.Logging.Abstractions</span>;
<span class="code-keyword">using</span> <span class="code-type">Betalgo.Ranul.OpenAI.ObjectModels.RequestModels</span>;
<span class="code-keyword">await</span> <span class="code-type">FoundryLocalManager</span>.<span class="code-call">CreateAsync</span>(
    <span class="code-keyword">new</span> <span class="code-type">Configuration</span> { AppName = <span class="code-string">"my-app"</span> },
    <span class="code-type">NullLogger</span>.Instance);
<span class="code-keyword">var</span> catalog = <span class="code-keyword">await</span> <span class="code-type">FoundryLocalManager</span>.Instance.<span class="code-call">GetCatalogAsync</span>();
<span class="code-keyword">var</span> model = <span class="code-keyword">await</span> catalog.<span class="code-call">GetModelAsync</span>(<span class="code-string">"qwen2.5-0.5b"</span>)
    ?? <span class="code-keyword">throw new</span> <span class="code-type">Exception</span>(<span class="code-string">"Model not found."</span>);
<span class="code-keyword">await</span> model.<span class="code-call">DownloadAsync</span>(); <span class="code-keyword">await</span> model.<span class="code-call">LoadAsync</span>();
<span class="code-keyword">var</span> chat = <span class="code-keyword">await</span> model.<span class="code-call">GetChatClientAsync</span>();`
		},
		rust: {
			raw: `use foundry_local_sdk::{
    ChatCompletionRequestUserMessage, FoundryLocalConfig, FoundryLocalManager,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("my-app"))?;
    let model = manager.catalog().get_model("qwen2.5-0.5b").await?;
    model.download(None::<fn(f64)>).await?;
    model.load().await?;
    let client = model.create_chat_client();`,
			html: `<span class="code-keyword">use</span> <span class="code-type">foundry_local_sdk</span>::{
    <span class="code-type">ChatCompletionRequestUserMessage</span>, <span class="code-type">FoundryLocalConfig</span>, <span class="code-type">FoundryLocalManager</span>,
};

#[tokio::main]
<span class="code-keyword">async fn</span> <span class="code-call">main</span>() -&gt; <span class="code-type">Result</span>&lt;(), <span class="code-type">Box</span>&lt;dyn std::error::Error&gt;&gt; {
    <span class="code-keyword">let</span> manager = <span class="code-type">FoundryLocalManager</span>::<span class="code-call">create</span>(<span class="code-type">FoundryLocalConfig</span>::<span class="code-call">new</span>(<span class="code-string">"my-app"</span>))?;
    <span class="code-keyword">let</span> model = manager.<span class="code-call">catalog</span>().<span class="code-call">get_model</span>(<span class="code-string">"qwen2.5-0.5b"</span>).await?;
    model.<span class="code-call">download</span>(None::&lt;fn(f64)&gt;).await?;
    model.<span class="code-call">load</span>().await?;
    <span class="code-keyword">let</span> client = model.<span class="code-call">create_chat_client</span>();`
		}
	};

	const inferenceSnippets: Record<SdkLanguage, Snippet> = {
		c: nativeSnippet(String.raw`    flRequest* request = NULL;
    flResponse* response = NULL;
    flItem* input = NULL;
    flTextData json = {
        FOUNDRY_LOCAL_API_VERSION,
        "{\"messages\":[{\"role\":\"user\",\"content\":\"Hello!\"}]}",
        FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON
    };
    check(inference->Request_Create(&request));
    check(items->Create(FOUNDRY_LOCAL_ITEM_TEXT, &input));
    check(items->SetText(input, &json));
    check(inference->Request_AddItem(request, input, false));
    check(inference->Session_ProcessRequest(session, request, &response));
    for (size_t i = 0; i < inference->Response_GetItemCount(response); ++i) {
        const flItem* output = NULL;
        check(inference->Response_GetItem(response, i, &output));
        if (items->GetType(output) == FOUNDRY_LOCAL_ITEM_TEXT) {
            flTextData text = {0};
            text.version = FOUNDRY_LOCAL_API_VERSION;
            check(items->GetText(output, &text));
            puts(text.text);
        }
    }
    inference->Response_Release(response);
    inference->Request_Release(request);
    items->Item_Release(input);
    inference->Session_Release(session);
    check(models->Unload(model));
    api->Manager_Release(manager);
    config_api->Configuration_Release(config);
    return EXIT_SUCCESS;
}`),
		cpp: nativeSnippet(String.raw`        {
            ChatSession session(*model);
            Response response = session.ProcessRequest(Request{UserMessage("Hello!")});
            for (const Item& item : response.GetItems()) {
                if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
                    std::cout << item.GetMessage().GetSimpleText() << "\n";
                }
            }
        }
        model->Unload();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}`),
		python: {
			raw: `response = client.complete_chat([
    {"role": "user", "content": "Hello!"}
])
print(response.choices[0].message.content)
model.unload()`,
			html: `response = client.<span class="code-call">complete_chat</span>([
    {<span class="code-string">"role"</span>: <span class="code-string">"user"</span>, <span class="code-string">"content"</span>: <span class="code-string">"Hello!"</span>}
])
<span class="code-call">print</span>(response.choices[0].message.content)
model.<span class="code-call">unload</span>()`
		},
		javascript: {
			raw: `const response = await chat.completeChat([
  { role: 'user', content: 'Hello!' }
]);
console.log(response.choices[0]?.message?.content);
await model.unload();`,
			html: `<span class="code-keyword">const</span> response = <span class="code-keyword">await</span> chat.<span class="code-call">completeChat</span>([
  { role: <span class="code-string">'user'</span>, content: <span class="code-string">'Hello!'</span> }
]);
console.<span class="code-call">log</span>(response.choices[0]?.message?.content);
<span class="code-keyword">await</span> model.<span class="code-call">unload</span>();`
		},
		csharp: {
			raw: `var response = await chat.CompleteChatAsync(new[] {
    new ChatMessage { Role = "user", Content = "Hello!" }
});
Console.WriteLine(response.Choices![0].Message.Content);
await model.UnloadAsync();`,
			html: `<span class="code-keyword">var</span> response = <span class="code-keyword">await</span> chat.<span class="code-call">CompleteChatAsync</span>(new[] {
    <span class="code-keyword">new</span> <span class="code-type">ChatMessage</span> { Role = <span class="code-string">"user"</span>, Content = <span class="code-string">"Hello!"</span> }
});
<span class="code-type">Console</span>.<span class="code-call">WriteLine</span>(response.Choices![0].Message.Content);
<span class="code-keyword">await</span> model.<span class="code-call">UnloadAsync</span>();`
		},
		rust: {
			raw: `    let messages = vec![
        ChatCompletionRequestUserMessage::from("Hello!").into()
    ];
    let response = client.complete_chat(&messages, None).await?;
    if let Some(content) = response.choices.first().and_then(|choice| choice.message.content.as_deref()) {
        println!("{content}");
    }
    model.unload().await?;
    Ok(())
}`,
			html: `    <span class="code-keyword">let</span> messages = vec![
        <span class="code-type">ChatCompletionRequestUserMessage</span>::<span class="code-call">from</span>(<span class="code-string">"Hello!"</span>).<span class="code-call">into</span>()
    ];
    <span class="code-keyword">let</span> response = client.<span class="code-call">complete_chat</span>(&messages, None).await?;
    <span class="code-keyword">if let</span> Some(content) = response.choices.<span class="code-call">first</span>().<span class="code-call">and_then</span>(|choice| choice.message.content.<span class="code-call">as_deref</span>()) {
        <span class="code-call">println!</span>(<span class="code-string">"{content}"</span>);
    }
    model.<span class="code-call">unload</span>().await?;
    Ok(())
}`
		}
	};

	let copiedCommand = $state(false);
	let copiedExample = $state(false);
	let activeTab = $state<SdkLanguage>('python');
	let showExample = $state(false);

	let currentCommand = $derived(sdkCommands[activeTab]);
	let currentFilename = $derived(filenames[activeTab]);
	let currentExample = $derived({
		raw: `${setupSnippets[activeTab].raw}\n\n${inferenceSnippets[activeTab].raw}`,
		html: `${setupSnippets[activeTab].html}\n\n${inferenceSnippets[activeTab].html}`
	});

	function revealMotion(node: HTMLElement) {
		const height = node.offsetHeight;
		const shouldReduceMotion =
			typeof window !== 'undefined' &&
			window.matchMedia('(prefers-reduced-motion: reduce)').matches;

		return {
			duration: shouldReduceMotion ? 0 : 240,
			easing: cubicOut,
			css: (t: number) => `
				max-height: ${t * height}px;
				opacity: ${t};
				overflow: hidden;
				transform: translateY(${(1 - t) * -6}px);
			`
		};
	}

	async function copyCommand() {
		try {
			await navigator.clipboard.writeText(currentCommand);
			copiedCommand = true;
			showExample = true;
			toast.success('Copied install command');
			setTimeout(() => {
				copiedCommand = false;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy install command');
		}
	}

	async function copyExample() {
		try {
			await navigator.clipboard.writeText(currentExample.raw);
			copiedExample = true;
			toast.success('Copied complete example');
			setTimeout(() => {
				copiedExample = false;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy example');
		}
	}
</script>

<div
	class="border-primary/20 bg-primary/5 hover:border-primary/40 relative w-full rounded-xl border p-4 transition-all duration-300 hover:shadow-lg sm:p-5"
>
	<div class="mb-4 text-center">
		<h2 class="text-foreground text-sm font-semibold sm:text-base">Start with the SDK</h2>
		<p class="text-muted-foreground mx-auto mt-1 max-w-xl text-xs">
			Install the SDK, load a model, and run inference in-process.
		</p>
	</div>

	<div class="mb-4 flex flex-wrap justify-center gap-1">
		{#each tabs as tab}
			<button
				type="button"
				aria-pressed={activeTab === tab.key}
				class="min-h-9 rounded-md px-3 py-1.5 text-xs font-medium transition-all duration-200 {activeTab ===
				tab.key
					? 'bg-primary text-primary-foreground shadow-sm'
					: 'text-muted-foreground hover:text-foreground hover:bg-muted'}"
				onclick={() => {
					activeTab = tab.key;
					copiedCommand = false;
					copiedExample = false;
				}}
			>
				{tab.label}
			</button>
		{/each}
	</div>

	<div class="grid gap-3">
		<section class="border-primary/20 bg-background/55 rounded-lg border p-3">
			<div class="text-muted-foreground mb-2 flex items-center gap-2 text-xs font-medium">
				<span
					class="bg-primary text-primary-foreground flex size-5 items-center justify-center rounded-full font-mono text-[11px]"
					aria-hidden="true">1</span
				>
				<Terminal class="text-primary size-4" aria-hidden="true" />
				<span>Install package</span>
			</div>
			<div
				class="group border-border bg-muted/40 flex items-center gap-2 rounded-md border px-3 py-2.5"
			>
				<code class="text-foreground min-w-0 flex-1 overflow-x-auto font-mono text-xs select-all">
					{currentCommand}
				</code>
				<button
					type="button"
					onclick={copyCommand}
					class="bg-primary/10 text-primary hover:bg-primary/20 focus:ring-primary flex shrink-0 items-center gap-1 rounded px-2 py-1.5 text-xs font-medium transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
					aria-label="Copy installation command"
				>
					{#if copiedCommand}
						<Check class="size-3.5" aria-hidden="true" />
					{:else}
						<Copy class="size-3.5" aria-hidden="true" />
					{/if}
				</button>
			</div>
		</section>

		{#if showExample}
			<section
				transition:revealMotion
				class="border-primary/20 bg-background/55 rounded-lg border p-3"
			>
				<div class="mb-2 flex items-center justify-between gap-3">
					<div class="text-muted-foreground flex items-center gap-2 text-xs font-medium">
						<span
							class="bg-primary text-primary-foreground flex size-5 items-center justify-center rounded-full font-mono text-[11px]"
							aria-hidden="true">2</span
						>
						<Code2 class="text-primary size-4" aria-hidden="true" />
						<span>Run your first chat</span>
					</div>
					<span class="text-muted-foreground ml-auto hidden font-mono text-[11px] sm:inline">
						{currentFilename}
					</span>
					<button
						type="button"
						onclick={copyExample}
						class="bg-primary/10 text-primary hover:bg-primary/20 focus:ring-primary flex shrink-0 items-center gap-1 rounded px-2 py-1 text-xs font-medium transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
						aria-label="Copy complete example"
					>
						{#if copiedExample}
							<Check class="size-3.5" aria-hidden="true" />
							Copied
						{:else}
							<Copy class="size-3.5" aria-hidden="true" />
							Copy
						{/if}
					</button>
				</div>
				<pre
					class="snippet border-border bg-muted/40 text-foreground max-h-80 overflow-auto rounded-md border p-3 font-mono text-[11px] leading-relaxed sm:text-xs"><code
						>{@html currentExample.html}</code
					></pre>
			</section>
		{:else}
			<button
				type="button"
				transition:revealMotion
				class="border-border/80 bg-background/35 hover:border-primary/35 hover:bg-primary/5 focus:ring-primary text-muted-foreground hover:text-foreground w-full rounded-lg border border-dashed p-3 text-left transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
				onclick={() => (showExample = true)}
				aria-label="Reveal complete example"
			>
				<span class="flex items-center gap-2 text-xs">
					<span
						class="border-border bg-muted flex size-5 shrink-0 items-center justify-center rounded-full border font-mono text-[11px]"
						aria-hidden="true">2</span
					>
					<Code2 class="text-muted-foreground/60 size-4 shrink-0" aria-hidden="true" />
					<span class="font-medium">Run your first chat</span>
					<span class="text-muted-foreground ml-auto hidden shrink-0 sm:inline">view example</span>
				</span>
			</button>
		{/if}
	</div>
</div>

<style>
	.snippet {
		scrollbar-width: thin;
	}

	.snippet :global(.code-keyword) {
		color: hsl(var(--primary));
		font-weight: 600;
	}

	.snippet :global(.code-type) {
		color: hsl(var(--warning));
		font-weight: 600;
	}

	.snippet :global(.code-call) {
		color: hsl(var(--info));
	}

	.snippet :global(.code-string) {
		color: hsl(var(--success));
	}
</style>
