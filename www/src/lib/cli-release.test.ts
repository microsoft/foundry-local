import { describe, it, expect } from 'vitest';
import { resolveCliLinks } from './cli-release';

const RELEASES_PAGE = 'https://github.com/microsoft/Foundry-Local/releases';

function makeRelease(tag: string, publishedAt: string | null, opts: { draft?: boolean } = {}) {
	return {
		tag_name: tag,
		draft: opts.draft ?? false,
		prerelease: true,
		published_at: publishedAt,
		html_url: `https://github.com/microsoft/Foundry-Local/releases/tag/${tag}`
	};
}

// Minimal fetch stubs. Types are loosened because resolveCliLinks only reads `ok` and `json()`.
const okFetch = (body: unknown) =>
	(async () => ({ ok: true, json: async () => body })) as unknown as typeof fetch;
const statusFetch = (ok: boolean) =>
	(async () => ({ ok, json: async () => [] })) as unknown as typeof fetch;
const throwingFetch = () =>
	(async () => {
		throw new Error('network down');
	}) as unknown as typeof fetch;

describe('resolveCliLinks', () => {
	it('resolves the newest cli-preview release by published date, not array order', async () => {
		const releases = [
			makeRelease('cli-preview-0.10.0', '2026-06-04T20:35:47Z'),
			makeRelease('cli-preview-0.10.2', '2026-07-14T21:39:03Z'),
			makeRelease('cli-preview-0.10.1', '2026-06-22T23:45:41Z')
		];
		const links = await resolveCliLinks(okFetch(releases));
		expect(links.tag).toBe('cli-preview-0.10.2');
		expect(links.version).toBe('0.10.2');
		expect(links.releasePage).toBe(
			'https://github.com/microsoft/Foundry-Local/releases/tag/cli-preview-0.10.2'
		);
	});

	it('ignores non cli-preview tags and draft releases', async () => {
		const releases = [
			makeRelease('v-unrelated', '2027-01-01T00:00:00Z'),
			makeRelease('cli-preview-0.11.0', '2026-08-01T00:00:00Z', { draft: true }),
			makeRelease('cli-preview-0.10.2', '2026-07-14T21:39:03Z')
		];
		const links = await resolveCliLinks(okFetch(releases));
		expect(links.tag).toBe('cli-preview-0.10.2');
	});

	it('treats a null published_at as oldest, deterministically, regardless of array order', async () => {
		const releases = [
			makeRelease('cli-preview-0.10.3', null),
			makeRelease('cli-preview-0.10.2', '2026-07-14T21:39:03Z')
		];
		const links = await resolveCliLinks(okFetch(releases));
		expect(links.tag).toBe('cli-preview-0.10.2');
	});

	it('sends a bearer Authorization header only when a token is provided', async () => {
		const seen: Array<Record<string, string> | undefined> = [];
		const spyFetch = ((_url: string, init?: { headers?: Record<string, string> }) => {
			seen.push(init?.headers);
			return Promise.resolve({
				ok: true,
				json: async () => [makeRelease('cli-preview-0.10.2', '2026-07-14T21:39:03Z')]
			});
		}) as unknown as typeof fetch;

		await resolveCliLinks(spyFetch, 'secret-token');
		await resolveCliLinks(spyFetch);

		expect(seen[0]?.Authorization).toBe('Bearer secret-token');
		expect(seen[1]?.Authorization).toBeUndefined();
	});

	it('falls back to the releases page when the API responds non-OK', async () => {
		const links = await resolveCliLinks(statusFetch(false));
		expect(links.tag).toBeNull();
		expect(links.version).toBeNull();
		expect(links.releasePage).toBe(RELEASES_PAGE);
	});

	it('falls back to the releases page when no cli-preview release exists', async () => {
		const links = await resolveCliLinks(okFetch([makeRelease('v1.0.0', '2027-01-01T00:00:00Z')]));
		expect(links.tag).toBeNull();
		expect(links.releasePage).toBe(RELEASES_PAGE);
	});

	it('falls back to the releases page when fetch throws', async () => {
		const links = await resolveCliLinks(throwingFetch());
		expect(links.tag).toBeNull();
		expect(links.releasePage).toBe(RELEASES_PAGE);
	});

	it('falls back when the API returns a non-array error object (e.g. rate limited)', async () => {
		const links = await resolveCliLinks(okFetch({ message: 'API rate limit exceeded' }));
		expect(links.tag).toBeNull();
		expect(links.releasePage).toBe(RELEASES_PAGE);
	});
});
