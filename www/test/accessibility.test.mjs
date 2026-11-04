import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import test from 'node:test';
import { transpileModule } from 'typescript';

const readSource = (path) => readFileSync(new URL(path, import.meta.url), 'utf8');

function getOpeningTag(source, tagName) {
	const start = source.indexOf(`<${tagName}`);
	assert.notEqual(start, -1, `Expected <${tagName}>`);

	let braceDepth = 0;
	let quote = null;
	for (let index = start; index < source.length; index += 1) {
		const character = source[index];
		const previous = source[index - 1];

		if (quote !== null) {
			if (character === quote && previous !== '\\') quote = null;
			continue;
		}
		if (character === '"' || character === "'" || character === '`') {
			quote = character;
		} else if (character === '{') {
			braceDepth += 1;
		} else if (character === '}') {
			braceDepth -= 1;
		} else if (character === '>' && braceDepth === 0) {
			return { start, end: index + 1, source: source.slice(start, index + 1) };
		}
	}

	throw new Error(`Unterminated <${tagName}>`);
}

function getCssRule(source, selector) {
	const escapedSelector = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
	const match = source.match(new RegExp(`${escapedSelector}\\s*\\{([^}]+)\\}`));
	assert.ok(match, `Expected CSS rule for ${selector}`);
	return match[1];
}

test('ModelCard uses sibling controls instead of nested interactive descendants', () => {
	const modelCard = readSource('../src/routes/models/components/ModelCard.svelte');
	const cardComponent = readSource('../src/lib/components/ui/card/card.svelte');
	const cardRoot = getOpeningTag(modelCard, 'Card.Root');

	assert.match(cardComponent, /<div\b/);
	assert.doesNotMatch(cardRoot.source, /\b(?:onclick|onkeydown|role|tabindex)=/);

	const detailsMarker = 'class="model-card-details ';
	const detailsClass = modelCard.indexOf(detailsMarker, cardRoot.end);
	assert.notEqual(detailsClass, -1);
	const detailsStart = modelCard.lastIndexOf('<button', detailsClass);
	const detailsOpen = getOpeningTag(modelCard.slice(detailsStart), 'button');
	const detailsEnd = modelCard.indexOf('</button>', detailsStart) + '</button>'.length;
	const detailsButton = modelCard.slice(detailsStart, detailsEnd);

	assert.match(detailsOpen.source, /type="button"/);
	assert.match(detailsOpen.source, /\bz-10\b/);
	assert.match(detailsOpen.source, /onclick=\{\(\) => onCardClick\(model\)\}/);
	assert.match(detailsOpen.source, /aria-label=\{`View details for \$\{model\.displayName\}`\}/);
	assert.doesNotMatch(detailsButton.slice(detailsOpen.end), /<(?:a|button|Button)\b/);
	assert.match(modelCard.slice(cardRoot.end, detailsStart), /^\s*$/);

	const cardHeader = modelCard.indexOf('<Card.Header', detailsEnd);
	assert.notEqual(cardHeader, -1);
	assert.match(modelCard.slice(detailsEnd, cardHeader), /^\s*$/);
	assert.match(modelCard.slice(cardHeader), /<a\b/);
	assert.match(modelCard.slice(cardHeader), /<Button\b/);

	const foregroundActions =
		modelCard.match(/<(?:a|Button)\b[\s\S]*?\bclass="[^"]*\bz-20\b[^"]*"/g) ?? [];
	assert.equal(foregroundActions.length, 4);
});

test('model task and capability filters have associated labels', () => {
	const modelFilters = readSource('../src/routes/models/components/ModelFilters.svelte');

	assert.match(modelFilters, /<Label for="task">Task<\/Label>/);
	assert.match(modelFilters, /<Button\b[^>]*\bid="task"[^>]*>/);
	assert.match(modelFilters, /<Label for="capability">Capability<\/Label>/);
	assert.match(modelFilters, /<Button\b[^>]*\bid="capability"[^>]*>/);
});

test('home feature cards are keyboard-focusable named groups', () => {
	const features = readSource('../src/lib/components/home/features.svelte');

	const featureIds = [
		'sdk-lifecycle',
		'hardware',
		'offline-runtime',
		'native-sdks',
		'native-rest',
		'data-privacy'
	];

	for (const featureId of featureIds) {
		assert.match(
			features,
			new RegExp(
				`<div\\s+tabindex="0"\\s+role="group"\\s+aria-labelledby="${featureId}-title"\\s+aria-describedby="${featureId}-description"`
			)
		);
	}
	assert.match(features, /focus-visible:ring-2/);
});

test('model dropdown triggers are labelled by their visible labels and values', () => {
	const modelFilters = readSource('../src/routes/models/components/ModelFilters.svelte');

	assert.match(modelFilters, /<Label id="sort-by">Sort By<\/Label>/);
	assert.match(modelFilters, /<Button\b[^>]*aria-labelledby="sort-by sort-by-value"[^>]*>/);
	assert.match(modelFilters, /<span id="sort-by-value">/);
	assert.match(modelFilters, /<Label id="model-family">Model Family<\/Label>/);
	assert.match(
		modelFilters,
		/<Button\b[^>]*aria-labelledby="model-family model-family-value"[^>]*>/
	);
	assert.match(modelFilters, /<span id="model-family-value">/);
	assert.match(modelFilters, /<Label id="acceleration">Acceleration<\/Label>/);
	assert.match(
		modelFilters,
		/<Button\b[^>]*aria-labelledby="acceleration acceleration-value"[^>]*>/
	);
	assert.match(modelFilters, /<span id="acceleration-value">/);
});

