<script lang="ts">
	import { Rocket, Cpu, Wifi, Code, Bot, Shield } from 'lucide-svelte';
	import { animate } from '$lib/utils/animations';
	import { onMount } from 'svelte';

	let featureGrid: HTMLElement;

	onMount(() => {
		// Stagger animation for feature cards
		if (featureGrid) {
			const cards = Array.from(featureGrid.children) as HTMLElement[];
			cards.forEach((card, index) => {
				card.style.opacity = '0';
				card.style.transform = 'translateY(30px)';
			});

			const animateCards = () => {
				cards.forEach((card, index) => {
					card.style.transition =
						'opacity 600ms cubic-bezier(0.4, 0, 0.2, 1), transform 600ms cubic-bezier(0.4, 0, 0.2, 1)';
					card.style.transitionDelay = `${index * 100}ms`;

					requestAnimationFrame(() => {
						card.style.opacity = '1';
						card.style.transform = 'translateY(0)';
					});
				});
			};

			// Check if element is already in viewport
			const rect = featureGrid.getBoundingClientRect();
			const isInViewport = rect.top < window.innerHeight && rect.bottom > 0;

			if (isInViewport) {
				// Trigger animation immediately if already visible
				animateCards();
			} else {
				// Otherwise, wait for it to scroll into view
				const observer = new IntersectionObserver(
					(entries) => {
						entries.forEach((entry) => {
							if (entry.isIntersecting) {
								animateCards();
								observer.unobserve(featureGrid);
							}
						});
					},
					{ threshold: 0.1 }
				);

				observer.observe(featureGrid);
			}
		}
	});
</script>

