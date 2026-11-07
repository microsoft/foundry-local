<script lang="ts">
	import { onMount } from 'svelte';
	import { cookieConsentError, initializeCookieConsent } from '$lib/cookie-consent';

	onMount(initializeCookieConsent);
</script>

<div class="fixed inset-x-0 bottom-0 z-[10000]">
	<div id="cookie-banner"></div>
	{#if $cookieConsentError}
		<div
			role="alert"
			class="border-border bg-background text-foreground flex flex-wrap items-center justify-center gap-4 border-t p-4 text-sm"
		>
			<p>{$cookieConsentError}</p>
			<button
				type="button"
				class="rounded-sm underline underline-offset-4"
				onclick={initializeCookieConsent}
			>
				Try again
			</button>
		</div>
	{/if}
</div>
