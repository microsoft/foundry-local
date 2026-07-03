<script lang="ts">
	import { Copy, Check } from 'lucide-svelte';
	import { toast } from 'svelte-sonner';
	import type { GroupedFoundryModel } from '../types';
	import type { StarterLanguage } from '../model-boilerplate';
	import {
		STARTER_LANGUAGES,
		getStarterSnippet,
		getStarterKindLabel,
		getStarterHint
	} from '../model-boilerplate';

	export let model: GroupedFoundryModel;
	/** true → card-mode: language picker + single copy button, no code preview */
	export let compact: boolean = false;

	let activeLanguage: StarterLanguage = 'python';
	let copied = false;

	$: snippet = getStarterSnippet(model, activeLanguage);
	$: kindLabel = getStarterKindLabel(snippet.kind);
	$: hint = getStarterHint(snippet.kind);

	async function copySnippet() {
		try {
			await navigator.clipboard.writeText(snippet.code);
			copied = true;
			toast.success('Starter code copied');
			setTimeout(() => {
				copied = false;
			}, 2000);
		} catch {
			toast.error('Failed to copy');
		}
	}
</script>

{#if compact}
	<!-- Card-mode: one row with language pills + copy button -->
	<div class="flex items-center gap-2">
		<div class="flex min-w-0 flex-1 flex-wrap gap-1">
			{#each STARTER_LANGUAGES as lang}
				<button
					type="button"
					class="rounded px-2 py-0.5 text-[11px] font-medium transition-colors duration-150 {activeLanguage ===
					lang.id
						? 'bg-primary text-primary-foreground'
						: 'text-muted-foreground hover:text-foreground hover:bg-muted'}"
					onclick={(e) => {
						e.stopPropagation();
						activeLanguage = lang.id;
					}}
					aria-label="Select {lang.label}"
					aria-pressed={activeLanguage === lang.id}
				>
					{lang.shortLabel}
				</button>
			{/each}
		</div>
		<button
			type="button"
			class="border-border text-muted-foreground hover:text-foreground hover:border-primary/50 hover:bg-primary/5 flex shrink-0 items-center gap-1 rounded border px-2 py-1 text-xs transition-colors"
			onclick={(e) => {
				e.stopPropagation();
				copySnippet();
			}}
			aria-label="Copy {activeLanguage} starter code"
		>
			{#if copied}
				<Check class="size-3 text-green-500" aria-hidden="true" />
				<span>Copied</span>
			{:else}
				<Copy class="size-3" aria-hidden="true" />
				<span>Copy</span>
			{/if}
		</button>
	</div>
{:else}
	<!-- Modal-mode: full panel with language tabs, code block, and copy -->
	<div class="space-y-3">
		<div class="flex items-center justify-between gap-3">
			<div>
				<h3 class="text-base font-semibold">Get started — {kindLabel}</h3>
				<p class="text-muted-foreground mt-0.5 text-xs">{hint}</p>
				{#if snippet.kind === 'audio'}
					<p class="text-primary mt-1 text-[11px] font-medium">
						SDK only — not available via <code class="font-mono">foundry model run</code>
					</p>
				{/if}
			</div>
			<button
				type="button"
				class="bg-primary/10 text-primary hover:bg-primary/20 focus:ring-primary flex shrink-0 items-center gap-1.5 rounded px-3 py-1.5 text-xs font-medium transition-colors focus:ring-2 focus:ring-offset-2 focus:outline-none"
				onclick={copySnippet}
				aria-label="Copy {activeLanguage} starter code for {model.displayName}"
			>
				{#if copied}
					<Check class="size-3.5 text-green-500" aria-hidden="true" />
					Copied
				{:else}
					<Copy class="size-3.5" aria-hidden="true" />
					Copy code
				{/if}
			</button>
		</div>

		<!-- Language tabs -->
		<div class="border-border -mx-1 flex gap-0.5 border-b px-1">
			{#each STARTER_LANGUAGES as lang}
				<button
					type="button"
					aria-pressed={activeLanguage === lang.id}
					class="relative -mb-px rounded-t px-3 py-1.5 text-xs font-medium transition-colors {activeLanguage ===
					lang.id
						? 'border-border border-x border-t text-foreground bg-background'
						: 'text-muted-foreground hover:text-foreground'}"
					onclick={() => (activeLanguage = lang.id)}
				>
					{lang.label}
				</button>
			{/each}
		</div>

		<!-- Code block -->
		<div class="border-border bg-muted/40 relative overflow-hidden rounded-lg border">
			<div class="text-muted-foreground border-border flex items-center justify-between border-b px-3 py-1.5 text-[11px]">
				<span class="font-mono">{snippet.fileName}</span>
				<span class="font-mono text-[10px] opacity-60">{snippet.installCommand}</span>
			</div>
			<pre
				class="max-h-64 overflow-auto p-4 font-mono text-[11px] leading-relaxed sm:text-xs"><code class="text-foreground">{snippet.code}</code></pre>
		</div>
	</div>
{/if}