<div class="bg-gradient-to-b from-neutral-50 to-white dark:from-transparent dark:to-neutral-900">
	<div class="mx-auto max-w-[85rem] px-4 py-16 sm:px-6 lg:px-8 lg:py-24">
		<!-- Section Header -->
		<div class="mx-auto mb-16 max-w-3xl text-center">
			<h2
				use:animate={{ delay: 0, duration: 800, animation: 'slide-up', threshold: 0.2 }}
				class="mb-4 text-3xl font-bold tracking-tight text-gray-900 sm:text-4xl dark:text-white"
			>
				The fastest path from SDK install to shipped local AI
			</h2>
			<p
				use:animate={{ delay: 200, duration: 800, animation: 'slide-up', threshold: 0.2 }}
				class="text-lg text-gray-600 dark:text-neutral-400"
			>
				Start with native SDKs, keep inference in your app process, and use CLI or REST tools only
				when they help your development workflow.
			</p>
		</div>

		<!-- Bento Grid -->
		<div
			bind:this={featureGrid}
			class="grid auto-rows-[minmax(180px,auto)] gap-4 sm:grid-cols-2 lg:grid-cols-4"
		>
			<!-- Ship to Production - Large -->
			<div
				class="group border-border bg-card text-card-foreground hover:border-primary/50 relative overflow-hidden rounded-3xl border shadow-sm lg:col-span-2 lg:row-span-2"
			>
				<div class="relative z-10 flex h-full flex-col p-8">
					<div
						class="bg-primary/10 text-primary mb-4 flex size-14 items-center justify-center rounded-2xl transition-transform duration-300 group-hover:scale-110"
					>
						<Rocket class="size-7" aria-hidden="true" />
					</div>
					<h3 class="mb-2 text-2xl font-bold text-gray-900 dark:text-white">One SDK lifecycle</h3>
					<p class="mb-6 text-lg text-gray-600 dark:text-neutral-400">
						Initialize the manager, choose a model alias, download and cache it, load it, then call
						chat or audio clients from your app.
					</p>
					<!-- Animated code snippet with typing effect -->
					<div
						class="mt-auto overflow-hidden rounded-xl bg-gray-900 p-4 font-mono text-xs text-gray-300 dark:bg-black/40"
					>
						<div class="mb-1 text-emerald-400">
							// SDK lifecycle: initialize, download, load, call
						</div>
						<div class="code-line code-line-1">
							<span class="text-purple-400">const</span> mgr =
							<span class="text-blue-400">FoundryLocalManager</span>.<span class="text-yellow-300"
								>create</span
							>({'{'} appName: <span class="text-amber-300">'my-app'</span>
							{'}'})
						</div>
						<div class="code-line code-line-2">
							<span class="text-purple-400">const</span> model =
							<span class="text-purple-400">await</span>
							mgr.<span class="text-yellow-300">catalog</span>.<span class="text-yellow-300"
								>getModel</span
							>(<span class="text-amber-300">'qwen2.5-0.5b'</span>)
						</div>
						<div class="code-line code-line-3">
							<span class="text-purple-400">await</span>
							model.<span class="text-yellow-300">download</span>();
							<span class="text-purple-400">await</span>
							model.<span class="text-yellow-300">load</span>()
						</div>
						<div class="code-line code-line-4">
							<span class="text-purple-400">const</span> res =
							<span class="text-purple-400">await</span>
							model.<span class="text-yellow-300">createChatClient</span>().<span
								class="text-yellow-300">completeChat</span
							>(msgs)<span class="typing-cursor">|</span>
						</div>
					</div>
					<style>
						.code-line {
							opacity: 0;
							animation: typeIn 0.5s ease-out forwards;
						}
						.code-line-1 {
							animation-delay: 0.3s;
						}
						.code-line-2 {
							animation-delay: 0.9s;
						}
						.code-line-3 {
							animation-delay: 1.5s;
						}
						.code-line-4 {
							animation-delay: 2.1s;
						}
						@keyframes typeIn {
							from {
								opacity: 0;
								transform: translateX(-10px);
							}
							to {
								opacity: 1;
								transform: translateX(0);
							}
						}
						.typing-cursor {
							animation: blink 0.8s step-end infinite;
							color: #22c55e;
							font-weight: bold;
						}
						@keyframes blink {
							0%,
							100% {
								opacity: 1;
							}
							50% {
								opacity: 0;
							}
						}
					</style>
				</div>
			</div>

			<!-- Hardware Optimized - Large -->
			<div
				class="group border-border bg-card text-card-foreground hover:border-primary/50 relative overflow-hidden rounded-3xl border shadow-sm lg:col-span-2 lg:row-span-2"
			>
				<div class="relative z-10 flex h-full flex-col p-8">
					<div
						class="mb-4 flex size-14 items-center justify-center rounded-2xl bg-blue-500/10 text-blue-500 transition-transform duration-300 group-hover:scale-110"
					>
						<Cpu class="size-7" aria-hidden="true" />
					</div>
					<h3 class="mb-2 text-2xl font-bold text-gray-900 dark:text-white">
						Hardware handled for you
					</h3>
					<p class="mb-6 text-lg text-gray-600 dark:text-neutral-400">
						The SDK picks and registers execution providers so apps can target NPU, GPU, or CPU
						without custom device plumbing.
					</p>
					<!-- Hardware acceleration visual with glowing cards -->
					<div class="mt-auto grid grid-cols-3 gap-3">
						<div
							class="hw-card hw-1 border-border bg-secondary/50 flex flex-col items-center rounded-xl border p-3"
						>
							<div class="text-primary text-2xl font-bold">NPU</div>
							<div class="text-muted-foreground text-xs whitespace-nowrap">Neural Engine</div>
						</div>
						<div
							class="hw-card hw-2 border-border bg-secondary/50 flex flex-col items-center rounded-xl border p-3"
						>
							<div class="text-2xl font-bold text-cyan-600">GPU</div>
							<div class="text-muted-foreground text-xs whitespace-nowrap">Graphics Card</div>
						</div>
						<div
							class="hw-card hw-3 border-border bg-secondary/50 flex flex-col items-center rounded-xl border p-3"
						>
							<div class="text-2xl font-bold text-violet-500">CPU</div>
							<div class="text-muted-foreground text-xs whitespace-nowrap">Processor</div>
						</div>
					</div>
					<style>
						.hw-card {
							animation: hwGlow 3s ease-in-out infinite;
							box-shadow: 0 0 0 transparent;
						}
						.hw-1 {
							animation-delay: 0s;
						}
						.hw-2 {
							animation-delay: 0.5s;
						}
						.hw-3 {
							animation-delay: 1s;
						}
						@keyframes hwGlow {
							0%,
							100% {
								box-shadow: 0 0 0 transparent;
								transform: scale(1);
							}
							50% {
								box-shadow: 0 0 20px rgba(34, 197, 94, 0.3);
								transform: scale(1.02);
							}
						}
					</style>
				</div>
			</div>

			<!-- Edge-Ready -->
			<div
				class="group border-border bg-card text-card-foreground hover:border-primary/50 relative overflow-hidden rounded-3xl border shadow-sm"
			>
				<div class="relative z-10 p-6 pb-20">
					<div
						class="mb-4 flex size-12 items-center justify-center rounded-xl bg-emerald-500/10 text-emerald-500 transition-transform duration-300 group-hover:scale-110"
					>
						<Wifi class="size-6" aria-hidden="true" />
					</div>
					<h3 class="mb-2 text-xl font-semibold text-gray-900 dark:text-white">
						Offline app runtime
					</h3>
					<p class="text-gray-600 dark:text-neutral-400">
						The runtime and cached models stay local so user workflows keep running without a
						network.
					</p>
				</div>
				<!-- Dramatic wifi disconnect animation -->
				<div class="absolute right-3 bottom-3 flex items-center gap-3">
					<!-- Animated signal bars that lose connection -->
					<div class="signal-container flex items-end gap-1">
						<div class="signal-bar bar-1 h-2 w-1.5 rounded-sm bg-emerald-500"></div>
						<div class="signal-bar bar-2 h-3 w-1.5 rounded-sm bg-emerald-500"></div>
						<div class="signal-bar bar-3 h-4 w-1.5 rounded-sm bg-emerald-500"></div>
						<div class="signal-bar bar-4 h-5 w-1.5 rounded-sm bg-emerald-500"></div>
					</div>
					<!-- Animated checkmark that appears when offline -->
					<div class="checkmark-container">
						<svg
							class="checkmark-icon size-6 text-emerald-500"
							viewBox="0 0 24 24"
							fill="none"
							stroke="currentColor"
							stroke-width="3"
							aria-hidden="true"
						>
							<path
								class="checkmark-path"
								d="M5 13l4 4L19 7"
								stroke-linecap="round"
								stroke-linejoin="round"
							/>
						</svg>
					</div>
				</div>
				<style>
					.bar-1 {
						animation: signalRed 5s ease-in-out infinite;
					}
					.bar-2,
					.bar-3,
					.bar-4 {
						animation: signalGray 5s ease-in-out infinite;
					}

					@keyframes signalRed {
						0%,
						20% {
							background-color: #22c55e;
						}
						30%,
						60% {
							background-color: #ef4444;
						}
						70%,
						100% {
							background-color: #22c55e;
						}
					}
					@keyframes signalGray {
						0%,
						20% {
							background-color: #22c55e;
						}
						30%,
						60% {
							background-color: #52525b;
						}
						70%,
						100% {
							background-color: #22c55e;
						}
					}
					.checkmark-icon {
						animation: checkSync 5s ease-in-out infinite;
					}
					.checkmark-path {
						stroke-dasharray: 30;
						animation: checkDrawSync 5s ease-in-out infinite;
					}
					@keyframes checkSync {
						0%,
						100% {
							transform: scale(1);
						}
						30%,
						35% {
							transform: scale(1.2);
						}
						70%,
						75% {
							transform: scale(1.2);
						}
					}
					@keyframes checkDrawSync {
						0%,
						25% {
							stroke-dashoffset: 0;
						}
						28% {
							stroke-dashoffset: 30;
						}
						35%,
						65% {
							stroke-dashoffset: 0;
						}
						68% {
							stroke-dashoffset: 30;
						}
						75%,
						100% {
							stroke-dashoffset: 0;
						}
					}
				</style>
			</div>

			<!-- Multi-Language SDKs -->
			<div
				class="group border-border bg-card text-card-foreground hover:border-primary/50 relative overflow-hidden rounded-3xl border shadow-sm"
			>
				<div class="relative z-10 p-6 pb-20">
					<div
						class="mb-4 flex size-12 items-center justify-center rounded-xl bg-orange-500/10 text-orange-500 transition-transform duration-300 group-hover:scale-110"
					>
						<Code class="size-6" aria-hidden="true" />
					</div>
					<h3 class="mb-2 text-xl font-semibold text-gray-900 dark:text-white">Native SDKs</h3>
					<p class="text-gray-600 dark:text-neutral-400">
						Start in Python or JavaScript; ship production apps in C# and Rust too.
					</p>
				</div>
				<!-- Language icons with staggered pop animation -->
				<div class="absolute right-4 bottom-4 flex gap-2">
					<div
						class="lang-badge lang-1 flex size-8 items-center justify-center rounded-lg bg-blue-500/10 text-xs font-bold text-blue-600 dark:text-blue-400"
					>
						PY
					</div>
					<div
						class="lang-badge lang-2 flex size-8 items-center justify-center rounded-lg bg-yellow-500/10 text-xs font-bold text-yellow-700 dark:text-yellow-400"
					>
						JS
					</div>
					<div
						class="lang-badge lang-3 flex size-8 items-center justify-center rounded-lg bg-purple-500/10 text-xs font-bold text-purple-600 dark:text-purple-400"
					>
						C#
					</div>
					<div
						class="lang-badge lang-4 flex size-8 items-center justify-center rounded-lg bg-orange-500/10 text-xs font-bold text-orange-700 dark:text-orange-400"
					>
						RS
					</div>
				</div>
				<style>
					.lang-badge {
						animation: langPop 4s ease-in-out infinite;
					}
					.lang-1 {
						animation-delay: 0s;
					}
					.lang-2 {
						animation-delay: 0.2s;
					}
					.lang-3 {
						animation-delay: 0.4s;
					}
					.lang-4 {
						animation-delay: 0.6s;
					}
					@keyframes langPop {
						0%,
						100% {
							transform: scale(1);
						}
						10%,
						30% {
							transform: scale(1.15);
						}
					}
				</style>
			</div>

			<!-- OpenAI Compatible -->
			<div
				class="group border-border bg-card text-card-foreground hover:border-primary/50 relative overflow-hidden rounded-3xl border shadow-sm"
			>
				<div class="relative z-10 p-6 pb-20">
					<div
						class="mb-4 flex size-12 items-center justify-center rounded-xl bg-pink-500/10 text-pink-500 transition-transform duration-300 group-hover:scale-110"
					>
						<Bot class="size-6" aria-hidden="true" />
					</div>
					<h3 class="mb-2 text-xl font-semibold text-gray-900 dark:text-white">
						Native first, REST when needed
					</h3>
					<p class="text-gray-600 dark:text-neutral-400">
						Use SDK clients in-process, or start the optional OpenAI-compatible server for
						frameworks like LangChain.
					</p>
				</div>
				<!-- Animated API comparison -->
				<div class="absolute right-3 bottom-3 overflow-hidden rounded-lg bg-gray-900/90 p-2">
					<div class="api-compare font-mono text-[10px] leading-tight">
						<div class="api-line api-line-1 text-gray-400 line-through">
							base_url=<span class="text-amber-300">"api.openai.com"</span>
						</div>
						<div class="api-line api-line-2 text-emerald-400">
							base_url=<span class="text-amber-300">"localhost"</span>
						</div>
					</div>
				</div>
				<style>
					.api-line {
						animation: apiLineReveal 6s ease-in-out infinite;
					}
					.api-line-1 {
						animation-delay: 0s;
					}
					.api-line-2 {
						animation-delay: 0.3s;
					}
					@keyframes apiLineReveal {
						0%,
						10% {
							opacity: 0;
							transform: translateX(-5px);
						}
						20%,
						80% {
							opacity: 1;
							transform: translateX(0);
						}
						90%,
						100% {
							opacity: 0;
							transform: translateX(0);
						}
					}
				</style>
			</div>

			<!-- Data Privacy -->
			<div
				class="group border-border bg-card text-card-foreground hover:border-primary/50 relative overflow-hidden rounded-3xl border shadow-sm"
			>
				<div class="relative z-10 p-6 pb-20">
					<div
						class="mb-4 flex size-12 items-center justify-center rounded-xl bg-indigo-500/10 text-indigo-500 transition-transform duration-300 group-hover:scale-110"
					>
						<Shield class="size-6" aria-hidden="true" />
					</div>
					<h3 class="mb-2 text-xl font-semibold text-gray-900 dark:text-white">Data Privacy</h3>
					<p class="text-gray-600 dark:text-neutral-400">
						Prompts, audio, and responses stay on the user's device.
					</p>
				</div>
				<!-- Animated shield with glow pulse -->
				<div class="absolute -right-4 -bottom-4">
					<Shield class="shield-glow size-24 text-indigo-500/20" aria-hidden="true" />
				</div>
				<style>
					.shield-glow {
						animation: shieldPulse 3s ease-in-out infinite;
						filter: drop-shadow(0 0 0 transparent);
					}
					@keyframes shieldPulse {
						0%,
						100% {
							opacity: 0.2;
							filter: drop-shadow(0 0 0 transparent);
							transform: scale(1);
						}
						50% {
							opacity: 0.4;
							filter: drop-shadow(0 0 15px rgba(99, 102, 241, 0.4));
							transform: scale(1.05);
						}
					}
				</style>
			</div>
		</div>
	</div>
</div>
