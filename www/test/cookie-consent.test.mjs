import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import test from 'node:test';
import { transpileModule, ModuleKind } from 'typescript';

const readSource = (path) => readFileSync(new URL(path, import.meta.url), 'utf8');
const source = readSource('../src/lib/cookie-consent.ts');
const { outputText } = transpileModule(source, {
	compilerOptions: { module: ModuleKind.CommonJS }
});

function createHarness({
	accepted = false,
	initError,
	missingConsent = false,
	throws = false
} = {}) {
	const scripts = [];
	const injections = [];
	const errors = [];
	let consent = { Required: true, Analytics: accepted, SocialMedia: false, Advertising: false };
	let onConsentChanged;
	let onInit;
	let manageCount = 0;
	let initCount = 0;
	const siteConsent = {
		getConsent: () => consent,
		manageConsent: () => manageCount++
	};
	const sdk = {
		init(locale, container, callback, onChanged) {
			assert.equal(locale, 'en-US');
			assert.equal(container, 'cookie-banner');
			initCount++;
			onInit = callback;
			onConsentChanged = onChanged;
			if (throws) throw new Error('Initialization failed');
		}
	};
	const window = {};
	const exports = {};

	runInNewContext(outputText, {
		exports,
		require(name) {
			if (name === '@vercel/analytics') {
				return { inject: (options) => injections.push(options) };
			}
			if (name === 'svelte/store') {
				return {
					writable(value) {
						return {
							set: (next) => (value = next),
							subscribe(callback) {
								callback(value);
								return () => {};
							}
						};
					}
				};
			}
			throw new Error(`Unexpected import: ${name}`);
		},
		window,
		document: {
			createElement(tag) {
				assert.equal(tag, 'script');
				return {
					remove() {
						this.removed = true;
					}
				};
			},
			head: { appendChild: (script) => scripts.push(script) }
		},
		console: { error: (...args) => errors.push(args) }
	});

	return {
		...exports,
		scripts,
		injections,
		errors,
		get initCount() {
			return initCount;
		},
		get manageCount() {
			return manageCount;
		},
		get error() {
			let value;
			exports.cookieConsentError.subscribe((next) => (value = next))();
			return value;
		},
		load() {
			window.WcpConsent = sdk;
			scripts.at(-1).onload();
		},
		complete() {
			onInit(initError, missingConsent ? undefined : siteConsent);
		},
		setConsent(accepted, notify = true) {
			consent = { ...consent, Analytics: accepted };
			if (notify) onConsentChanged(consent);
		}
	};
}

test('loads Microsoft WCP once and never injects analytics before a choice or after rejection', () => {
	const harness = createHarness();
	harness.initializeCookieConsent();
	harness.initializeCookieConsent();
	assert.equal(harness.scripts.length, 1);
	assert.equal(
		harness.scripts[0].src,
		'https://wcpstatic.microsoft.com/mscc/lib/v2/wcp-consent.js'
	);
	assert.equal(harness.scripts[0].async, true);
	assert.equal(harness.injections.length, 0);
	harness.load();
	harness.complete();
	assert.equal(harness.injections.length, 0);
	harness.setConsent(false);
	assert.equal(harness.injections.length, 0);
	assert.equal(harness.error, null);
});

test('accept enables analytics once, reject blocks events, and reaccept restores events', () => {
	const harness = createHarness();
	harness.initializeCookieConsent();
	harness.load();
	harness.complete();
	harness.setConsent(true);
	assert.equal(harness.injections.length, 1);
	const { beforeSend } = harness.injections[0];
	for (const type of ['pageview', 'event']) {
		const event = { type, url: 'https://www.foundrylocal.ai/models' };
		assert.equal(beforeSend(event), event);
		harness.setConsent(false);
		assert.equal(beforeSend(event), null);
		harness.setConsent(true);
		assert.equal(beforeSend(event), event);
	}
	assert.equal(harness.injections.length, 1);
	harness.initializeCookieConsent();
	assert.equal(harness.initCount, 1);
});

test('saved acceptance enables analytics on initialization', () => {
	const harness = createHarness({ accepted: true });
	harness.initializeCookieConsent();
	harness.load();
	assert.equal(harness.injections.length, 0);
	harness.complete();
	assert.equal(harness.injections.length, 1);
});

test('revocation in another tab blocks events without a local consent-change notification', () => {
	const harness = createHarness({ accepted: true });
	harness.initializeCookieConsent();
	harness.load();
	harness.complete();
	harness.setConsent(false, false);
	assert.equal(harness.injections[0].beforeSend({ type: 'pageview', url: '/' }), null);
});

test('Manage Cookies opens Microsoft preferences without changing the current consent', () => {
	const harness = createHarness({ accepted: true });
	harness.initializeCookieConsent();
	harness.load();
	harness.complete();
	harness.manageCookies();
	harness.manageCookies();
	assert.equal(harness.manageCount, 2);
	assert.equal(harness.injections.length, 1);
});

test('script failure is reported, leaves analytics disabled, and can be retried', () => {
	const harness = createHarness();
	harness.initializeCookieConsent();
	harness.scripts[0].onerror();
	assert.equal(harness.scripts[0].removed, true);
	assert.match(harness.error, /Optional analytics remain disabled/);
	assert.equal(harness.errors.length, 1);
	assert.equal(harness.injections.length, 0);
	harness.initializeCookieConsent();
	assert.equal(harness.scripts.length, 2);
	harness.load();
	harness.complete();
	assert.equal(harness.error, null);
});

test('initialization errors, missing preferences, and thrown SDK errors fail closed', () => {
	for (const options of [
		{ initError: new Error('Consent unavailable') },
		{ missingConsent: true },
		{ throws: true }
	]) {
		const harness = createHarness(options);
		harness.initializeCookieConsent();
		harness.load();
		if (!options.throws) harness.complete();
		assert.equal(harness.injections.length, 0);
		assert.match(harness.error, /Optional analytics remain disabled/);
		assert.equal(harness.errors.length, 1);
	}
});

test('early preference requests show a recoverable error which clears when initialization completes', () => {
	const harness = createHarness();
	harness.initializeCookieConsent();
	harness.manageCookies();
	assert.match(harness.error, /not ready/);
	harness.load();
	harness.complete();
	assert.equal(harness.error, null);
	harness.manageCookies();
	assert.equal(harness.manageCount, 1);
});

test('all routes use the shared banner and every page footer exposes Manage Cookies', () => {
	const layout = readSource('../src/routes/+layout.svelte');
	const banner = readSource('../src/lib/components/cookie-consent.svelte');
	const footer = readSource('../src/lib/components/home/footer.svelte');
	assert.match(layout, /<CookieConsent\s*\/>/);
	assert.doesNotMatch(layout, /\binject\s*\(/);
	assert.match(banner, /onMount\(initializeCookieConsent\)/);
	assert.match(banner, /id="cookie-banner"/);
	assert.match(banner, /role="alert"/);
	assert.match(banner, /onclick=\{initializeCookieConsent\}/);
	assert.match(footer, /<button[^>]*onclick=\{manageCookies\}[^>]*>\s*Manage Cookies\s*<\/button>/);
	for (const page of ['+page.svelte', 'models/+page.svelte', '+error.svelte']) {
		assert.match(readSource(`../src/routes/${page}`), /<Footer\s*\/>/);
	}
});
