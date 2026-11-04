import type { RequestHandler } from '@sveltejs/kit';

const CATALOG_ENDPOINT = 'https://api.catalog.azureml.ms/asset-gallery/v1.0/models';

export const POST: RequestHandler = async ({ request, fetch }) => {
	const response = await fetch(CATALOG_ENDPOINT, {
		method: 'POST',
		headers: {
			'Content-Type': 'application/json',
			Accept: 'application/json',
			'User-Agent': 'AzureAiStudio',
			'x-ms-use-full-service-contracts': 'true'
		},
		body: await request.text()
	});

	return new Response(response.body, {
		status: response.status,
		headers: {
			'Content-Type': response.headers.get('Content-Type') ?? 'application/json',
			'Cache-Control': 'no-store'
		}
	});
};
