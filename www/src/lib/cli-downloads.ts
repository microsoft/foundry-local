export const FOUNDRY_LOCAL_RELEASES_URL = 'https://github.com/microsoft/Foundry-Local/releases';

export type CliDownloadLink = {
	id: 'windows-cli' | 'macos-cli' | 'linux-cli';
	label: string;
	href: string;
	releaseLabel: string;
};

export const fallbackCliDownloadLinks: CliDownloadLink[] = [
	{
		id: 'windows-cli',
		label: 'Windows',
		href: FOUNDRY_LOCAL_RELEASES_URL,
		releaseLabel: 'GitHub releases'
	},
	{
		id: 'macos-cli',
		label: 'macOS',
		href: FOUNDRY_LOCAL_RELEASES_URL,
		releaseLabel: 'GitHub releases'
	},
	{
		id: 'linux-cli',
		label: 'Linux',
		href: FOUNDRY_LOCAL_RELEASES_URL,
		releaseLabel: 'GitHub releases'
	}
];