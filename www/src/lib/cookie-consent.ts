import { inject } from '@vercel/analytics';
import { writable } from 'svelte/store';

interface Consent {
	Required: boolean;
	Analytics: boolean;
	SocialMedia: boolean;
	Advertising: boolean;
}

interface SiteConsent {
	getConsent(): Consent;
	manageConsent(): void;
}

interface WcpConsent {
	init(
		locale: string,
		containerId: string,
		onInit: (error?: Error, consent?: SiteConsent) => void,
		onConsentChanged: (consent: Consent) => void
	): void;
}

declare global {
	interface Window {
		WcpConsent?: WcpConsent;
	}
}

export const cookieConsentError = writable<string | null>(null);

let siteConsent: SiteConsent | undefined;
let loading = false;
let analyticsAllowed = false;
let analyticsInjected = false;

function updateAnalytics(consent: Consent) {
	analyticsAllowed = consent.Analytics === true;
	if (analyticsAllowed && !analyticsInjected) {
		inject({
			// Check the current cookie too, so revocation in another tab also stops events.
			beforeSend: (event) =>
				analyticsAllowed && siteConsent?.getConsent().Analytics === true ? event : null
		});
		analyticsInjected = true;
	}
}

function reportError(error: unknown) {
	loading = false;
	analyticsAllowed = false;
	console.error('Cookie consent could not be initialized.', error);
	cookieConsentError.set(
		'Cookie preferences are unavailable. Optional analytics remain disabled. Please try again.'
	);
}

export function initializeCookieConsent() {
	if (loading || siteConsent) return;
	loading = true;
	cookieConsentError.set(null);

	function initialize() {
		if (!window.WcpConsent) {
			reportError(new Error('The Microsoft cookie consent library is unavailable.'));
			return;
		}

		try {
			window.WcpConsent.init(
				'en-US',
				'cookie-banner',
				(error, consent) => {
					loading = false;
					if (error || !consent) {
						reportError(error ?? new Error('The cookie consent library returned no preferences.'));
						return;
					}
					siteConsent = consent;
					cookieConsentError.set(null);
					updateAnalytics(consent.getConsent());
				},
				updateAnalytics
			);
		} catch (error) {
			reportError(error);
		}
	}

	if (window.WcpConsent) {
		initialize();
		return;
	}

	const script = document.createElement('script');
	script.src = 'https://wcpstatic.microsoft.com/mscc/lib/v2/wcp-consent.js';
	script.async = true;
	script.onload = initialize;
	script.onerror = () => {
		script.remove();
		reportError(new Error('The Microsoft cookie consent script failed to load.'));
	};
	document.head.appendChild(script);
}

export function manageCookies() {
	if (siteConsent) {
		siteConsent.manageConsent();
	} else {
		cookieConsentError.set(
			'Cookie preferences are not ready. Optional analytics remain disabled. Please try again.'
		);
	}
}
