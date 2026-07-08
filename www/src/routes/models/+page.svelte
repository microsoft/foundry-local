<script lang="ts">
	import { browser } from '$app/environment';
	import { onMount, onDestroy } from 'svelte';
	import { page } from '$app/stores';
	import { goto } from '$app/navigation';
	import { foundryModelService } from './service';
	import type { GroupedFoundryModel } from './types';
	import Nav from '$lib/components/home/nav.svelte';
	import Footer from '$lib/components/home/footer.svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Card from '$lib/components/ui/card';
	import * as Tooltip from '$lib/components/ui/tooltip';
	import { toast } from 'svelte-sonner';
	import { ModelFilters, ModelGrid, ModelDetailsModal } from './components';
	import { Terminal, Copy, Check, ExternalLink } from 'lucide-svelte';

	// Known device names used as shorthand URL params (e.g. /models?cpu)
	const KNOWN_DEVICES = ['cpu', 'gpu', 'npu'];
	const MODEL_QUERY_PARAM = 'model';
	const CLI_RUN_COMMAND = 'foundry run qwen2.5-0.5b';
	const CLI_RELEASE_URL =
		'https://github.com/microsoft/Foundry-Local/releases/tag/cli-preview-0.10.0';
	const CLI_INSTALL_LINKS = [
		{
			id: 'windows-cli',
			label: 'Windows',
			href: CLI_RELEASE_URL
		},
		{
			id: 'macos-cli',
			label: 'macOS',
			href: CLI_RELEASE_URL
		},
		{
			id: 'linux-cli',
			label: 'Linux',
			href: CLI_RELEASE_URL
		}
	];

	// Debounce timer for search
	let searchDebounceTimer: ReturnType<typeof setTimeout> | null = null;
	let debouncedSearchTerm = '';

	// State
	let allModels: GroupedFoundryModel[] = [];
	let filteredModels: GroupedFoundryModel[] = [];
	let loading = false;
	let error = '';
	let copiedModelId: string | null = null;
	let copiedCliCommandId: string | null = null;

	// Modal state
	let selectedModel: GroupedFoundryModel | null = null;
	let isModalOpen = false;
	let lastNonModelUrl = '/models';
	let previousModalOpenState = false;
	let invalidModelAliasHandled: string | null = null;

	// Filter state
	let searchTerm = '';
	let selectedDevices: string[] = [];
	let selectedFamily = '';
	let selectedAcceleration = '';
	let sortBy = 'lastModified';
	let sortOrder: 'asc' | 'desc' = 'desc';

	// Track whether we've initialized filters from URL
	let filtersInitialized = false;
	// Suppress URL updates while reading from URL
	let suppressUrlUpdate = false;

	// Available filter options
	let availableDevices: string[] = [];
	let availableFamilies: string[] = ['deepseek', 'mistral', 'qwen', 'phi', 'whisper'];
	let availableAccelerations: string[] = [];

	// Read filter state from URL search params
	function readFiltersFromUrl() {
		const params = getUrlSearchParams();

		// Device filters: shorthand keys like ?cpu, ?gpu, ?npu
		const devices: string[] = [];
		for (const device of KNOWN_DEVICES) {
			if (params.has(device)) {
				devices.push(device);
			}
		}
		selectedDevices = devices;

		// Named params
		searchTerm = params.get('q') ?? '';
		debouncedSearchTerm = searchTerm;
		selectedFamily = params.get('family') ?? '';
		selectedAcceleration = params.get('acceleration') ?? '';
		sortBy = params.get('sort') ?? 'lastModified';
		sortOrder = (params.get('order') as 'asc' | 'desc') ?? 'desc';
	}

	function normalizeModelAlias(modelAlias: string): string {
		return modelAlias.trim().toLowerCase();
	}

	function getUrlSearchParams(): URLSearchParams {
		return browser ? $page.url.searchParams : new URLSearchParams();
	}

	function buildModelsUrl(params: URLSearchParams): string {
		const search = params.toString();
		const cleanSearch = search.replace(/=(?=&|$)/g, '');
		return cleanSearch ? `/models?${cleanSearch}` : '/models';
	}

	function buildFilterSearchParams(): URLSearchParams {
		const params = new URLSearchParams();

		for (const device of selectedDevices) {
			if (KNOWN_DEVICES.includes(device)) {
				params.set(device, '');
			}
		}

		if (searchTerm) params.set('q', searchTerm);
		if (selectedFamily) params.set('family', selectedFamily);
		if (selectedAcceleration) params.set('acceleration', selectedAcceleration);
		if (sortBy && sortBy !== 'lastModified') params.set('sort', sortBy);
		if (sortOrder && sortOrder !== 'desc') params.set('order', sortOrder);

		return params;
	}

	function getModelAliasFromUrl(): string {
		return normalizeModelAlias(getUrlSearchParams().get(MODEL_QUERY_PARAM) ?? '');
	}

	function getCurrentUrlWithoutModel(): string {
		const params = new URLSearchParams(getUrlSearchParams());
		params.delete(MODEL_QUERY_PARAM);
		return buildModelsUrl(params);
	}

	// Write current filter state to URL search params (replaceState, no navigation)
	function updateUrlFromFilters() {
		if (suppressUrlUpdate || getModelAliasFromUrl()) return;

		const newUrl = buildModelsUrl(buildFilterSearchParams());

		goto(newUrl, { replaceState: true, noScroll: true, keepFocus: true });
	}

	function getModelByAlias(modelAlias: string): GroupedFoundryModel | null {
		const normalizedAlias = normalizeModelAlias(modelAlias);
		return allModels.find((model) => normalizeModelAlias(model.alias) === normalizedAlias) ?? null;
	}

	function syncSelectedModelFromUrl() {
		const modelAlias = getModelAliasFromUrl();

		if (!modelAlias) {
			invalidModelAliasHandled = null;
			selectedModel = null;
			isModalOpen = false;
			return;
		}

		if (loading || error) return;

		const matchedModel = getModelByAlias(modelAlias);

		if (!matchedModel) {
			if (invalidModelAliasHandled !== modelAlias) {
				invalidModelAliasHandled = modelAlias;
				toast.error(`Model \"${modelAlias}\" is no longer available.`);
				goto(getCurrentUrlWithoutModel(), {
					replaceState: true,
					noScroll: true,
					keepFocus: true
				});
			}
			return;
		}

		invalidModelAliasHandled = null;

		const currentUrlWithoutModel = getCurrentUrlWithoutModel();
		if (currentUrlWithoutModel !== '/models' || lastNonModelUrl === '/models') {
			lastNonModelUrl = currentUrlWithoutModel;
		}

		selectedModel = matchedModel;
		isModalOpen = true;
	}

	function openModelDetails(model: GroupedFoundryModel) {
		selectedModel = model;
		isModalOpen = true;
		lastNonModelUrl = getCurrentUrlWithoutModel();

		const currentModelAlias = getModelAliasFromUrl();
		const nextModelAlias = normalizeModelAlias(model.alias);
		if (currentModelAlias === nextModelAlias) return;

		const params = new URLSearchParams();
		params.set(MODEL_QUERY_PARAM, model.alias);
		goto(buildModelsUrl(params), { noScroll: true, keepFocus: true });
	}

	function closeModelDetails(restorePreviousUrl = true) {
		selectedModel = null;
		isModalOpen = false;

		if (!getModelAliasFromUrl()) return;

		const fallbackUrl = restorePreviousUrl ? lastNonModelUrl : getCurrentUrlWithoutModel();
		goto(fallbackUrl || '/models', {
			replaceState: true,
			noScroll: true,
			keepFocus: true
		});
	}

	async function copyModelShareUrl(modelAlias: string) {
		try {
			const shareUrl = new URL('/models', window.location.origin);
			shareUrl.searchParams.set(MODEL_QUERY_PARAM, modelAlias);
			await navigator.clipboard.writeText(shareUrl.toString());
			toast.success('Model link copied to clipboard');
		} catch (err) {
			toast.error('Failed to copy link');
		}
	}

	// Fetch all models from API
	async function fetchAllModels() {
		loading = true;
		error = '';

		try {
			allModels = await foundryModelService.fetchGroupedModels(
				{},
				{ sortBy: 'lastModified', sortOrder: 'desc' }
			);
			updateFilterOptions();
		} catch (err: any) {
			console.error('Failed to fetch models:', err);
			error = 'Failed to fetch models. Please try again later.';
		} finally {
			loading = false;
		}
	}

	// Refresh models
	async function refreshModels() {
		foundryModelService.clearCache();
		await fetchAllModels();
		toast.success('Models refreshed successfully');
	}

	function updateFilterOptions() {
		availableDevices = [...new Set(allModels.flatMap((m) => m.deviceSupport))].sort();

		const accelerations = new Set<string>();
		allModels.forEach((model) => {
			if (model.acceleration) {
				accelerations.add(model.acceleration);
			}
			if (model.variants) {
				model.variants.forEach((variant) => {
					if (variant.acceleration) {
						accelerations.add(variant.acceleration);
					}
				});
			}
		});

		availableAccelerations = [...accelerations].sort((a, b) =>
			foundryModelService
				.getAccelerationDisplayName(a)
				.localeCompare(foundryModelService.getAccelerationDisplayName(b))
		);
	}

	// Check if model matches search term
	function matchesSearchTerm(model: GroupedFoundryModel, searchLower: string): boolean {
		if (!searchLower) return true;

		return Boolean(
			model.displayName.toLowerCase().includes(searchLower) ||
			model.alias.toLowerCase().includes(searchLower) ||
			model.description.toLowerCase().includes(searchLower) ||
			model.tags.some((tag) => tag.toLowerCase().includes(searchLower)) ||
			model.variants?.some((v) => v.name.toLowerCase().includes(searchLower)) ||
			(model.acceleration &&
				foundryModelService
					.getAccelerationDisplayName(model.acceleration)
					.toLowerCase()
					.includes(searchLower))
		);
	}

	// Get sort value for a model
	function getSortValue(model: GroupedFoundryModel, sortKey: string): string | number | Date {
		switch (sortKey) {
			case 'displayName':
			case 'name':
				return model.displayName;
			case 'totalDownloads':
			case 'downloadCount':
				return model.totalDownloads || 0;
			case 'fileSizeBytes':
				return model.fileSizeBytes || 0;
			case 'lastModified':
				return model.lastModified;
			default:
				return String((model as unknown as Record<string, unknown>)[sortKey] ?? '');
		}
	}

	function applyFilters() {
		const searchLower = debouncedSearchTerm.toLowerCase();

		filteredModels = allModels.filter((model) => {
			const matchesSearch = matchesSearchTerm(model, searchLower);
			const matchesDevice =
				selectedDevices.length === 0 ||
				selectedDevices.some((device) => model.deviceSupport.includes(device));
			const matchesFamily =
				!selectedFamily ||
				model.displayName.toLowerCase().includes(selectedFamily.toLowerCase()) ||
				model.alias.toLowerCase().includes(selectedFamily.toLowerCase());
			const matchesAcceleration =
				!selectedAcceleration ||
				model.acceleration === selectedAcceleration ||
				model.variants?.some((v) => v.acceleration === selectedAcceleration);

			return matchesSearch && matchesDevice && matchesFamily && matchesAcceleration;
		});

		// Apply sorting
		filteredModels.sort((a, b) => {
			let aVal: string | number | Date = getSortValue(a, sortBy);
			let bVal: string | number | Date = getSortValue(b, sortBy);

			if (sortBy === 'lastModified') {
				aVal = new Date(aVal as string);
				bVal = new Date(bVal as string);
			} else if (typeof aVal === 'string') {
				aVal = aVal.toLowerCase();
				bVal = (bVal as string).toLowerCase();
			}

			return sortOrder === 'asc' ? (aVal > bVal ? 1 : -1) : aVal < bVal ? 1 : -1;
		});
	}

	function clearFilters() {
		searchTerm = '';
		debouncedSearchTerm = '';
		selectedDevices = [];
		selectedFamily = '';
		selectedAcceleration = '';
		sortBy = 'lastModified';
		sortOrder = 'desc';
		// Clear URL params
		goto('/models', { replaceState: true, noScroll: true, keepFocus: true });
	}

	async function copyModelId(modelId: string) {
		try {
			await navigator.clipboard.writeText(modelId);
			copiedModelId = modelId;
			toast.success('Model ID copied to clipboard');
			setTimeout(() => {
				copiedModelId = null;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy to clipboard');
		}
	}

	async function copyRunCommand(modelId: string) {
		try {
			const command = `foundry run ${modelId}`;
			await navigator.clipboard.writeText(command);
			copiedModelId = `run-${modelId}`;
			toast.success('Run command copied to clipboard');
			setTimeout(() => {
				copiedModelId = null;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy to clipboard');
		}
	}

	async function copyCliCommand(command: string, id: string) {
		try {
			await navigator.clipboard.writeText(command);
			copiedCliCommandId = id;
			toast.success('CLI command copied to clipboard');
			setTimeout(() => {
				copiedCliCommandId = null;
			}, 2000);
		} catch (err) {
			toast.error('Failed to copy to clipboard');
		}
	}

	// Reactive statements
	$: {
		if (searchDebounceTimer) {
			clearTimeout(searchDebounceTimer);
		}
		searchDebounceTimer = setTimeout(() => {
			debouncedSearchTerm = searchTerm;
		}, 300);
	}

	// Auto-set default sort order only when sortBy changes
	let previousSortBy = sortBy;
	$: if (sortBy !== previousSortBy) {
		if (sortBy === 'fileSizeBytes' || sortBy === 'lastModified' || sortBy === 'downloadCount') {
			sortOrder = 'desc';
		} else {
			sortOrder = 'asc';
		}
		previousSortBy = sortBy;
	}

	$: {
		// Trigger filtering whenever any filter value changes
		selectedDevices;
		selectedFamily;
		selectedAcceleration;
		debouncedSearchTerm;
		sortBy;
		sortOrder;

		if (allModels.length > 0) {
			applyFilters();
		} else {
			filteredModels = [];
		}
	}

	// Sync filter state to URL whenever filters change (after initialization)
	$: if (browser && filtersInitialized) {
		// Track all filter values to trigger reactivity
		selectedDevices;
		selectedFamily;
		selectedAcceleration;
		searchTerm;
		sortBy;
		sortOrder;

		updateUrlFromFilters();
	}

	$: if (browser && filtersInitialized) {
		$page.url.searchParams.get(MODEL_QUERY_PARAM);
		allModels;
		loading;
		error;

		syncSelectedModelFromUrl();
	}

	$: if (browser) {
		const modelAlias = getModelAliasFromUrl();
		if (previousModalOpenState && !isModalOpen && modelAlias) {
			closeModelDetails(true);
		}
		previousModalOpenState = isModalOpen;
	}

	onMount(() => {
		// Read initial filter state from URL before fetching
		suppressUrlUpdate = true;
		readFiltersFromUrl();
		suppressUrlUpdate = false;
		filtersInitialized = true;

		fetchAllModels();
	});

	onDestroy(() => {
		if (searchDebounceTimer) {
			clearTimeout(searchDebounceTimer);
		}
	});

	let description =
		'Discover and explore Foundry local models optimized for various hardware devices including NPUs, GPUs, CPUs, FPGAs and other specialized compute platforms.';
	let keywords =
		'foundry, local models, npu models, gpu models, cpu models, onnx runtime, machine learning models, ai models, hardware optimization';
</script>

<svelte:head>
	<title>Foundry Local Models - Browse AI Models</title>
	<meta name="description" content={description} />
	<meta name="keywords" content={keywords} />
	<meta property="og:title" content="Foundry Local Models" />
	<meta property="og:description" content={description} />
	<meta property="twitter:title" content="Foundry Local Models" />
	<meta property="twitter:description" content={description} />
</svelte:head>

<Tooltip.Provider delayDuration={150}>
	<Nav />

	<div class="bg-white dark:bg-neutral-950">
		<main id="main-content" class="mx-auto w-full max-w-6xl px-6 py-8 sm:px-8 lg:px-12">
			<h1 class="sr-only">Foundry Local model catalog</h1>
			<section
				class="border-border/50 bg-muted/30 mb-4 rounded-lg border px-3 py-2.5 sm:px-4"
				aria-label="CLI quick test"
			>
				<div class="flex flex-col gap-2 xl:flex-row xl:items-center">
					<div class="flex shrink-0 items-center gap-2">
						<Terminal class="text-primary size-4" aria-hidden="true" />
						<div class="leading-tight">
							<div class="text-sm font-medium">Test with the CLI</div>
							<div class="text-muted-foreground text-xs">
								Copy a command, then swap in any model alias.
							</div>
						</div>
					</div>

					<div
						class="grid min-w-0 flex-1 gap-2 md:grid-cols-[repeat(3,minmax(6rem,auto))_minmax(18rem,1fr)]"
					>
						{#each CLI_INSTALL_LINKS as item}
							<a
								href={item.href}
								target="_blank"
								rel="noopener noreferrer"
								class="border-border/60 bg-background/60 hover:bg-background focus:ring-primary flex min-h-11 items-center justify-between gap-3 rounded-md border px-3 py-2 text-left transition-colors focus:ring-2 focus:ring-offset-2 focus:outline-none"
								aria-label={`${item.label} CLI download on GitHub (opens in new tab)`}
							>
								<span class="text-sm font-medium">{item.label}</span>
								<span class="text-primary flex shrink-0 items-center gap-1 text-xs font-medium">
									<ExternalLink class="size-4" aria-hidden="true" />
									GitHub
								</span>
							</a>
						{/each}

						<button
							type="button"
							class="border-primary/20 bg-background/80 hover:bg-background focus:ring-primary flex min-h-11 min-w-0 items-center gap-2 rounded-md border px-3 py-2 text-left transition-colors focus:ring-2 focus:ring-offset-2 focus:outline-none"
							onclick={() => copyCliCommand(CLI_RUN_COMMAND, 'run-model')}
							aria-label="Copy CLI model run command"
						>
							<span class="text-primary shrink-0 text-xs font-medium">Run</span>
							<code class="text-muted-foreground min-w-0 flex-1 text-xs whitespace-nowrap"
								>{CLI_RUN_COMMAND}</code
							>
							{#if copiedCliCommandId === 'run-model'}
								<Check class="size-4 shrink-0 text-green-600" aria-hidden="true" />
								<span class="sr-only">Copied</span>
							{:else}
								<Copy class="size-4 shrink-0 opacity-60" aria-hidden="true" />
							{/if}
						</button>
					</div>
				</div>
			</section>

			<ModelFilters
				bind:searchTerm
				bind:selectedDevices
				bind:selectedFamily
				bind:selectedAcceleration
				bind:sortBy
				bind:sortOrder
				{availableDevices}
				{availableFamilies}
				{availableAccelerations}
				filteredCount={filteredModels.length}
				{loading}
				isFiltering={searchTerm !== debouncedSearchTerm}
				onRefresh={refreshModels}
				onClearFilters={clearFilters}
			/>

			<!-- Loading State -->
			{#if loading}
				<div
					class="flex flex-col items-center justify-center py-20"
					role="status"
					aria-live="polite"
				>
					<div
						class="border-primary mb-4 size-12 animate-spin rounded-full border-4 border-t-transparent"
						aria-hidden="true"
					></div>
					<p class="text-lg text-gray-600 dark:text-gray-400">Loading foundry models...</p>
				</div>
			{/if}

			<!-- Error State -->
			{#if error}
				<Card.Root
					class="border-red-200 bg-red-50 dark:border-red-900 dark:bg-red-950"
					role="alert"
				>
					<Card.Content class="pt-6">
						<div class="flex items-start gap-4">
							<div class="rounded-full bg-red-100 p-2 dark:bg-red-900">
								<svg
									class="size-6 text-red-600 dark:text-red-400"
									fill="none"
									stroke="currentColor"
									viewBox="0 0 24 24"
									aria-hidden="true"
								>
									<path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12" />
								</svg>
							</div>
							<div class="flex-1">
								<h3 class="font-semibold text-red-900 dark:text-red-100">Error Loading Models</h3>
								<p class="mt-1 text-sm text-red-700 dark:text-red-300">{error}</p>
							</div>
						</div>
					</Card.Content>
				</Card.Root>
			{/if}

			<!-- Models Grid -->
			{#if !loading && !error}
				<ModelGrid
					models={filteredModels}
					{copiedModelId}
					onCardClick={openModelDetails}
					onCopyCommand={copyRunCommand}
					onClearFilters={clearFilters}
				/>
			{/if}
		</main>
	</div>

	<!-- Model Details Modal -->
	<ModelDetailsModal
		model={selectedModel}
		bind:isOpen={isModalOpen}
		{copiedModelId}
		onCopyModelId={copyModelId}
		onCopyCommand={copyRunCommand}
		onCopyShareUrl={copyModelShareUrl}
	/>

	<Footer />
</Tooltip.Provider>
