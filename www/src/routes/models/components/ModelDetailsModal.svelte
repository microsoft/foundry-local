<script lang="ts">
	import type { GroupedFoundryModel } from '../types';
	import { Button } from '$lib/components/ui/button';
	import { Badge } from '$lib/components/ui/badge';
	import { Calendar, Package, Copy, Check, ExternalLink } from 'lucide-svelte';
	import * as Dialog from '$lib/components/ui/dialog';
	import { foundryModelService } from '../service';
	import ModelStarterCode from './ModelStarterCode.svelte';
	import { getModelStarterKind } from '../model-boilerplate';

	export let model: GroupedFoundryModel | null;
	export let isOpen = false;
	export let copiedModelId: string | null = null;
	export let onCopyModelId: (modelId: string) => void;
	export let onCopyShareUrl: (modelAlias: string) => void;

	const HIDDEN_TAG_SUBSTRINGS = [
		':licensedescription',
		'prompttemplate',
		'toolcallend',
		'toolcallstart',
		'toolregisterend',
		'toolregisterstart',
		'toolresponsestart',
		'toolresponseend',
		'directorypath',
		'invisiblelatest'
	];

	$: genericModelName = model ? getGenericModelName(model) : '';

	$: visibleTags =
		model?.tags.filter((tag) => {
			const lower = tag.toLowerCase();
			return !HIDDEN_TAG_SUBSTRINGS.some((pattern) => lower.includes(pattern));
		}) ?? [];

	import {
		getDeviceIcon,
		getAcceleratorLogo,
		getAcceleratorColor,
		getVariantLabel,
		getAcceleratorLogoFromAcceleration,
		getAcceleratorColorFromAcceleration
	} from '$lib/utils/model-helpers';
	import { generateModelDescription } from '$lib/utils/model-description';

	function getUniqueVariants(model: GroupedFoundryModel) {
		if (!model.variants || model.variants.length === 0) return [];

		const uniqueMap = new Map();
		for (const variant of model.variants) {
			if (!uniqueMap.has(variant.name)) {
				uniqueMap.set(variant.name, variant);
			}
		}
		return Array.from(uniqueMap.values());
	}

	function getUniqueAccelerations(model: GroupedFoundryModel): string[] {
		if (!model.variants || model.variants.length === 0) return [];

		const accelerations = new Set<string>();
		for (const variant of model.variants) {
			if (variant.acceleration) {
				accelerations.add(variant.acceleration);
			}
		}
		return Array.from(accelerations).sort();
	}

	function getAverageFileSize(
		model: GroupedFoundryModel
	): { bytes: number; formatted: string } | null {
		if (!model.variants || model.variants.length === 0) return null;

		const fileSizes = model.variants.map((v) => v.fileSizeBytes || 0).filter((s) => s > 0);

		if (fileSizes.length === 0) return null;

		const averageBytes = Math.round(
			fileSizes.reduce((sum, size) => sum + size, 0) / fileSizes.length
		);
		const formatted = foundryModelService.formatFileSize(averageBytes);

		return { bytes: averageBytes, formatted };
	}

	function formatDate(dateString: string): string {
		return new Date(dateString).toLocaleDateString('en-US', {
			year: 'numeric',
			month: 'short',
			day: 'numeric'
		});
	}

	function renderMarkdown(text: string): string {
		if (!text) return '';

		let html = text;

		// Headers
		html = html.replace(/^#### (.*?)$/gm, '<h4 class="text-base font-semibold mt-4 mb-2">$1</h4>');
		html = html.replace(/^### (.*?)$/gm, '<h3 class="text-lg font-semibold mt-5 mb-2">$1</h3>');
		html = html.replace(/^## (.*?)$/gm, '<h2 class="text-xl font-bold mt-6 mb-3">$1</h2>');
		html = html.replace(/^# (.*?)$/gm, '<h1 class="text-2xl font-bold mt-6 mb-3">$1</h1>');

		// Process lists
		const lines = html.split('\n');
		const processed: string[] = [];
		let inList = false;
		let listItems: string[] = [];

		for (let i = 0; i < lines.length; i++) {
			const line = lines[i];
			const trimmedLine = line.trim();
			const listMatch = trimmedLine.match(/^[-*]\s+(.+)$/);

			if (listMatch) {
				if (!inList) {
					inList = true;
					listItems = [];
				}
				listItems.push(listMatch[1]);
			} else {
				if (inList) {
					const listHtml =
						'<ul class="list-disc list-outside mb-3 space-y-1 ml-6 pl-2">' +
						listItems.map((item) => `<li>${item}</li>`).join('') +
						'</ul>';
					processed.push(listHtml);
					inList = false;
					listItems = [];
				}
				processed.push(line);
			}
		}

		if (inList) {
			const listHtml =
				'<ul class="list-disc list-outside mb-3 space-y-1 ml-6 pl-2">' +
				listItems.map((item) => `<li>${item}</li>`).join('') +
				'</ul>';
			processed.push(listHtml);
		}

		html = processed.join('\n');

		// Bold, italic, code, links
		html = html.replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>');
		html = html.replace(/(?<!^)(?<![-\s])\*([^\*\n]+?)\*/g, '<em>$1</em>');
		html = html.replace(/`(.*?)`/g, '<code>$1</code>');
		html = html.replace(
			/\[(.*?)\]\((.*?)\)/g,
			'<a href="$2" target="_blank" rel="noopener noreferrer" class="text-primary hover:underline">$1</a>'
		);

		// Paragraphs
		html = html
			.split('\n\n')
			.map((para) => {
				const trimmed = para.trim();
				if (
					trimmed &&
					!trimmed.startsWith('<h') &&
					!trimmed.startsWith('<ul') &&
					!trimmed.startsWith('<ol')
				) {
					return `<p class="mb-3">${para.replace(/\n/g, '<br>')}</p>`;
				}
				return para;
			})
			.join('\n');

		return html;
	}

	// Device suffix pattern for cleaning model names
	const DEVICE_SUFFIX_PATTERN = /-(generic|cuda|qnn|openvino|vitis)-(cpu|gpu|npu)$|-(cpu|gpu|npu)$/i;

	function getGenericModelName(model: GroupedFoundryModel): string {
		const baseName = model.alias || model.variants[0]?.name || model.displayName;
		return baseName.split(':')[0].replace(DEVICE_SUFFIX_PATTERN, '');
	}
</script>

<Dialog.Root bind:open={isOpen}>
	<Dialog.Content class="max-h-[90vh] max-w-4xl overflow-y-auto">
		{#if model}
			<Dialog.Header class="gap-3 pr-10 sm:flex-row sm:items-start sm:justify-between sm:pr-12">
				<div class="space-y-1.5">
					<Dialog.Title class="flex items-center gap-3 text-2xl font-bold">
						<span>{model.displayName}</span>
						<Badge variant="secondary" class="text-xs">v{model.latestVersion}</Badge>
					</Dialog.Title>
					<Dialog.Description class="text-muted-foreground text-sm">
						{model.publisher}
					</Dialog.Description>
				</div>
				<Button
					variant="outline"
					size="sm"
					onclick={() => onCopyShareUrl(model.alias)}
					class="gap-2 self-start"
				>
					<Copy class="size-4" />
					Copy Share Link
				</Button>
			</Dialog.Header>

			<div class="mt-6 space-y-6">
				<!-- Stats Section -->
				<div class="grid grid-cols-2 gap-4 sm:grid-cols-3">
					<div class="bg-card rounded-lg border p-4">
						<div class="text-primary text-2xl font-bold">{model.variants.length}</div>
						<div class="text-muted-foreground text-xs">Variants</div>
					</div>
					<div class="bg-card rounded-lg border p-4">
						<div class="text-primary text-2xl font-bold">
							{formatDate(model.lastModified)}
						</div>
						<div class="text-muted-foreground text-xs">Updated</div>
					</div>
					<div class="bg-card rounded-lg border p-4">
						<div class="flex flex-wrap gap-2">
							{#each model.deviceSupport as device}
								<div
									class="bg-primary/10 inline-flex items-center gap-1 rounded-md px-2.5 py-1 text-sm font-medium"
								>
									<span class="text-base">{getDeviceIcon(device)}</span>
									<span class="text-primary">{device.toUpperCase()}</span>
								</div>
							{/each}
						</div>
						<div class="text-muted-foreground mt-2 text-xs">Supported Devices</div>
					</div>
				</div>

				<!-- Get started / boilerplate code -->
				{#if model}
					{#if getModelStarterKind(model) !== 'audio'}
						<div class="border-border/40 flex items-center gap-2 rounded-lg border px-3 py-2">
							<code class="text-muted-foreground min-w-0 flex-1 font-mono text-sm">
								foundry run {genericModelName}
							</code>
							<button
								type="button"
								onclick={() => onCopyModelId(`run-${genericModelName}`)}
								class="border-border text-muted-foreground hover:text-foreground hover:border-primary/50 hover:bg-primary/5 flex shrink-0 items-center gap-1.5 rounded border px-2.5 py-1 text-xs transition-colors"
								aria-label="Copy run command for {genericModelName}"
							>
								{#if copiedModelId === `run-${genericModelName}`}
									<Check class="size-3.5 text-green-500" aria-hidden="true" />
									<span>Copied</span>
								{:else}
									<Copy class="size-3.5" aria-hidden="true" />
									<span>Copy</span>
								{/if}
							</button>
						</div>
					{/if}
					<ModelStarterCode {model} />
				{/if}

				<!-- Description -->
				<div>
					<h3 class="mb-2 text-lg font-semibold">Description</h3>
					<div
						class="prose prose-sm text-muted-foreground dark:prose-invert max-w-none text-sm leading-relaxed"
					>
						{@html renderMarkdown(model.longDescription || generateModelDescription(model))}
					</div>
				</div>

				<!-- Model Details -->
				<div>
					<h3 class="mb-3 text-lg font-semibold">Model Information</h3>
					<div class="grid gap-3 sm:grid-cols-2">
						{#if model.taskType}
							<div class="bg-card/50 flex items-start gap-3 rounded-lg border p-3">
								<Package class="text-primary mt-0.5 size-4" aria-hidden="true" />
								<div>
									<div class="text-muted-foreground text-xs font-medium">Task Type</div>
									<div class="text-sm font-medium">{model.taskType}</div>
								</div>
							</div>
						{/if}
						{#if model && getAverageFileSize(model)}
							<div class="bg-card/50 flex items-start gap-3 rounded-lg border p-3">
								<svg class="text-primary mt-0.5 size-4" fill="currentColor" viewBox="0 0 20 20" aria-hidden="true">
									<path
										fill-rule="evenodd"
										d="M4 4a2 2 0 012-2h4.586A2 2 0 0112 2.586L15.414 6A2 2 0 0116 7.414V16a2 2 0 01-2 2H6a2 2 0 01-2-2V4zm2 6a1 1 0 011-1h6a1 1 0 110 2H7a1 1 0 01-1-1zm1 3a1 1 0 100 2h6a1 1 0 100-2H7z"
										clip-rule="evenodd"
									/>
								</svg>
								<div>
									<div class="text-muted-foreground text-xs font-medium">File Size (avg)</div>
									<div class="text-sm font-medium">{getAverageFileSize(model)?.formatted}</div>
								</div>
							</div>
						{/if}
						{#if model.license}
							{@const licenseUrl = foundryModelService.getLicenseUrl(model.license)}
							<div class="bg-card/50 flex items-start gap-3 rounded-lg border p-3">
								<svg class="text-primary mt-0.5 size-4" fill="currentColor" viewBox="0 0 20 20" aria-hidden="true">
									<path d="M9 2a1 1 0 000 2h2a1 1 0 100-2H9z" />
									<path
										fill-rule="evenodd"
										d="M4 5a2 2 0 012-2 3 3 0 003 3h2a3 3 0 003-3 2 2 0 012 2v11a2 2 0 01-2 2H6a2 2 0 01-2-2V5zm3 4a1 1 0 000 2h.01a1 1 0 100-2H7zm3 0a1 1 0 000 2h3a1 1 0 100-2h-3zm-3 4a1 1 0 100 2h.01a1 1 0 100-2H7zm3 0a1 1 0 100 2h3a1 1 0 100-2h-3z"
										clip-rule="evenodd"
									/>
								</svg>
								<div>
									<div class="text-muted-foreground text-xs font-medium">License</div>
									{#if licenseUrl}
										<a
											href={licenseUrl}
											target="_blank"
											rel="noopener noreferrer"
											class="text-primary text-sm font-medium hover:underline"
											aria-label={`${model.license} license (opens in new tab)`}
										>
											{model.license}
										</a>
									{:else}
										<div class="text-sm font-medium">{model.license}</div>
									{/if}
								</div>
							</div>
						{/if}
						{#if model && getUniqueAccelerations(model).length > 0}
							<div class="bg-card/50 flex items-start gap-3 rounded-lg border p-3">
								<svg class="text-primary mt-0.5 size-4" fill="currentColor" viewBox="0 0 20 20" aria-hidden="true">
									<path
										fill-rule="evenodd"
										d="M11.3 1.046A1 1 0 0112 2v5h4a1 1 0 01.82 1.573l-7 10A1 1 0 018 18v-5H4a1 1 0 01-.82-1.573l7-10a1 1 0 011.12-.38z"
										clip-rule="evenodd"
									/>
								</svg>
								<div class="flex-1">
									<div class="text-muted-foreground text-xs font-medium">Acceleration</div>
									<div class="mt-1 flex flex-wrap gap-1.5">
										{#each getUniqueAccelerations(model) as acceleration}
											{@const accelerationLogo = getAcceleratorLogoFromAcceleration(acceleration)}
											{@const accelerationColor = getAcceleratorColorFromAcceleration(acceleration)}
											<Badge variant="secondary" class="text-xs">
												{#if accelerationLogo}
													<span
														class="accelerator-logo-mask mr-1 inline-block size-3.5"
														style="--logo-color: {accelerationColor}; --logo-url: url({accelerationLogo});"
														role="img"
														aria-label="{acceleration} logo"
													></span>
												{/if}
												{foundryModelService.getAccelerationDisplayName(acceleration)}
											</Badge>
										{/each}
									</div>
								</div>
							</div>
						{/if}
					</div>
				</div>

				<!-- Available Variants -->
				<div>
					<div class="mb-3 flex items-center justify-between gap-3">
						<h3 class="text-lg font-semibold">Available Variants</h3>
						<div class="flex items-center gap-2">
							<span class="text-muted-foreground font-mono text-xs">{genericModelName}</span>
							<Button
								variant="outline"
								size="sm"
								onclick={(e) => {
									e.stopPropagation();
									onCopyModelId(genericModelName);
								}}
								class="h-7 gap-1.5 px-2.5 text-xs"
							>
								{#if copiedModelId === genericModelName}
									<Check class="size-3.5 text-green-500" />
									Copied
								{:else}
									<Copy class="size-3.5" />
									Copy ID
								{/if}
							</Button>
						</div>
					</div>
					<div class="space-y-2">
						{#each getUniqueVariants(model) as variant}
							<div
								class="bg-card hover:border-primary/50 flex items-center justify-between rounded-lg border p-3 transition-colors"
							>
								<div class="min-w-0 flex-1">
									<div class="font-mono text-xs font-medium">{variant.name}</div>
									<div
										class="text-muted-foreground mt-1 flex flex-wrap items-center gap-1.5 text-xs"
									>
										{#each variant.deviceSupport as device}
											{@const acceleratorLogo = getAcceleratorLogo(variant.name)}
											{@const acceleratorColor = getAcceleratorColor(variant.name)}
											<Badge variant="secondary" class="gap-1 text-xs">
												{#if acceleratorLogo}
													<span
														class="accelerator-logo-mask inline-block size-3"
														style="--logo-color: {acceleratorColor}; --logo-url: url({acceleratorLogo});"
														role="img"
														aria-label="Accelerator logo"
													></span>
												{:else}
													{getDeviceIcon(device)}
												{/if}
												{getVariantLabel(variant)}
											</Badge>
										{/each}
										{#if variant.fileSizeBytes}
											<Badge variant="outline" class="text-xs">
												{foundryModelService.formatFileSize(variant.fileSizeBytes)}
											</Badge>
										{/if}
									</div>
								</div>
								<Button
									variant="ghost"
									size="sm"
									onclick={(e) => {
										e.stopPropagation();
										onCopyModelId(variant.name);
									}}
									class="ml-3 h-7 shrink-0 gap-1.5 px-2.5 text-xs"
								>
									{#if copiedModelId === variant.name}
										<Check class="size-3.5 text-green-500" />
										Copied
									{:else}
										<Copy class="size-3.5" />
										Copy ID
									{/if}
								</Button>
							</div>
						{/each}
					</div>
				</div>

				<!-- Links Section -->
				{#if model.githubUrl || model.paperUrl || model.demoUrl || model.documentation}
					<div>
						<h3 class="mb-3 text-lg font-semibold">Resources</h3>
						<div class="flex flex-wrap gap-2">
							{#if model.githubUrl}
								<Button
									variant="outline"
									size="sm"
									onclick={() => model && window.open(model.githubUrl, '_blank')}
								>
									<ExternalLink class="mr-2 size-4" />
									GitHub
								</Button>
							{/if}
							{#if model.paperUrl}
								<Button
									variant="outline"
									size="sm"
									onclick={() => model && window.open(model.paperUrl, '_blank')}
								>
									<ExternalLink class="mr-2 size-4" />
									Paper
								</Button>
							{/if}
							{#if model.demoUrl}
								<Button
									variant="outline"
									size="sm"
									onclick={() => model && window.open(model.demoUrl, '_blank')}
								>
									<ExternalLink class="mr-2 size-4" />
									Demo
								</Button>
							{/if}
							{#if model.documentation}
								<Button
									variant="outline"
									size="sm"
									onclick={() => model && window.open(model.documentation, '_blank')}
								>
									<ExternalLink class="mr-2 size-4" />
									Documentation
								</Button>
							{/if}
						</div>
					</div>
				{/if}

				<!-- Tags -->
				{#if visibleTags.length > 0}
					<div>
						<h3 class="mb-2 text-lg font-semibold">Tags</h3>
						<div class="flex flex-wrap gap-2">
							{#each visibleTags as tag}
								<Badge variant="outline" class="text-xs">{tag}</Badge>
							{/each}
						</div>
					</div>
				{/if}
			</div>
		{/if}
	</Dialog.Content>
</Dialog.Root>

<style>
	.prose :global(code) {
		background-color: hsl(var(--muted));
		padding: 0.125rem 0.25rem;
		border-radius: 0.25rem;
		font-size: 0.875em;
		font-family:
			ui-monospace, SFMono-Regular, 'SF Mono', Menlo, Consolas, 'Liberation Mono', monospace;
	}

	.prose :global(a) {
		color: hsl(var(--primary));
		text-decoration: none;
	}

	.prose :global(a:hover) {
		text-decoration: underline;
	}

	.prose :global(strong) {
		font-weight: 600;
		color: hsl(var(--foreground));
	}

	.prose :global(h1),
	.prose :global(h2),
	.prose :global(h3),
	.prose :global(h4) {
		color: hsl(var(--foreground));
		line-height: 1.3;
	}

	.prose :global(h1:first-child),
	.prose :global(h2:first-child),
	.prose :global(h3:first-child),
	.prose :global(h4:first-child) {
		margin-top: 0;
	}

	.prose :global(p) {
		margin-bottom: 0.75rem;
		line-height: 1.6;
	}

	.prose :global(p:last-child) {
		margin-bottom: 0;
	}

	.prose :global(ul) {
		margin-bottom: 0.75rem;
		padding-left: 0;
	}

	.prose :global(li) {
		line-height: 1.6;
		margin-bottom: 0.25rem;
		color: hsl(var(--muted-foreground));
	}

	.prose :global(li):last-child {
		margin-bottom: 0;
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
