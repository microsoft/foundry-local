// Resolves the latest `cli-preview-*` GitHub release at build time so the site never hardcodes a
// specific preview tag (issue #924). Returns the release's page URL; the platform download buttons
// link there and let the user pick their architecture/variant (mirroring the arch-safe `winget`/
// `brew` install path in platform.ts). If release discovery fails, it falls back to the general
// releases page.

const OWNER_REPO = 'microsoft/Foundry-Local';
const RELEASES_PAGE = `https://github.com/${OWNER_REPO}/releases`;
const RELEASES_API = `https://api.github.com/repos/${OWNER_REPO}/releases?per_page=30`;
const CLI_TAG_PREFIX = 'cli-preview-';

export interface CliLinks {
	/** Release tag (e.g. "cli-preview-0.10.2"), or null when discovery failed. */
	tag: string | null;
	/** Version portion of the tag (e.g. "0.10.2"), or null when discovery failed. */
	version: string | null;
	/** The resolved release's page, or the general releases page on fallback. */
	releasePage: string;
}

interface GitHubRelease {
	tag_name: string;
	draft: boolean;
	published_at: string | null;
	html_url: string;
}

function fallbackLinks(): CliLinks {
	return { tag: null, version: null, releasePage: RELEASES_PAGE };
}

/**
 * Fetch the repo's releases and resolve the newest non-draft `cli-preview-*` release.
 * Pass the SvelteKit-provided `fetch` from a server `load` so this runs at build/prerender time.
 * An optional `token` (from the build environment) is sent as a bearer credential to raise the
 * GitHub API rate limit; it is never required and never exposed to the client.
 */
export async function resolveCliLinks(
	fetchFn: typeof fetch = fetch,
	token?: string
): Promise<CliLinks> {
	try {
		const headers: Record<string, string> = { Accept: 'application/vnd.github+json' };
		if (token) {
			headers.Authorization = `Bearer ${token}`;
		}

		const response = await fetchFn(RELEASES_API, { headers });
		if (!response.ok) {
			return fallbackLinks();
		}

		const releases = (await response.json()) as GitHubRelease[];
		const latest = releases
			.filter((release) => release.tag_name?.startsWith(CLI_TAG_PREFIX) && !release.draft)
			// Coerce invalid/missing published_at to 0 so a null date sorts oldest deterministically.
			.sort(
				(a, b) => (Date.parse(b.published_at ?? '') || 0) - (Date.parse(a.published_at ?? '') || 0)
			)[0];

		if (!latest) {
			return fallbackLinks();
		}

		return {
			tag: latest.tag_name,
			version: latest.tag_name.slice(CLI_TAG_PREFIX.length) || null,
			releasePage: latest.html_url ?? RELEASES_PAGE
		};
	} catch {
		return fallbackLinks();
	}
}
