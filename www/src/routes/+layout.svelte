<script lang="ts">
	import '../app.css';
	import { ModeWatcher } from 'mode-watcher';
	import { Toaster } from '$lib/components/ui/sonner';
	import { onNavigate } from '$app/navigation';
	import { inject } from '@vercel/analytics';

	let { children } = $props();

	// Inject Vercel Analytics
	inject();

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
<div class="h-dvh">
	{@render children()}
</div>
