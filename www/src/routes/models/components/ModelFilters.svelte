<script lang="ts">
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Label } from '$lib/components/ui/label';
	import { Search, RefreshCw, ChevronDown, Check } from 'lucide-svelte';
	import * as Card from '$lib/components/ui/card';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu';
	import { foundryModelService } from '../service';
	import {
		getDeviceIcon,
		getAcceleratorLogoFromAcceleration,
		getAcceleratorColorFromAcceleration
	} from '$lib/utils/model-helpers';

	export let searchTerm = '';
	export let selectedDevices: string[] = [];
	export let selectedFamily = '';
	export let selectedAcceleration = '';
	export let sortBy = 'lastModified';
	export let sortOrder: 'asc' | 'desc' = 'desc';
	export let availableDevices: string[] = [];
	export let availableFamilies: string[] = [];
	export let availableAccelerations: string[] = [];
	export let filteredCount = 0;
	export let loading = false;
	export let isFiltering = false;

	export let onRefresh: () => void;
	export let onClearFilters: () => void;

	function toggleDevice(device: string) {
		if (selectedDevices.includes(device)) {
			selectedDevices = selectedDevices.filter((d) => d !== device);
		} else {
			selectedDevices = [...selectedDevices, device];
		}
	}

	$: hasActiveFilters =
		searchTerm || selectedDevices.length > 0 || selectedFamily || selectedAcceleration;
</script>