test('download menu copy actions participate in keyboard menu navigation', () => {
	const downloadDropdown = readSource('../src/lib/components/download-dropdown.svelte');

	const copyItem = getOpeningTag(downloadDropdown, 'DropdownMenu.Item');
	assert.ok(
		copyItem.source.includes(
			'onclick={() => copyToClipboard(item.crossPlatformCommand, item.crossPlatformId)}'
		)
	);
	assert.ok(copyItem.source.includes('aria-label={`Copy ${item.label} installation command`}'));
	assert.doesNotMatch(
		downloadDropdown,
		/<button[\s\S]*?onclick=\{\(\) => copyToClipboard\(item\.crossPlatformCommand, item\.crossPlatformId\)\}/
	);
});

test('model filter results are announced through a live status region', () => {
	const modelFilters = readSource('../src/routes/models/components/ModelFilters.svelte');

	assert.match(
		modelFilters,
		/<div role="status"[^>]*>[\s\S]*?\{#if isFiltering\}[\s\S]*?\{filteredCount\} model/
	);
});

test('sort-order control has a purpose-based accessible name', () => {
	const modelFilters = readSource('../src/routes/models/components/ModelFilters.svelte');

	assert.match(
		modelFilters,
		/aria-label=\{`Sort order: \$\{sortOrder === 'asc' \? 'ascending' : 'descending'\}`\}/
	);
});

test('execution device filters have a shared native group label', () => {
	const modelFilters = readSource('../src/routes/models/components/ModelFilters.svelte');

	assert.match(
		modelFilters,
		/<fieldset>\s*<legend[^>]*>Execution Device<\/legend>[\s\S]*?\{#each availableDevices as device\}/
	);
});

test('device and SDK selectors expose their pressed state', () => {
	const modelFilters = readSource('../src/routes/models/components/ModelFilters.svelte');
	const installCommand = readSource('../src/lib/components/install-command.svelte');

	assert.match(modelFilters, /aria-pressed=\{selectedDevices\.includes\(device\)\}/);
	assert.match(installCommand, /aria-pressed=\{activeTab === tab\.key\}/);
});

test('C and C++ selectors provide package installation and complete native examples', () => {
	const installCommand = readSource('../src/lib/components/install-command.svelte');
	const dropdown = readSource('../src/lib/components/download-dropdown.svelte');

	for (const [key, label] of [
		['c', 'C'],
		['cpp', 'C++']
	]) {
		assert.ok(installCommand.includes(`{ key: '${key}', label: '${label}' }`));
		assert.ok(
			installCommand.includes(`${key}: 'nuget install Microsoft.AI.Foundry.Local.Runtime'`)
		);
		assert.equal(installCommand.split(`${key}: nativeSnippet(String.raw`).length - 1, 2);
		assert.ok(dropdown.includes(`label: '${label} SDK'`));
	}

	assert.match(installCommand, /flex flex-wrap justify-center/);
	assert.doesNotMatch(installCommand, /Combine steps 2 and 3|Runtime package's|#if isNative/);
	assert.doesNotMatch(installCommand, /#build-from-source/);
	assert.doesNotMatch(dropdown, /Build from source/);
});

test('native examples use the shared syntax colors without changing the displayed or copied code', () => {
	const source = readSource('../src/lib/components/install-command.svelte');
	const helper = source.slice(
		source.indexOf('function nativeSnippet'),
		source.indexOf('const tabs:')
	);
	const { outputText } = transpileModule(helper, {});
	const nativeSnippet = runInNewContext(`${outputText}; nativeSnippet`);
	const examples = [...source.matchAll(/(?:c|cpp): nativeSnippet\(String\.raw`([\s\S]*?)`\)/g)];

	assert.equal(examples.length, 4);
	for (const [, raw] of examples) {
		const snippet = nativeSnippet(raw);
		assert.equal(snippet.raw, raw);
		for (const style of ['keyword', 'type', 'call', 'string']) {
			assert.ok(snippet.html.includes(`class="code-${style}"`));
		}
		const displayed = snippet.html
			.replace(/<span class="code-[a-z]+">|<\/span>/g, '')
			.replaceAll('&lt;', '<')
			.replaceAll('&gt;', '>')
			.replaceAll('&amp;', '&');
		assert.equal(displayed, raw);
	}

	for (const text of [
		'<script>&\\"Hello\\"</script>',
		'<SCRIPT SRC=x></SCRIPT>',
		'<ScRiPt type=x>alert(1)</sCrIpT foo=bar>',
		'<img src=x onerror=alert(1)>',
		'<!-- comment --!>'
	]) {
		const raw = `puts("${text}");`;
		const snippet = nativeSnippet(raw);
		const encoded = snippet.html.replace(/<span class="code-[a-z]+">|<\/span>/g, '');
		assert.equal(snippet.raw, raw);
		assert.ok(snippet.html.includes('class="code-string"'));
		assert.equal(encoded.includes('<'), false);
		assert.equal(encoded.includes('>'), false);
		assert.equal(
			encoded.replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&'),
			raw
		);
	}
});

test('quickstarts display and copy one complete example instead of separate code fragments', () => {
	const installCommand = readSource('../src/lib/components/install-command.svelte');

	assert.match(
		installCommand,
		/raw: `\$\{setupSnippets\[activeTab\]\.raw\}\\n\\n\$\{inferenceSnippets\[activeTab\]\.raw\}`/
	);
	assert.match(installCommand, /navigator\.clipboard\.writeText\(currentExample\.raw\)/);
	assert.match(installCommand, /aria-label="Copy complete example"/);
	assert.match(installCommand, /aria-label="Reveal complete example"/);
	assert.match(installCommand, /\{@html currentExample\.html\}/);
	assert.doesNotMatch(installCommand, /Copy model setup|Copy inference call|revealedStep/);
});

test('ModelDetailsModal describes Copy ID buttons with visible model IDs', () => {
	const modelDetailsModal = readSource('../src/routes/models/components/ModelDetailsModal.svelte');

	assert.match(modelDetailsModal, /id=\{`model-id-\$\{genericModelName\}`\}/);
	assert.match(modelDetailsModal, /id=\{`model-id-\$\{variant\.name\}`\}/);
	assert.match(modelDetailsModal, /aria-describedby=\{`model-id-\$\{genericModelName\}`\}/);
	assert.match(modelDetailsModal, /aria-describedby=\{`model-id-\$\{variant\.name\}`\}/);
	assert.doesNotMatch(modelDetailsModal, /aria-labelledby=\{`model-id-/);
});

test('model-card copy buttons describe their Foundry run commands', () => {
	const modelCard = readSource('../src/routes/models/components/ModelCard.svelte');

	assert.match(modelCard, /aria-describedby=\{`run-command-\$\{genericModelName\}`\}/);
	assert.match(
		modelCard,
		/<\/Tooltip\.Trigger>\s*<span id=\{`run-command-\$\{genericModelName\}`\} class="sr-only">\s*\{formatModelCommand\(genericModelName\)\}/
	);
	assert.match(modelCard, /aria-describedby=\{`run-command-\$\{variant\.name\}`\}/);
	assert.match(
		modelCard,
		/<\/Tooltip\.Trigger>\s*<span id=\{`run-command-\$\{variant\.name\}`\} class="sr-only">\s*\{formatModelCommand\(variant\.name\)\}/
	);
});

test('back-to-top control has a visible keyboard focus indicator', () => {
	const backToTop = readSource('../src/lib/components/back-to-top.svelte');

	assert.match(backToTop, /focus-visible:ring-2/);
	assert.match(backToTop, /focus-visible:ring-offset-2/);
	assert.match(backToTop, /focus-visible:ring-offset-background/);
});

test('shared skip link is hidden until keyboard focus and targets both main landmarks', () => {
	const skipLink = readSource('../src/lib/components/skip-link.svelte');
	const nav = readSource('../src/lib/components/home/nav.svelte');
	const homePage = readSource('../src/routes/+page.svelte');
	const modelsPage = readSource('../src/routes/models/+page.svelte');
	const styles = readSource('../src/app.css');

	assert.match(skipLink, /<a href="#main-content" class="skip-link">Skip to main content<\/a>/);
	assert.match(nav, /import SkipLink from '\$lib\/components\/skip-link\.svelte';/);
	assert.match(nav, /<SkipLink \/>/);
	assert.match(homePage, /<main id="main-content">/);
	assert.match(modelsPage, /<main id="main-content"(?:\s|>)/);

	const hiddenRule = getCssRule(styles, '.skip-link');
	assert.match(hiddenRule, /position:\s*fixed/);
	assert.match(hiddenRule, /top:\s*1rem/);
	assert.match(hiddenRule, /left:\s*1rem/);
	assert.match(hiddenRule, /z-index:\s*100/);
	assert.match(hiddenRule, /width:\s*1px/);
	assert.match(hiddenRule, /height:\s*1px/);
	assert.match(hiddenRule, /overflow:\s*hidden/);
	assert.match(hiddenRule, /clip-path:\s*inset\(50%\)/);

	const focusRule = getCssRule(styles, '.skip-link:focus-visible');
	assert.match(focusRule, /width:\s*auto/);
	assert.match(focusRule, /height:\s*auto/);
	assert.match(focusRule, /overflow:\s*visible/);
	assert.match(focusRule, /clip-path:\s*none/);
});

test('dropdown menus remain scrollable within the viewport below the sticky header', () => {
	const content = readSource('../src/lib/components/ui/dropdown-menu/dropdown-menu-content.svelte');

	assert.match(content, /max-h-\[var\(--bits-dropdown-menu-content-available-height\)\]/);
	assert.match(content, /overflow-y-auto/);
});
