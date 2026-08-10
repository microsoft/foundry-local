import { getCliDownloadLinks } from '$lib/server/cli-release';

export async function load({ fetch }) {
	return {
		cliDownloadLinks: await getCliDownloadLinks(fetch)
	};
}