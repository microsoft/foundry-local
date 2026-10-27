import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { createServer } from 'vite';

let server;
let getCliDownloadLinks;

before(async () => {
	server = await createServer({
		server: { middlewareMode: true },
		optimizeDeps: { noDiscovery: true },
		appType: 'custom'
	});
	({ getCliDownloadLinks } = await server.ssrLoadModule('/src/lib/server/cli-release.ts'));
});

after(async () => {
	await server?.close();
});

function cliRelease(version, assetNames) {
	const tag = `cli-preview-${version}`;
	const baseUrl = 'https://github.com/microsoft/Foundry-Local/releases';
	return {
		tag_name: tag,
		html_url: `${baseUrl}/tag/${tag}`,
		assets: assetNames.map((name) => ({
			name,
			browser_download_url: `${baseUrl}/download/${tag}/${name}`
		}))
	};
}

test('latest CLI downloads use 0.11.0 assets and labels on every platform', async () => {
	const release = cliRelease('0.11.0', [
		'foundry-0.11.0-win-arm64.msix',
		'foundry-0.11.0-win-x64.msix',
		'foundry-0.11.0-osx-arm64.zip',
		'foundry-0.11.0-osx-arm64.pkg',
		'foundry-0.11.0-linux-arm64.tar.gz',
		'foundry-0.11.0-linux-x64.tar.gz'
	]);
	const links = await getCliDownloadLinks(async () =>
		Response.json([
			{
				tag_name: 'v2.1.0',
				html_url: 'https://github.com/microsoft/Foundry-Local/releases/tag/v2.1.0'
			},
			cliRelease('0.10.3', ['foundry-0.10.3-win-x64-winml.msix']),
			release
		])
	);

	assert.deepEqual(
		links.map(({ id, href, releaseLabel }) => ({ id, href, releaseLabel })),
		[
			{ id: 'windows-cli', asset: 'foundry-0.11.0-win-x64.msix' },
			{ id: 'macos-cli', asset: 'foundry-0.11.0-osx-arm64.pkg' },
			{ id: 'linux-cli', asset: 'foundry-0.11.0-linux-x64.tar.gz' }
		].map(({ id, asset }) => ({
			id,
			href: release.assets.find(({ name }) => name === asset).browser_download_url,
			releaseLabel: 'cli-preview-0.11.0'
		}))
	);
});

test('Windows downloads still recognize the older WinML asset naming', async () => {
	const release = cliRelease('0.10.3', ['foundry-0.10.3-win-x64-winml.msix']);
	const links = await getCliDownloadLinks(async () => Response.json([release]));

	assert.equal(links[0].href, release.assets[0].browser_download_url);
	assert.equal(links[0].releaseLabel, release.tag_name);
});
