import { defineConfig } from 'vitest/config';

// Standalone vitest config (kept separate from vite.config.ts so unit tests run in a plain Node
// environment without loading the SvelteKit plugin). Suited to pure-logic modules like cli-release.ts.
export default defineConfig({
	test: {
		environment: 'node',
		include: ['src/**/*.test.ts']
	}
});
