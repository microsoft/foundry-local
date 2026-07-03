<script lang="ts">
	import type { GroupedFoundryModel } from '../types';
	import { Badge } from '$lib/components/ui/badge';
	import { Calendar, Copy, Check } from 'lucide-svelte';
	import * as Card from '$lib/components/ui/card';
	import { animate } from '$lib/utils/animations';
	import { foundryModelService } from '../service';
	import { generateModelDescription } from '$lib/utils/model-description';
	import ModelStarterCode from './ModelStarterCode.svelte';

	export let model: GroupedFoundryModel;
	export let copiedModelId: string | null = null;
	export let onCardClick: (model: GroupedFoundryModel) => void;
	export let onCopyCommand: (modelId: string) => void;

	const DEVICE_SUFFIX_PATTERN = /-(generic|cuda|qnn|openvino|vitis)-(cpu|gpu|npu)$|-(cpu|gpu|npu)$/i;

	function getGenericModelName(): string {
		const baseName = model.alias || model.variants[0]?.name || model.displayName;
		return baseName.split(':')[0].replace(DEVICE_SUFFIX_PATTERN, '');
	}

	function isSpeechToTextModel(): boolean {
		const taskType = model.taskType?.toLowerCase() || '';
		const alias = model.alias?.toLowerCase() || '';
		const displayName = model.displayName?.toLowerCase() || '';
		return (
			taskType.includes('automatic-speech-recognition') ||
			taskType.includes('speech-to-text') ||
			alias.includes('whisper') ||
			displayName.includes('whisper')
		);
	}

	$: displayDescription = generateModelDescription(model);
	$: genericModelName = getGenericModelName();
	$: isSpeechToText = isSpeechToTextModel();
</script>

