<script lang="ts">
	import { onMount } from 'svelte';
	import { Button } from '$lib/components/ui/button';
	import { ArrowUp } from 'lucide-svelte';
	import { scale } from 'svelte/transition';
	import { cubicOut } from 'svelte/easing';

	let showButton = $state(false);

	function scrollToTop() {
		window.scrollTo({
			top: 0,
			behavior: 'smooth'
		});
	}

	onMount(() => {
		function handleScroll() {
			showButton = window.scrollY > 400;
		}

		window.addEventListener('scroll', handleScroll);
		handleScroll();

		return () => {
			window.removeEventListener('scroll', handleScroll);
		};
	});
</script>

{#if showButton}
	<div
		class="fixed bottom-8 right-8 z-50"
		transition:scale={{ duration: 300, easing: cubicOut, start: 0.8 }}
	>
		<Button
			onclick={scrollToTop}
			size="icon"
			class="size-12 rounded-full shadow-lg transition-all duration-300 hover:scale-110 hover:shadow-xl focus-visible:ring-2 focus-visible:ring-offset-2 focus-visible:ring-offset-background"
			aria-label="Scroll back to top"
		>
			<ArrowUp class="size-5" aria-hidden="true" />
		</Button>
	</div>
{/if}
