<script lang="ts">
	import { Bot, Copy, Check, Terminal, Code2 } from 'lucide-svelte';
	import { toast } from 'svelte-sonner';
	import { cubicOut } from 'svelte/easing';

	type SdkLanguage = 'python' | 'javascript' | 'csharp' | 'rust';

	type Snippet = {
		raw: string;
		html: string;
	};

	const tabs: Array<{ key: SdkLanguage; label: string }> = [
		{ key: 'python', label: 'Python' },
		{ key: 'javascript', label: 'JS' },
		{ key: 'csharp', label: 'C#' },
		{ key: 'rust', label: 'Rust' }
	];

	const sdkCommands: Record<SdkLanguage, string> = {
		python: 'pip install foundry-local-sdk',
		javascript: 'npm install foundry-local-sdk',
		csharp: 'dotnet add package Microsoft.AI.Foundry.Local',
		rust: 'cargo add foundry-local-sdk'
	};

	const setupSnippets: Record<SdkLanguage, Snippet> = {
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
			raw: `const manager = FoundryLocalManager.create({ appName: 'my-app' });
const model = await manager.catalog.getModel('qwen2.5-0.5b');
await model.download(); await model.load();
const chat = model.createChatClient();`,
			html: `<span class="code-keyword">const</span> manager = <span class="code-type">FoundryLocalManager</span>.<span class="code-call">create</span>({ appName: <span class="code-string">'my-app'</span> });
<span class="code-keyword">const</span> model = <span class="code-keyword">await</span> manager.catalog.<span class="code-call">getModel</span>(<span class="code-string">'qwen2.5-0.5b'</span>);
<span class="code-keyword">await</span> model.<span class="code-call">download</span>(); <span class="code-keyword">await</span> model.<span class="code-call">load</span>();
<span class="code-keyword">const</span> chat = model.<span class="code-call">createChatClient</span>();`
		},
		csharp: {
			raw: `using Microsoft.AI.Foundry.Local;
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
			html: `<span class="code-keyword">using</span> <span class="code-type">Microsoft.AI.Foundry.Local</span>;
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
			raw: `let manager = FoundryLocalManager::create(FoundryLocalConfig::new("my-app"))?;
let model = manager.catalog().get_model("qwen2.5-0.5b").await?;
model.download(None::<fn(f64)>).await?; model.load().await?;
let client = model.create_chat_client();`,
			html: `<span class="code-keyword">let</span> manager = <span class="code-type">FoundryLocalManager</span>::<span class="code-call">create</span>(<span class="code-type">FoundryLocalConfig</span>::<span class="code-call">new</span>(<span class="code-string">"my-app"</span>))?;
<span class="code-keyword">let</span> model = manager.<span class="code-call">catalog</span>().<span class="code-call">get_model</span>(<span class="code-string">"qwen2.5-0.5b"</span>).await?;
model.<span class="code-call">download</span>(None::&lt;fn(f64)&gt;).await?; model.<span class="code-call">load</span>().await?;
<span class="code-keyword">let</span> client = model.<span class="code-call">create_chat_client</span>();`
		}
	};

	const inferenceSnippets: Record<SdkLanguage, Snippet> = {
		python: {
			raw: `response = client.complete_chat([
    {"role": "user", "content": "Hello!"}
])
print(response.choices[0].message.content)`,
			html: `response = client.<span class="code-call">complete_chat</span>([
    {<span class="code-string">"role"</span>: <span class="code-string">"user"</span>, <span class="code-string">"content"</span>: <span class="code-string">"Hello!"</span>}
])
<span class="code-call">print</span>(response.choices[0].message.content)`
		},
		javascript: {
			raw: `const response = await chat.completeChat([
  { role: 'user', content: 'Hello!' }
]);
console.log(response.choices[0]?.message?.content);`,
			html: `<span class="code-keyword">const</span> response = <span class="code-keyword">await</span> chat.<span class="code-call">completeChat</span>([
  { role: <span class="code-string">'user'</span>, content: <span class="code-string">'Hello!'</span> }
]);
console.<span class="code-call">log</span>(response.choices[0]?.message?.content);`
		},
		csharp: {
			raw: `var response = await chat.CompleteChatAsync(new[] {
    new ChatMessage { Role = "user", Content = "Hello!" }
});
Console.WriteLine(response.Choices![0].Message.Content);`,
			html: `<span class="code-keyword">var</span> response = <span class="code-keyword">await</span> chat.<span class="code-call">CompleteChatAsync</span>(new[] {
    <span class="code-keyword">new</span> <span class="code-type">ChatMessage</span> { Role = <span class="code-string">"user"</span>, Content = <span class="code-string">"Hello!"</span> }
});
<span class="code-type">Console</span>.<span class="code-call">WriteLine</span>(response.Choices![0].Message.Content);`
		},
		rust: {
			raw: `let messages = vec![
    ChatCompletionRequestUserMessage::from("Hello!").into()
];
let response = client.complete_chat(&messages, None).await?;`,
			html: `<span class="code-keyword">let</span> messages = vec![
    <span class="code-type">ChatCompletionRequestUserMessage</span>::<span class="code-call">from</span>(<span class="code-string">"Hello!"</span>).<span class="code-call">into</span>()
];
<span class="code-keyword">let</span> response = client.<span class="code-call">complete_chat</span>(&messages, None).await?;`
		}
	};

	let copiedCommand = $state(false);
	let copiedSetup = $state(false);
	let copiedInference = $state(false);
	let activeTab = $state<SdkLanguage>('python');
	let revealedStep = $state(1);

	let currentCommand = $derived(sdkCommands[activeTab]);
	let currentSetupSnippet = $derived(setupSnippets[activeTab]);
	let currentInferenceSnippet = $derived(inferenceSnippets[activeTab]);

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

	function revealStep(step: number) {
		if (revealedStep < step) {
			revealedStep = step;
		}
	}

	async function copyCommand() {
		try {
			await navigator.clipboard.writeText(currentCommand);
			copiedCommand = true;
			revealStep(2);
			toast.success('Copied install command');
			setTimeout(() => {
				copiedCommand = false;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy install command');
		}
	}

	async function copySetupSnippet() {
		try {
			await navigator.clipboard.writeText(currentSetupSnippet.raw);
			copiedSetup = true;
			revealStep(3);
			toast.success('Copied model setup');
			setTimeout(() => {
				copiedSetup = false;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy model setup');
		}
	}

	async function copyInferenceSnippet() {
		try {
			await navigator.clipboard.writeText(currentInferenceSnippet.raw);
			copiedInference = true;
			toast.success('Copied inference call');
			setTimeout(() => {
				copiedInference = false;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy inference call');
		}
	}
</script>

<div
	class="border-primary/20 bg-primary/5 hover:border-primary/40 relative w-full rounded-xl border p-4 transition-all duration-300 hover:shadow-lg sm:p-5"
>
	<div class="mb-4 text-center">
		<h2 class="text-foreground text-sm font-semibold sm:text-base">Start with the SDK</h2>
		<p class="text-muted-foreground mx-auto mt-1 max-w-xl text-xs">
			Install one package, load a model, then run inference in-process.
		</p>
	</div>

	<div class="mb-4 flex justify-center gap-1">
		{#each tabs as tab}
			<button
				type="button"
				class="min-h-9 rounded-md px-3 py-1.5 text-xs font-medium transition-all duration-200 {activeTab ===
				tab.key
					? 'bg-primary text-primary-foreground shadow-sm'
					: 'text-muted-foreground hover:text-foreground hover:bg-muted'}"
				onclick={() => {
					activeTab = tab.key;
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

		{#if revealedStep >= 2}
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
						<span>Load a model</span>
					</div>
					<button
						type="button"
						onclick={copySetupSnippet}
						class="bg-primary/10 text-primary hover:bg-primary/20 focus:ring-primary flex shrink-0 items-center gap-1 rounded px-2 py-1 text-xs font-medium transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
						aria-label="Copy model setup"
					>
						{#if copiedSetup}
							<Check class="size-3.5" aria-hidden="true" />
							Copied
						{:else}
							<Copy class="size-3.5" aria-hidden="true" />
							Copy
						{/if}
					</button>
				</div>
				<pre
					class="snippet border-border bg-muted/40 text-foreground max-h-40 overflow-auto rounded-md border p-3 font-mono text-[11px] leading-relaxed sm:text-xs"><code
						>{@html currentSetupSnippet.html}</code
					></pre>
			</section>
		{:else}
			<button
				type="button"
				transition:revealMotion
				class="border-border/80 bg-background/35 hover:border-primary/35 hover:bg-primary/5 focus:ring-primary text-muted-foreground hover:text-foreground w-full rounded-lg border border-dashed p-3 text-left transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
				onclick={() => revealStep(2)}
				aria-label="Reveal model setup"
			>
				<span class="flex items-center gap-2 text-xs">
					<span
						class="border-border bg-muted flex size-5 shrink-0 items-center justify-center rounded-full border font-mono text-[11px]"
						aria-hidden="true">2</span
					>
					<Code2 class="text-muted-foreground/60 size-4 shrink-0" aria-hidden="true" />
					<span class="font-medium">Load a model</span>
					<span class="text-muted-foreground ml-auto hidden shrink-0 sm:inline"
						>copy step 1 or click to reveal</span
					>
				</span>
			</button>
		{/if}

		{#if revealedStep >= 3}
			<section
				transition:revealMotion
				class="border-primary/20 bg-background/55 rounded-lg border p-3"
			>
				<div class="mb-2 flex items-center justify-between gap-3">
					<div class="text-muted-foreground flex items-center gap-2 text-xs font-medium">
						<span
							class="bg-primary text-primary-foreground flex size-5 items-center justify-center rounded-full font-mono text-[11px]"
							aria-hidden="true">3</span
						>
						<Bot class="text-primary size-4" aria-hidden="true" />
						<span>Run chat inference</span>
					</div>
					<button
						type="button"
						onclick={copyInferenceSnippet}
						class="bg-primary/10 text-primary hover:bg-primary/20 focus:ring-primary flex shrink-0 items-center gap-1 rounded px-2 py-1 text-xs font-medium transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
						aria-label="Copy inference call"
					>
						{#if copiedInference}
							<Check class="size-3.5" aria-hidden="true" />
							Copied
						{:else}
							<Copy class="size-3.5" aria-hidden="true" />
							Copy
						{/if}
					</button>
				</div>
				<pre
					class="snippet border-border bg-muted/40 text-foreground max-h-40 overflow-auto rounded-md border p-3 font-mono text-[11px] leading-relaxed sm:text-xs"><code
						>{@html currentInferenceSnippet.html}</code
					></pre>
			</section>
		{:else}
			<button
				type="button"
				transition:revealMotion
				class="border-border/80 bg-background/35 hover:border-primary/35 hover:bg-primary/5 focus:ring-primary text-muted-foreground hover:text-foreground w-full rounded-lg border border-dashed p-3 text-left transition-all duration-200 focus:ring-2 focus:ring-offset-2 focus:outline-none"
				onclick={() => revealStep(3)}
				aria-label="Reveal chat inference call"
			>
				<span class="flex items-center gap-2 text-xs">
					<span
						class="border-border bg-muted flex size-5 shrink-0 items-center justify-center rounded-full border font-mono text-[11px]"
						aria-hidden="true">3</span
					>
					<Bot class="text-muted-foreground/60 size-4 shrink-0" aria-hidden="true" />
					<span class="font-medium">Run chat inference</span>
					<span class="text-muted-foreground ml-auto hidden shrink-0 sm:inline"
						>copy step 2 or click to reveal</span
					>
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
