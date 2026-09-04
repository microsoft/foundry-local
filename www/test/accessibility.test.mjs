import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

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
