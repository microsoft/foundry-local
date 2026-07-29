import tailwindcss from '@tailwindcss/vite';
import { sveltekit } from '@sveltejs/kit/vite';
import { defineConfig } from 'vite';

export default defineConfig({
	plugins: [tailwindcss(), sveltekit()],
	server: {
		allowedHosts: ['.ngrok-free.app']
	},
	build: {
		rollupOptions: {
			output: {
				manualChunks: (id) => {
					// Split vendor code into separate chunks
					if (id.includes('node_modules')) {
						// Large icon library
						if (id.includes('lucide-svelte')) {
							return 'vendor-icons';
						}
						// UI component libraries
						if (id.includes('bits-ui') || id.includes('formsnap') || id.includes('svelte-sonner')) {
							return 'vendor-ui';
						}
						// Date/time libraries
						if (id.includes('@internationalized') || id.includes('date-fns')) {
							return 'vendor-date';
						}
						// Other large dependencies can stay in the main vendor chunk
					}
					// Don't split Svelte core - let SvelteKit handle it
				}
			}
		}
	}
});
