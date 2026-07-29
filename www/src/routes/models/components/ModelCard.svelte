<script lang="ts">
	import type { GroupedFoundryModel } from '../types';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import { Calendar, Check } from 'lucide-svelte';
	import * as Card from '$lib/components/ui/card';
	import * as Tooltip from '$lib/components/ui/tooltip';
	import { animate } from '$lib/utils/animations';

	export let model: GroupedFoundryModel;
	export let copiedModelId: string | null = null;
	export let onCardClick: (model: GroupedFoundryModel) => void;
	export let onCopyCommand: (modelId: string) => void;

	import { foundryModelService } from '../service';

	import {
		getDeviceIcon,
		getAcceleratorLogo,
		getAcceleratorColor,
		getVariantLabel
	} from '$lib/utils/model-helpers';
	import { generateModelDescription } from '$lib/utils/model-description';

	function getUniqueVariants() {
		if (!model.variants || model.variants.length === 0) return [];

		const uniqueMap = new Map();
		for (const variant of model.variants) {
			if (!uniqueMap.has(variant.name)) {
				uniqueMap.set(variant.name, variant);
			}
		}
		return Array.from(uniqueMap.values());
	}

	function sortVariantsByDevice(variants: any[]) {
		const devicePriority: Record<string, number> = {
			cpu: 1,
			gpu: 2,
			npu: 3
		};

		return [...variants].sort((a, b) => {
			const aDevice = a.deviceSupport[0]?.toLowerCase() || 'zzz';
			const bDevice = b.deviceSupport[0]?.toLowerCase() || 'zzz';

			const aPriority = devicePriority[aDevice] || 99;
			const bPriority = devicePriority[bDevice] || 99;

			return aPriority - bPriority;
		});
	}

	function formatModelCommand(modelId: string): string {
		return `foundry run ${modelId}`;
	}

	// Device suffix pattern for cleaning model names
	const DEVICE_SUFFIX_PATTERN = /-(generic|cuda|qnn|openvino|vitis)-(cpu|gpu|npu)$|-(cpu|gpu|npu)$/i;

	// Get the generic model name for auto-selection
	function getGenericModelName(): string {
		const baseName = model.alias || model.variants[0]?.name || model.displayName;
		const withoutVersion = baseName.split(':')[0];
		return withoutVersion.replace(DEVICE_SUFFIX_PATTERN, '');
	}

	$: uniqueVariants = sortVariantsByDevice(getUniqueVariants());
	$: displayDescription = generateModelDescription(model);
	$: genericModelName = getGenericModelName();
	
	// Check if model is speech-to-text
	function isSpeechToTextModel(): boolean {
		const taskType = model.taskType?.toLowerCase() || '';
		const alias = model.alias?.toLowerCase() || '';
		const displayName = model.displayName?.toLowerCase() || '';
		
		return taskType.includes('automatic-speech-recognition') || 
			taskType.includes('speech-to-text') ||
			alias.includes('whisper') ||
			displayName.includes('whisper');
	}
	
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

			<!-- Copy Run Command Section -->
			<div class="border-border/40 border-t pt-3">
				{#if isSpeechToText}
					<!-- SDK Only Notice for Speech-to-Text Models -->
					<div class="space-y-2">
						<div class="bg-gradient-to-r from-violet-500/10 to-purple-500/10 rounded-lg p-3 border border-violet-500/20">
							<div class="flex items-center gap-2 mb-2">
								<svg class="size-4 text-violet-500" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
									<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 11a7 7 0 01-7 7m0 0a7 7 0 01-7-7m7 7v4m0 0H8m4 0h4m-4-8a3 3 0 01-3-3V5a3 3 0 116 0v6a3 3 0 01-3 3z" />
								</svg>
								<span class="text-xs font-semibold text-violet-600 dark:text-violet-400">SDK Only</span>
							</div>
							<div class="font-mono text-xs text-muted-foreground mb-2">
								Model ID: <span class="text-foreground font-medium">{genericModelName}</span>
							</div>
							<a
								href="https://learn.microsoft.com/en-us/azure/ai-foundry/foundry-local/how-to/how-to-transcribe-audio?view=foundry-classic&tabs=windows"
								target="_blank"
								rel="noopener noreferrer"
								onclick={(e) => e.stopPropagation()}
								class="inline-flex items-center gap-1.5 text-xs font-medium text-violet-600 dark:text-violet-400 hover:text-violet-700 dark:hover:text-violet-300 transition-colors"
								aria-label="View SDK Documentation (opens in new tab)"
							>
								<svg class="size-3" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
									<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 6.253v13m0-13C10.832 5.477 9.246 5 7.5 5S4.168 5.477 3 6.253v13C4.168 18.477 5.754 18 7.5 18s3.332.477 4.5 1.253m0-13C13.168 5.477 14.754 5 16.5 5c1.747 0 3.332.477 4.5 1.253v13C19.832 18.477 18.247 18 16.5 18c-1.746 0-3.332.477-4.5 1.253" />
								</svg>
								View SDK Documentation
								<svg class="size-3" fill="none" stroke="currentColor" viewBox="0 0 24 24" aria-hidden="true">
									<path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14" />
								</svg>
							</a>
						</div>
					</div>
				{:else if uniqueVariants.length > 0}
					<div class="space-y-1.5">
						<div class="text-xs font-medium text-gray-600 dark:text-gray-400">
							Copy Run Command:
						</div>

						<!-- Default/Generic Run Command -->
						<Tooltip.Root>
							<Tooltip.Trigger>
								{#snippet child({ props })}
									<Button
										{...props}
										variant="outline"
										size="sm"
										onclick={(e) => {
											e.stopPropagation();
											onCopyCommand(genericModelName);
										}}
										class="border-primary text-primary hover:bg-primary/10 group relative h-8 w-full justify-start gap-2 overflow-hidden border-2 px-3 text-xs font-semibold"
									>
										{#if copiedModelId === `run-${genericModelName}`}
											<!-- Success State -->
											<div
												class="animate-in fade-in absolute inset-0 bg-gradient-to-r from-purple-500/20 to-violet-500/20 duration-300"
											></div>
											<Check class="relative z-10 size-4 shrink-0 text-green-500" />
											<span class="relative z-10 min-w-0 truncate">Copied!</span>
										{:else}
											<!-- Animated gradient overlay on hover/click -->
											<div
												class="from-primary/0 via-primary/20 to-primary/0 absolute inset-0 translate-x-[-100%] bg-gradient-to-r transition-transform duration-700 ease-in-out group-hover:translate-x-[100%]"
											></div>
											<svg
												class="relative z-10 size-4 shrink-0"
												fill="currentColor"
												viewBox="0 0 20 20"
											>
												<path
													fill-rule="evenodd"
													d="M11.3 1.046A1 1 0 0112 2v5h4a1 1 0 01.82 1.573l-7 10A1 1 0 018 18v-5H4a1 1 0 01-.82-1.573l7-10a1 1 0 011.12-.38z"
													clip-rule="evenodd"
												/>
											</svg>
											<span class="relative z-10 min-w-0 truncate">Auto-select Best Variant</span>
										{/if}
									</Button>
								{/snippet}
							</Tooltip.Trigger>
							<Tooltip.Portal>
								<Tooltip.Content side="top" align="center" sideOffset={6}>
									<code class="tooltip-code">{formatModelCommand(genericModelName)}</code>
								</Tooltip.Content>
							</Tooltip.Portal>
						</Tooltip.Root>

						<!-- Specific Variant Commands -->
						<div class="grid grid-cols-2 gap-1.5">
							{#each uniqueVariants as variant (variant.name)}
								<Tooltip.Root>
									<Tooltip.Trigger>
										{#snippet child({ props })}
											<Button
												{...props}
												variant="outline"
												size="sm"
												onclick={(e) => {
													e.stopPropagation();
													onCopyCommand(variant.name);
												}}
												class="h-7 w-full justify-start gap-1.5 px-2.5 text-xs"
											>
												{#if copiedModelId === `run-${variant.name}`}
													<Check class="size-4 shrink-0 text-green-500" />
													<span class="min-w-0 truncate">Copied!</span>
												{:else}
													{@const acceleratorLogo = getAcceleratorLogo(variant.name)}
													{@const acceleratorColor = getAcceleratorColor(variant.name)}
													{#if acceleratorLogo}
														<span
															class="accelerator-logo-mask size-4 shrink-0"
															style="--logo-color: {acceleratorColor}; --logo-url: url({acceleratorLogo});"
															role="img"
															aria-label="Accelerator logo"
														></span>
													{:else}
														<span class="shrink-0"
															>{getDeviceIcon(variant.deviceSupport[0] || '')}</span
														>
													{/if}
													<span class="min-w-0 truncate">{getVariantLabel(variant)}</span>
												{/if}
											</Button>
										{/snippet}
									</Tooltip.Trigger>
									<Tooltip.Portal>
										<Tooltip.Content side="top" align="center" sideOffset={6}>
											<code class="tooltip-code">{formatModelCommand(variant.name)}</code>
										</Tooltip.Content>
									</Tooltip.Portal>
								</Tooltip.Root>
							{/each}
						</div>
					</div>
				{/if}
			</div>
		</Card.Content>
	</Card.Root>
</div>

<style>
	:root {
		--amd-color: #000000; /* Black in light mode */
	}

	:global(.dark) {
		--amd-color: #ffffff; /* White in dark mode */
	}

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

	.tooltip-code {
		display: inline-block;
		font-family:
			ui-monospace, SFMono-Regular, 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace;
		white-space: nowrap;
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
