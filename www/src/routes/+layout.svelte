<script lang="ts">
	import '../app.css';
	import { ModeWatcher } from 'mode-watcher';
	import { Toaster } from '$lib/components/ui/sonner';
	import { onNavigate } from '$app/navigation';
	import CookieConsent from '$lib/components/cookie-consent.svelte';

	let { children } = $props();

	// Page transition animation
	onNavigate((navigation) => {
		if (!document.startViewTransition) return;

		return new Promise((resolve) => {
			document.startViewTransition(async () => {
				resolve();
				await navigation.complete;
			});
		});
	});
</script>

<Toaster />
<ModeWatcher />
<CookieConsent />
<div class="h-dvh">
	{@render children()}
</div>
