import { FOUNDRY_LOCAL_RELEASES_URL, fallbackCliDownloadLinks, type CliDownloadLink } from '$lib/cli-downloads';

type GitHubReleaseAsset = {
	name: string;
	browser_download_url: string;
};

type GitHubRelease = {
	tag_name: string;
	html_url: string;
	assets?: GitHubReleaseAsset[];
};

type FetchLike = typeof fetch;

const RELEASES_API_URL = 'https://api.github.com/repos/microsoft/Foundry-Local/releases';
const CLI_PREVIEW_TAG = /^cli-preview-(\d+)\.(\d+)\.(\d+)$/i;

export async function getCliDownloadLinks(fetcher: FetchLike): Promise<CliDownloadLink[]> {
	try {
		const response = await fetcher(RELEASES_API_URL, {
			headers: { Accept: 'application/vnd.github+json' }
		});

		if (!response.ok) {
			return fallbackCliDownloadLinks;
		}

		const releases = (await response.json()) as GitHubRelease[];
		const latestCliRelease = releases
			.filter((release) => CLI_PREVIEW_TAG.test(release.tag_name))
			.sort((left, right) => compareCliPreviewTags(right.tag_name, left.tag_name))[0];

		if (!latestCliRelease?.assets?.length) {
			return fallbackCliDownloadLinks;
		}

		return buildCliDownloadLinks(latestCliRelease);
	} catch {
		return fallbackCliDownloadLinks;
	}
}

function compareCliPreviewTags(left: string, right: string): number {
	const leftVersion = parseCliPreviewTag(left);
	const rightVersion = parseCliPreviewTag(right);

	for (let i = 0; i < leftVersion.length; i++) {
		const delta = leftVersion[i] - rightVersion[i];
		if (delta !== 0) return delta;
	}

	return 0;
}

function parseCliPreviewTag(tag: string): [number, number, number] {
	const match = CLI_PREVIEW_TAG.exec(tag);
	if (!match) return [0, 0, 0];
	return [Number(match[1]), Number(match[2]), Number(match[3])];
}

function buildCliDownloadLinks(release: GitHubRelease): CliDownloadLink[] {
	const fallbackById = new Map(fallbackCliDownloadLinks.map((link) => [link.id, link]));
	const releaseLabel = release.tag_name;

	return [
		buildLink(fallbackById.get('windows-cli')!, release, releaseLabel, /win-x64-winml\.msix$/i),
		buildLink(fallbackById.get('macos-cli')!, release, releaseLabel, /osx-arm64\.pkg$/i),
		buildLink(fallbackById.get('linux-cli')!, release, releaseLabel, /linux-x64\.tar\.gz$/i)
	];
}

function buildLink(
	fallback: CliDownloadLink,
	release: GitHubRelease,
	releaseLabel: string,
	assetPattern: RegExp
): CliDownloadLink {
	const asset = release.assets?.find((candidate) => assetPattern.test(candidate.name));

	return {
		...fallback,
		href: asset?.browser_download_url ?? release.html_url ?? FOUNDRY_LOCAL_RELEASES_URL,
		releaseLabel: asset ? releaseLabel : 'GitHub releases'
	};
}