<Card.Root class="border-border/40 bg-background shadow-sm">
	<Card.Header class="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
		<div>
			<Card.Title level={2} class="text-2xl font-semibold">Browse Foundry Models</Card.Title>
			<Card.Description>Results update automatically as you type or change filters</Card.Description
			>
		</div>
		<Button variant="ghost" size="sm" onclick={onRefresh} disabled={loading} aria-label="Refresh models">
			<RefreshCw class={`mr-1 size-4 ${loading ? 'animate-spin' : ''}`} aria-hidden="true" />
			Refresh
		</Button>
	</Card.Header>
	<Card.Content>
		<div class="grid gap-4 md:grid-cols-2 lg:grid-cols-3">
			<!-- Search -->
			<div class="lg:col-span-2">
				<Label for="search">Search Models</Label>
				<div class="relative">
					<Search class="absolute left-3 top-1/2 size-4 -translate-y-1/2 text-gray-400" aria-hidden="true" />
					<Input
						id="search"
						type="text"
						bind:value={searchTerm}
						placeholder="Search by name, description, or tags..."
						class="pl-10"
					/>
				</div>
			</div>

			<!-- Sort -->
			<div>
				<Label>Sort By</Label>
				<div class="flex gap-2">
					<DropdownMenu.Root>
						<DropdownMenu.Trigger asChild>
							{#snippet child({ props })}
								<Button
									{...props}
									variant="outline"
									class="h-10 w-full justify-between font-normal"
								>
									<span>
										{#if sortBy === 'lastModified'}
											Last Modified
										{:else if sortBy === 'name'}
											Name
										{:else if sortBy === 'fileSizeBytes'}
											File Size
										{/if}
									</span>
									<ChevronDown class="ml-2 size-4 opacity-50" />
								</Button>
							{/snippet}
						</DropdownMenu.Trigger>
						<DropdownMenu.Content
							style="width: var(--radix-dropdown-menu-trigger-width)"
							class="min-w-0"
						>
							<DropdownMenu.Item onclick={() => (sortBy = 'lastModified')}>
								<Check
									class="mr-2 size-4 {sortBy === 'lastModified' ? 'opacity-100' : 'opacity-0'}"
								/>
								Last Modified
							</DropdownMenu.Item>
							<DropdownMenu.Item onclick={() => (sortBy = 'name')}>
								<Check class="mr-2 size-4 {sortBy === 'name' ? 'opacity-100' : 'opacity-0'}" />
								Name
							</DropdownMenu.Item>
							<DropdownMenu.Item onclick={() => (sortBy = 'fileSizeBytes')}>
								<Check
									class="mr-2 size-4 {sortBy === 'fileSizeBytes' ? 'opacity-100' : 'opacity-0'}"
								/>
								File Size
							</DropdownMenu.Item>
						</DropdownMenu.Content>
					</DropdownMenu.Root>

					<Button
						variant="outline"
						class="h-10 w-16"
						onclick={() => (sortOrder = sortOrder === 'asc' ? 'desc' : 'asc')}
					>
						{sortOrder === 'asc' ? '↑' : '↓'}
					</Button>
				</div>
			</div>

			<!-- Device Filter -->
			<div>
				<Label>Execution Device</Label>
				<div class="flex h-10 gap-2">
					{#each availableDevices as device}
						<Button
							variant={selectedDevices.includes(device) ? 'default' : 'outline'}
							size="sm"
							onclick={() => toggleDevice(device)}
							class="h-full flex-1"
						>
							{getDeviceIcon(device)}
							{device.toUpperCase()}
						</Button>
					{/each}
				</div>
			</div>

			<!-- Family Filter -->
			<div>
				<Label for="family">Model Family</Label>
				<DropdownMenu.Root>
					<DropdownMenu.Trigger asChild>
						{#snippet child({ props })}
							<Button {...props} variant="outline" class="h-10 w-full justify-between font-normal">
								<span>
									{selectedFamily
										? selectedFamily.charAt(0).toUpperCase() + selectedFamily.slice(1)
										: 'All Families'}
								</span>
								<ChevronDown class="ml-2 size-4 opacity-50" />
							</Button>
						{/snippet}
					</DropdownMenu.Trigger>
					<DropdownMenu.Content
						style="width: var(--radix-dropdown-menu-trigger-width)"
						class="min-w-0"
					>
						<DropdownMenu.Item onclick={() => (selectedFamily = '')}>
							<Check class="mr-2 size-4 {!selectedFamily ? 'opacity-100' : 'opacity-0'}" />
							All Families
						</DropdownMenu.Item>
						<DropdownMenu.Separator />
						{#each availableFamilies as family}
							<DropdownMenu.Item onclick={() => (selectedFamily = family)}>
								<Check
									class="mr-2 size-4 {selectedFamily === family ? 'opacity-100' : 'opacity-0'}"
								/>
								{family.charAt(0).toUpperCase() + family.slice(1)}
							</DropdownMenu.Item>
						{/each}
					</DropdownMenu.Content>
				</DropdownMenu.Root>
			</div>

			<!-- Acceleration Filter -->
			<div>
				<Label for="acceleration">Acceleration</Label>
				<DropdownMenu.Root>
					<DropdownMenu.Trigger asChild>
						{#snippet child({ props })}
							<Button {...props} variant="outline" class="h-10 w-full justify-between font-normal">
								<span>
									{selectedAcceleration
										? foundryModelService.getAccelerationDisplayName(selectedAcceleration)
										: 'All Accelerations'}
								</span>
								<ChevronDown class="ml-2 size-4 opacity-50" />
							</Button>
						{/snippet}
					</DropdownMenu.Trigger>
					<DropdownMenu.Content
						style="width: var(--radix-dropdown-menu-trigger-width)"
						class="min-w-0"
					>
						<DropdownMenu.Item onclick={() => (selectedAcceleration = '')}>
							<Check class="mr-2 size-4 {!selectedAcceleration ? 'opacity-100' : 'opacity-0'}" />
							All Accelerations
						</DropdownMenu.Item>
						<DropdownMenu.Separator />
						{#each availableAccelerations as acceleration}
							<DropdownMenu.Item onclick={() => (selectedAcceleration = acceleration)}>
								{@const acceleratorLogo = getAcceleratorLogoFromAcceleration(acceleration)}
								{@const acceleratorColor = getAcceleratorColorFromAcceleration(acceleration)}
								{#if acceleratorLogo && selectedAcceleration === acceleration}
									<span
										class="accelerator-logo-mask mr-2 size-4"
										style="--logo-color: {acceleratorColor}; --logo-url: url({acceleratorLogo});"
										role="img"
										aria-label="{acceleration} logo"
									></span>
								{:else}
									<Check
										class="mr-2 size-4 {selectedAcceleration === acceleration
											? 'opacity-100'
											: 'opacity-0'}"
									/>
								{/if}
								{foundryModelService.getAccelerationDisplayName(acceleration)}
							</DropdownMenu.Item>
						{/each}
					</DropdownMenu.Content>
				</DropdownMenu.Root>
			</div>
		</div>

		<!-- Filter Summary -->
		<div class="mt-4 flex items-center justify-between border-t border-border/40 pt-4">
			<div class="text-sm text-gray-600 dark:text-gray-400">
				{#if isFiltering}
					<span class="inline-flex items-center">
						<div
							class="border-primary mr-2 size-4 animate-spin rounded-full border-2 border-t-transparent"
						></div>
						Filtering...
					</span>
				{:else}
					{filteredCount} model{filteredCount !== 1 ? 's' : ''} found
				{/if}
			</div>
			{#if hasActiveFilters}
				<Button variant="outline" size="sm" onclick={onClearFilters}>Clear Filters</Button>
			{/if}
		</div>
	</Card.Content>
</Card.Root>

<style>
	:root {
		--amd-color: #000000; /* Black in light mode */
	}

	:global(.dark) {
		--amd-color: #ffffff; /* White in dark mode */
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
