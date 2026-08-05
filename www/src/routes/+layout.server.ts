import type { LayoutServerLoad } from './$types';
import { env } from '$env/dynamic/private';
import { resolveCliLinks } from '$lib/cli-release';

// Resolve the latest cli-preview-* release at build time (the layout is prerendered) and expose it to
// every route, so the CLI download links never hardcode a stale preview tag (issue #924). A
// GITHUB_TOKEN in the build environment raises the GitHub API rate limit (60/hr -> 5000/hr) so
// resolution is reliable; without it the call is unauthenticated and degrades to the releases page.
export const load: LayoutServerLoad = async ({ fetch }) => {
	return {
		cliLinks: await resolveCliLinks(fetch, env.GITHUB_TOKEN)
	};
};
