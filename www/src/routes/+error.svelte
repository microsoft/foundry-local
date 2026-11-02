<script lang="ts">
	import { page } from '$app/stores';
	import { Home, ArrowLeft, Brain, Zap, RotateCcw } from 'lucide-svelte';
	import { Button } from '$lib/components/ui/button';
	import Footer from '$lib/components/home/footer.svelte';

	const errorMessages = [
		"Even our AI models can't find this page...",
		'404: Model not found in local cache',
		'This page ran away faster than a GPU at full load',
		'Looks like this endpoint needs an upgrade',
		'Our neural network thinks this page is still training...',
		'Error: Page.exe has stopped responding (and never started)',
		'This URL is more lost than a CPU trying to compete with a GPU'
	];

	const randomMessage = errorMessages[Math.floor(Math.random() * errorMessages.length)];
	const status = $page.status || 404;
	const errorType = status === 404 ? 'Page Not Found' : 'Something Went Wrong';

	function goBack() {
		window.history.back();
	}

	function reload() {
		window.location.reload();
	}
</script>

<svelte:head>
	<title>{status} - {errorType} | Foundry Local</title>
</svelte:head>

<main class="flex min-h-screen flex-col items-center justify-center bg-background px-4 py-12">
	<div class="mx-auto max-w-2xl text-center">
		<!-- Animated brain icon with glitch effect -->
		<div class="relative mb-8 inline-block">
			<div class="animate-pulse">
				<Brain class="h-32 w-32 text-primary opacity-20" strokeWidth={1} aria-hidden="true" />
			</div>
			<div class="absolute inset-0 flex items-center justify-center">
				<div
					class="flex h-24 w-24 items-center justify-center rounded-full bg-primary/10 backdrop-blur-sm"
				>
					<span class="font-mono text-6xl font-bold text-primary">{status}</span>
				</div>
			</div>
		</div>

		<!-- Error message -->
		<h1 class="mb-4 text-4xl font-bold tracking-tight text-foreground sm:text-5xl">
			{errorType}
		</h1>

		<p class="mb-2 text-xl text-muted-foreground">
			{randomMessage}
		</p>

		<p class="mb-8 text-sm text-muted-foreground/80">
			Don't worry, this happens to the best of us (and our models).
		</p>

		<!-- Fun AI-themed stats -->
		<div class="mb-12 grid grid-cols-3 gap-4 rounded-lg border border-border bg-card p-6">
			<div class="flex flex-col items-center">
				<div class="mb-2 flex items-center gap-2">
					<Zap class="h-4 w-4 text-yellow-500" aria-hidden="true" />
					<span class="text-2xl font-bold text-foreground">0%</span>
				</div>
				<span class="text-xs text-muted-foreground">Confidence</span>
			</div>
			<div class="flex flex-col items-center">
				<div class="mb-2 flex items-center gap-2">
					<Brain class="h-4 w-4 text-primary" aria-hidden="true" />
					<span class="text-2xl font-bold text-foreground">∞</span>
				</div>
				<span class="text-xs text-muted-foreground">Confusion</span>
			</div>
			<div class="flex flex-col items-center">
				<div class="mb-2">
					<span class="text-2xl font-bold text-foreground">NaN</span>
				</div>
				<span class="text-xs text-muted-foreground">Pages Found</span>
			</div>
		</div>

		<!-- Action buttons -->
		<div class="flex flex-col items-center justify-center gap-4 sm:flex-row">
			<Button href="/" size="lg" class="min-w-[160px]">
				<Home class="mr-2 size-4" aria-hidden="true" />
				Back to Home
			</Button>
			<Button variant="outline" size="lg" onclick={goBack} class="min-w-[160px]">
				<ArrowLeft class="mr-2 size-4" aria-hidden="true" />
				Go Back
			</Button>
			{#if status !== 404}
				<Button variant="outline" size="lg" onclick={reload} class="min-w-[160px]">
					<RotateCcw class="mr-2 size-4" aria-hidden="true" />
					Try Again
				</Button>
			{/if}
		</div>

		<!-- Funny footer message with easter egg -->
		<div class="mt-12 rounded-lg bg-muted/50 p-4">
			<p class="text-sm text-muted-foreground">
				<span class="font-mono text-primary">🎉 Easter Egg Unlocked!</span>
				<br />
				<span class="mt-2 block opacity-75">
					Psst... here's a secret: when chatting with a Foundry model, you can type
					<span class="font-mono text-primary">/toodaloo</span> to end the session with style. Because
					even AI deserves a proper goodbye! 👋
				</span>
			</p>
		</div>
	</div>
</main>

<Footer />

<style>
	@keyframes glitch {
		0% {
			transform: translate(0);
		}
		20% {
			transform: translate(-2px, 2px);
		}
		40% {
			transform: translate(-2px, -2px);
		}
		60% {
			transform: translate(2px, 2px);
		}
		80% {
			transform: translate(2px, -2px);
		}
		100% {
			transform: translate(0);
		}
	}

	h1:hover {
		animation: glitch 0.3s infinite;
	}
</style>