<div use:animate={{ delay: 0, duration: 600, animation: 'fade-in', once: true }} class="flex">
	<Card.Root
		class="border-border/40 hover:border-primary/50 focus:ring-primary relative z-0 flex flex-1 cursor-pointer flex-col transition-all duration-300 focus-within:z-20 hover:z-20 focus:ring-2 focus:ring-offset-2 focus:outline-none"
		onclick={() => onCardClick(model)}
		onkeydown={(e) => {
			if (e.key === 'Enter' || e.key === ' ') {
				e.preventDefault();
				onCardClick(model);
			}
		}}
		role="button"
		tabindex="0"
	>
		<Card.Header class="pb-3">
			<Card.Title class="line-clamp-1 text-lg">
				{model.displayName}
			</Card.Title>
			<!-- Publisher with Date and Version -->
			<div class="text-muted-foreground flex flex-wrap items-center gap-1.5 pt-1 text-xs">
				<span>{model.publisher}</span>
				<span>•</span>
				<div class="flex shrink-0 items-center gap-1">
					<Calendar class="size-3" aria-hidden="true" />
					<span class="whitespace-nowrap"
						>{new Date(model.lastModified).toLocaleDateString('en-US', {
							month: 'short',
							year: 'numeric'
						})}</span
					>
				</div>
				<span>•</span>
				<span>V{model.latestVersion}</span>
			</div>
		</Card.Header>
		<Card.Content class="flex flex-1 flex-col pt-0">
			<p class="mb-3 line-clamp-3 min-h-[3.75rem] text-sm text-gray-600 dark:text-gray-400">
				{displayDescription}
			</p>

			<!-- Badges - Task Type, File Size, License -->
			<div class="mb-3 flex min-h-[1.5rem] flex-row flex-wrap items-center gap-1">
				{#if model.fileSizeBytes}
					<Badge variant="secondary" class="flex shrink-0 items-center gap-1 text-xs">
						<svg class="size-3" fill="currentColor" viewBox="0 0 20 20" aria-hidden="true">
							<path
								fill-rule="evenodd"
								d="M4 4a2 2 0 012-2h4.586A2 2 0 0112 2.586L15.414 6A2 2 0 0116 7.414V16a2 2 0 01-2 2H6a2 2 0 01-2-2V4zm2 6a1 1 0 011-1h6a1 1 0 110 2H7a1 1 0 01-1-1zm1 3a1 1 0 100 2h6a1 1 0 100-2H7z"
								clip-rule="evenodd"
							/>
						</svg>
						{model.modelSize}
					</Badge>
				{/if}
				{#if model.taskType}
					<Badge variant="secondary" class="shrink-0 text-xs">{model.taskType}</Badge>
				{/if}
				{#if model.license}
					{@const licenseUrl = foundryModelService.getLicenseUrl(model.license)}
					{#if licenseUrl}
						<a
							href={licenseUrl}
							target="_blank"
							rel="noopener noreferrer"
							onclick={(e) => e.stopPropagation()}
							class="inline-block"
							aria-label={`View ${model.license} license (opens in new tab)`}
						>
							<Badge
								variant="outline"
								class="hover:bg-primary/10 flex shrink-0 items-center gap-1 text-xs transition-colors"
							>
								<svg class="size-3" fill="currentColor" viewBox="0 0 20 20" aria-hidden="true">
									<path d="M9 2a1 1 0 000 2h2a1 1 0 100-2H9z" />
									<path
										fill-rule="evenodd"
										d="M4 5a2 2 0 012-2 3 3 0 003 3h2a3 3 0 003-3 2 2 0 012 2v11a2 2 0 01-2 2H6a2 2 0 01-2-2V5zm3 4a1 1 0 000 2h.01a1 1 0 100-2H7zm3 0a1 1 0 000 2h3a1 1 0 100-2h-3zm-3 4a1 1 0 100 2h.01a1 1 0 100-2H7zm3 0a1 1 0 100 2h3a1 1 0 100-2h-3z"
										clip-rule="evenodd"
									/>
								</svg>
								{model.license}
							</Badge>
						</a>
					{:else}
						<Badge variant="outline" class="flex shrink-0 items-center gap-1 text-xs">
							<svg class="size-3" fill="currentColor" viewBox="0 0 20 20" aria-hidden="true">
								<path d="M9 2a1 1 0 000 2h2a1 1 0 100-2H9z" />
								<path
									fill-rule="evenodd"
									d="M4 5a2 2 0 012-2 3 3 0 003 3h2a3 3 0 003-3 2 2 0 012 2v11a2 2 0 01-2 2H6a2 2 0 01-2-2V5zm3 4a1 1 0 000 2h.01a1 1 0 100-2H7zm3 0a1 1 0 000 2h3a1 1 0 100-2h-3zm-3 4a1 1 0 100 2h.01a1 1 0 100-2H7zm3 0a1 1 0 100 2h3a1 1 0 100-2h-3z"
									clip-rule="evenodd"
								/>
							</svg>
							{model.license}
						</Badge>
					{/if}
				{/if}
			</div>

			<!-- Run command + starter code strip -->
			<div class="border-border/40 space-y-3 border-t pt-3">
				{#if isSpeechToText}
					<div class="flex items-center gap-1.5">
						<span
							class="bg-primary/10 text-primary rounded px-1.5 py-0.5 text-[10px] font-semibold"
						>SDK only</span>
						<span class="text-muted-foreground text-[11px]">Not available via CLI</span>
					</div>
				{:else}
					<div>
						<div class="text-muted-foreground mb-1 text-[10px] font-semibold tracking-wide uppercase">
							Run with CLI
						</div>
						<div class="flex min-w-0 items-center gap-2">
							<code
								class="text-muted-foreground min-w-0 flex-1 truncate font-mono text-[11px]"
								title="foundry run {genericModelName}"
							>
								foundry run {genericModelName}
							</code>
							<button
								type="button"
								onclick={(e) => {
									e.stopPropagation();
									onCopyCommand(genericModelName);
								}}
								class="border-border text-muted-foreground hover:text-foreground hover:border-primary/50 hover:bg-primary/5 flex shrink-0 items-center gap-1 rounded border px-2 py-1 text-xs transition-colors"
								aria-label="Copy run command for {genericModelName}"
							>
								{#if copiedModelId === `run-${genericModelName}`}
									<Check class="size-3 text-green-500" aria-hidden="true" />
									<span>Copied</span>
								{:else}
									<Copy class="size-3" aria-hidden="true" />
									<span>Run</span>
								{/if}
							</button>
						</div>
					</div>
				{/if}
				<div>
					<div class="text-muted-foreground mb-1 text-[10px] font-semibold tracking-wide uppercase">
						Starter code
					</div>
					<ModelStarterCode {model} compact />
				</div>
			</div>
		</Card.Content>
	</Card.Root>
</div>

<style>
	:global(.line-clamp-1) {
		display: -webkit-box;
		-webkit-line-clamp: 1;
		line-clamp: 1;
		-webkit-box-orient: vertical;
		overflow: hidden;
	}

	:global(.line-clamp-2) {
		display: -webkit-box;
		-webkit-line-clamp: 2;
		line-clamp: 2;
		-webkit-box-orient: vertical;
		overflow: hidden;
	}

	:global(.line-clamp-3) {
		display: -webkit-box;
		-webkit-line-clamp: 3;
		line-clamp: 3;
		-webkit-box-orient: vertical;
		overflow: hidden;
	}

	:global(.accelerator-logo-mask) {
		display: inline-block;
		background-color: var(--logo-color, currentColor);
		-webkit-mask: var(--logo-url) no-repeat center;
		mask: var(--logo-url) no-repeat center;
		-webkit-mask-size: contain;
		mask-size: contain;
	}
</style>
