<script>
	import { siteConfig } from '$lib/config';
	import { manageCookies } from '$lib/cookie-consent';
	import SocialMedia from '../social-media.svelte';
	import { animate } from '$lib/utils/animations';
	import LogoTransition from '$lib/components/logo-transition.svelte';
</script>

<footer
	use:animate={{ delay: 0, duration: 800, animation: 'fade-in', threshold: 0.1 }}
	class="border-border bg-muted/30 border-t"
>
	<div class="mx-auto w-full max-w-[85rem] px-4 py-12 sm:px-6 lg:px-8">
		<div class="grid grid-cols-1 gap-8 sm:grid-cols-2 lg:grid-cols-12">
			<!-- Logo and description -->
			<div class="sm:col-span-2 lg:col-span-6">
				<a href="/" class="logo-hover-target mb-4 inline-flex">
					<LogoTransition
						colorSrc={siteConfig.logo}
						darkSrc={siteConfig.logoDark}
						strokeSrc={siteConfig.logoMark}
						height={32}
						alt="Foundry Local"
					/>
				</a>
				<p class="text-muted-foreground max-w-xs text-sm">
					Build AI-powered applications with on-device inference through native SDKs.
				</p>
				<div class="mt-6">
					<SocialMedia />
				</div>
			</div>

			<!-- Links - Product & Resources -->
			<nav class="lg:col-span-3" aria-label="Product links">
				<h3 class="text-foreground mb-4 text-sm font-semibold">Product</h3>
				<ul class="space-y-3 text-sm">
					<li>
						<a
							href="https://learn.microsoft.com/en-us/azure/foundry-local/get-started"
							target="_blank"
							rel="noopener noreferrer"
							class="text-muted-foreground hover:text-foreground transition-colors"
							aria-label="Documentation (opens in new tab)">Documentation</a
						>
					</li>
					<li>
						<a href="/models" class="text-muted-foreground hover:text-foreground transition-colors"
							>Model Hub</a
						>
					</li>
					<li>
						<a
							href="https://github.com/microsoft/Foundry-Local/tree/main/samples"
							target="_blank"
							rel="noopener noreferrer"
							class="text-muted-foreground hover:text-foreground transition-colors"
							aria-label="SDK samples (opens in new tab)">SDK Samples</a
						>
					</li>
					<li>
						<a
							href="https://github.com/microsoft/foundry-local/releases"
							target="_blank"
							rel="noopener noreferrer"
							class="text-muted-foreground hover:text-foreground transition-colors"
							aria-label="Releases (opens in new tab)">Releases</a
						>
					</li>
				</ul>
			</nav>

			<!-- Community links -->
			<nav class="lg:col-span-3" aria-label="Community links">
				<h3 class="text-foreground mb-4 text-sm font-semibold">Community</h3>
				<ul class="space-y-3 text-sm">
					<li>
						<a
							href="https://github.com/microsoft/foundry-local"
							target="_blank"
							rel="noopener noreferrer"
							class="text-muted-foreground hover:text-foreground transition-colors"
							aria-label="GitHub (opens in new tab)">GitHub</a
						>
					</li>
					<li>
						<a
							href="https://github.com/microsoft/foundry-local/issues"
							target="_blank"
							rel="noopener noreferrer"
							class="text-muted-foreground hover:text-foreground transition-colors"
							aria-label="Support (opens in new tab)">Support</a
						>
					</li>
					<li>
						<a
							href="https://github.com/microsoft/Foundry-Local/blob/main/LICENSE"
							target="_blank"
							rel="noopener noreferrer"
							class="text-muted-foreground hover:text-foreground transition-colors"
							aria-label="MIT License (opens in new tab)">MIT License</a
						>
					</li>
				</ul>
			</nav>
		</div>

		<!-- Copyright -->
		<div class="border-border mt-12 border-t pt-8">
			<div class="flex flex-col items-center gap-2">
				<p class="text-muted-foreground text-center text-sm">
					&copy; {new Date().getFullYear()} Microsoft Corporation. All rights reserved.
				</p>
				<a
					href="https://go.microsoft.com/fwlink/?LinkId=521839"
					target="_blank"
					rel="noopener noreferrer"
					class="text-muted-foreground hover:text-foreground text-sm transition-colors"
					aria-label="Microsoft Privacy Statement (opens in new tab)"
				>
					Privacy Statement
				</a>
				<button
					id="manage-cookies"
					type="button"
					onclick={manageCookies}
					class="text-muted-foreground hover:text-foreground rounded-sm text-sm underline-offset-4 transition-colors hover:underline"
				>
					Manage Cookies
				</button>
			</div>
		</div>
	</div>
</footer>
