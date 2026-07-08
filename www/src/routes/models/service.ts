// API service for Azure AI Foundry models
// Uses Azure Function as CORS proxy for static deployment
//
// This service implements the Azure Foundry Model Catalog API specification:
// - Fetches models from ai.azure.com API endpoint (via proxy)
// - Supports pagination with skip and continuationToken
// - Filters by device (CPU/GPU/NPU) and execution provider (CPUExecutionProvider, CUDAExecutionProvider, etc.)
// - Includes required filters: type, kind, labels (latest), foundryLocal tag
// - Validates prompt templates (required except for gpt-oss-* models)
// - Handles parent-variant relationships for model organization
// - Sorts variants by priority (NPU > vendor GPU > generic GPU > vendor CPU > generic CPU)
// - Applies platform-specific filtering (ARM64 incompatibilities)
// - Caches results to minimize API calls
import type { FoundryModel, GroupedFoundryModel } from './types';

// Azure Function endpoint for CORS proxy
const FOUNDRY_API_ENDPOINT =
	'https://foundry-local-cors-proxy.azurewebsites.net/api/foundryproxy';

export interface ApiFilters {
	device?: string;
	family?: string;
	acceleration?: string;
	searchTerm?: string;
}

export interface ApiSortOptions {
	sortBy: string;
	sortOrder: 'asc' | 'desc';
}

export class FoundryModelService {
	// Cache for all models - fetched once
	private allModelsCache: FoundryModel[] | null = null;
	private fetchPromise: Promise<FoundryModel[]> | null = null;

	// Blocked model IDs that should not be displayed
	private readonly BLOCKED_MODEL_IDS = new Set([
		'deepseek-r1-distill-qwen-1.5b-generic-cpu',
		'deepseek-r1-distill-qwen-7b-generic-cpu',
		'deepseek-r1-distill-llama-8b-generic-cpu',
		'deepseek-r1-distill-qwen-14b-generic-cpu',
		'Phi-4-mini-instruct-generic-cpu',
		'deepseek-r1-distill-llama-8b-cuda-gpu',
		'deepseek-r1-distill-llama-8b-generic-gpu'
	]);

	// Models unsupported on ARM64 with INT8 quantization
	private readonly UNSUPPORTED_ON_ARM64 = new Set([
		'deepseek-r1-distill-qwen-1.5b-generic-cpu',
		'deepseek-r1-distill-qwen-7b-generic-cpu',
		'deepseek-r1-distill-llama-8b-generic-cpu',
		'deepseek-r1-distill-qwen-14b-generic-cpu',
		'Phi-4-mini-instruct-generic-cpu'
	]);

	// Detect if running on ARM64 (this is best-effort in browser context)
	private isArm64Platform(): boolean {
		if (typeof navigator !== 'undefined') {
			const userAgent = navigator.userAgent.toLowerCase();
			// Best-effort: Check for ARM indicators in user agent (may not be reliable in all browsers/platforms)
			return userAgent.includes('arm') || userAgent.includes('aarch64');
		}
		return false;
	}

	// Acceleration patterns mapped to their identifiers
	private static readonly ACCELERATION_PATTERNS: Array<{ patterns: string[]; acceleration: string }> = [
		{ patterns: ['-qnn-', '-qnn'], acceleration: 'qnn' },
		{ patterns: ['-vitis-', '-vitis', '-vitisai'], acceleration: 'vitis' },
		{ patterns: ['-openvino-', '-openvino'], acceleration: 'openvino' },
		{ patterns: ['-trtrtx-', '-trtrtx'], acceleration: 'trtrtx' },
		{ patterns: ['-trt-rtx-', '-tensorrt-', '-trt-rtx', '-tensorrt'], acceleration: 'trt-rtx' },
		{ patterns: ['-cuda-', '-cuda'], acceleration: 'cuda' },
		{ patterns: ['-webgpu-', '-webgpu', 'webgpu', '-generic-gpu'], acceleration: 'webgpu' }
	];

	// Detect acceleration from model name
	private detectAcceleration(modelName: string): string | undefined {
		const nameLower = modelName.toLowerCase();
		for (const { patterns, acceleration } of FoundryModelService.ACCELERATION_PATTERNS) {
			if (patterns.some(pattern => nameLower.includes(pattern))) {
				return acceleration;
			}
		}
		return undefined;
	}

	// Acceleration display name mapping
	private static readonly ACCELERATION_DISPLAY_NAMES: Record<string, string> = {
		qnn: 'Qualcomm QNN',
		vitis: 'AMD Vitis AI',
		openvino: 'Intel OpenVINO',
		cuda: 'NVIDIA CUDA',
		'trt-rtx': 'NVIDIA TensorRT RTX',
		trtrtx: 'NVIDIA TensorRT RTX',
		webgpu: 'WebGPU'
	};

	// Get acceleration display name
	getAccelerationDisplayName(acceleration: string): string {
		return FoundryModelService.ACCELERATION_DISPLAY_NAMES[acceleration] || acceleration;
	}

	// License URL mapping patterns
	private static readonly LICENSE_URL_PATTERNS: Array<{ pattern: string; url: string; checkVersion?: boolean }> = [
		{ pattern: 'mit', url: 'https://opensource.org/licenses/MIT' },
		{ pattern: 'apache', url: 'https://www.apache.org/licenses/', checkVersion: true },
		{ pattern: 'bsd', url: 'https://opensource.org/licenses/BSD-3-Clause' },
		{ pattern: 'gpl', url: 'https://www.gnu.org/licenses/gpl-3.0.en.html' },
		{ pattern: 'mistral', url: 'https://mistral.ai/terms-of-use/' },
		{ pattern: 'llama', url: 'https://llama.meta.com/llama-downloads/' },
		{ pattern: 'deepseek', url: '/licenses/deepseek' },
		{ pattern: 'phi', url: '/licenses/phi' }
	];

	// Get license URL for clickable links
	getLicenseUrl(license: string): string | null {
		const licenseLower = license.toLowerCase();

		for (const { pattern, url, checkVersion } of FoundryModelService.LICENSE_URL_PATTERNS) {
			if (licenseLower.includes(pattern)) {
				if (checkVersion && pattern === 'apache' && license.includes('2.0')) {
					return 'https://www.apache.org/licenses/LICENSE-2.0';
				}
				return url;
			}
		}

		return null;
	}

	// Fetch all models from API once and cache them
	async fetchAllModels(): Promise<FoundryModel[]> {
		// Return cached models if available
		if (this.allModelsCache) {
			return this.allModelsCache;
		}

		// If already fetching, return the existing promise
		if (this.fetchPromise) {
			return this.fetchPromise;
		}

		// Create fetch promise
		this.fetchPromise = this.fetchModelsFromAPI();

		try {
			this.allModelsCache = await this.fetchPromise;
			return this.allModelsCache;
		} finally {
			this.fetchPromise = null;
		}
	}

	// Clear cache if needed (for refresh)
	clearCache(): void {
		this.allModelsCache = null;
		this.fetchPromise = null;
	}

	async fetchModels(
		filters: ApiFilters = {},
		sortOptions: ApiSortOptions = { sortBy: 'lastModified', sortOrder: 'desc' }
	): Promise<FoundryModel[]> {
		// Get all models from cache or API
		const allModels = await this.fetchAllModels();

		// Apply filters and sorting client-side
		const filteredModels = this.applyClientSideProcessing(allModels, filters, sortOptions);

		return filteredModels;
	}

	private async fetchModelsFromAPI(): Promise<FoundryModel[]> {
		// Define device/execution provider combinations as per spec
		// Each device can have multiple execution providers
		const deviceConfigs = [
			{
				device: 'NPU',
				executionProviders: [
					'QNNExecutionProvider',
					'VitisAIExecutionProvider',
					'OpenVINOExecutionProvider'    // OpenVINO can run on NPU
				]
			},
			{
				device: 'GPU',
				executionProviders: [
					'CUDAExecutionProvider',      // NVIDIA CUDA
					'TensorrtExecutionProvider',   // NVIDIA TensorRT
					'NvTensorRTRTXExecutionProvider', // NVIDIA TensorRT RTX (TRTRTX)
					'WebGpuExecutionProvider',     // WebGPU
					'OpenVINOExecutionProvider',   // OpenVINO can run on GPU
					'VitisAIExecutionProvider'     // AMD Vitis AI can run on GPU
				]
			},
			{
				device: 'CPU',
				executionProviders: [
					'CPUExecutionProvider',        // Generic CPU
					'OpenVINOExecutionProvider',   // Intel OpenVINO
					'VitisAIExecutionProvider'     // AMD Vitis AI (can run on CPU)
				]
			}
		];

		const modelMap = new Map<string, FoundryModel[]>();

		for (const config of deviceConfigs) {
			for (const executionProvider of config.executionProviders) {
				try {
					const deviceModels = await this.fetchModelsForDeviceAndEP(
						config.device,
						executionProvider
					);

					// Organize models by parent-variant relationship
					for (const model of deviceModels) {
						const key = model.parentModelUri || model.id;
						if (!modelMap.has(key)) {
							modelMap.set(key, []);
						}
						modelMap.get(key)!.push(model);
					}
				} catch (error) {
					console.error(
						`Failed to fetch models for ${config.device}/${executionProvider}:`,
						error
					);
					// Continue with other configs if one fails
				}
			}
		}

		// Sort variants by priority and flatten
		const sortedModels: FoundryModel[] = [];
		for (const variants of modelMap.values()) {
			variants.sort((a, b) => this.getModelPriority(a.name) - this.getModelPriority(b.name));
			sortedModels.push(...variants);
		}

		return sortedModels;
	}

	// Get model priority based on device type (lower = higher priority)
	// Priority levels for device types
	private static readonly PRIORITY_NPU = 0;
	private static readonly PRIORITY_VENDOR_GPU = 1;
	private static readonly PRIORITY_GENERIC_GPU = 2;
	private static readonly PRIORITY_VENDOR_CPU = 3;
	private static readonly PRIORITY_GENERIC_CPU = 4;
	private static readonly PRIORITY_UNKNOWN = 5;

	private getModelPriority(name: string): number {
		const nameLower = name.toLowerCase();
		if (nameLower.includes('-npu:') || nameLower.includes('-npu-')) return FoundryModelService.PRIORITY_NPU; // NPU highest
		if (nameLower.includes('-gpu:') && !nameLower.includes('generic')) return FoundryModelService.PRIORITY_VENDOR_GPU; // Vendor GPU
		if (nameLower.includes('-generic-gpu:') || nameLower.includes('-generic-gpu-')) return FoundryModelService.PRIORITY_GENERIC_GPU; // Generic GPU
		if (nameLower.includes('-cpu:') && !nameLower.includes('generic')) return FoundryModelService.PRIORITY_VENDOR_CPU; // Vendor CPU
		if (nameLower.includes('-generic-cpu:') || nameLower.includes('-generic-cpu-')) return FoundryModelService.PRIORITY_GENERIC_CPU; // Generic CPU
		return FoundryModelService.PRIORITY_UNKNOWN; // Unknown
	}

	private async fetchModelsForDeviceAndEP(
		device: string,
		executionProvider: string
	): Promise<FoundryModel[]> {
		const allModels: FoundryModel[] = [];
		let skip: number | null = null;
		let continuationToken: string | null = null;
		let hasMore = true;

		// Paginate through all results
		while (hasMore) {
			const requestBody = this.buildRequestBody(device, executionProvider, skip, continuationToken);

			try {
				// NOTE: Content-Type is intentionally "text/plain" (not "application/json").
				// This keeps the request a CORS "simple request" so the browser does NOT
				// send a preflight OPTIONS. The Azure Functions host intercepts preflight
				// OPTIONS before our proxy code runs and returns 204 with no
				// Access-Control-Allow-Origin header, which fails CORS. The proxy parses the
				// body as JSON regardless of Content-Type, so sending text/plain is safe and
				// avoids the preflight entirely. Do not change this back to application/json
				// unless platform-level CORS is configured on the Function App.
				const response = await fetch(FOUNDRY_API_ENDPOINT, {
					method: 'POST',
					headers: {
						'Content-Type': 'text/plain',
						Accept: 'application/json'
					},
					body: JSON.stringify(requestBody)
				});

				if (!response.ok) {
					console.error(`API request failed with status: ${response.status} ${response.statusText}`);
					break;
				}

				const responseBody = await response.text();
				if (!responseBody) {
					console.error('Empty response body from API');
					break;
				}

				const apiData = JSON.parse(responseBody);

				if (!apiData.indexEntitiesResponse?.value) {
					console.error('Invalid response structure');
					break;
				}

				const pageModels = this.transformApiResponse(apiData);
				allModels.push(...pageModels);

				// Update pagination parameters
				skip = apiData.indexEntitiesResponse?.nextSkip || null;
				continuationToken = apiData.indexEntitiesResponse?.continuationToken || null;
				hasMore = skip !== null || continuationToken !== null;
			} catch (error) {
				console.error('Failed to fetch foundry models:', error);
				break;
			}
		}

		return allModels;
	}

	private buildRequestBody(
		device: string,
		executionProvider: string,
		skip: number | null = null,
		continuationToken: string | null = null
	) {
		const filters = [
			{
				field: 'type',
				operator: 'eq',
				values: ['models']
			},
			{
				field: 'kind',
				operator: 'eq',
				values: ['Versioned']
			},
			{
				field: 'labels',
				operator: 'eq',
				values: ['latest']
			},
			{
				field: 'annotations/tags/foundryLocal',
				operator: 'eq',
				values: ['', 'test']
			},
			{
				field: 'properties/variantInfo/variantMetadata/device',
				operator: 'eq',
				values: [device]
			},
			{
				field: 'properties/variantInfo/variantMetadata/executionProvider',
				operator: 'eq',
				values: [executionProvider]
			}
		];

		const requestBody = {
			resourceIds: [
				{
					resourceId: 'azureml',
					entityContainerType: 'Registry'
				}
			],
			indexEntitiesRequest: {
				filters: filters,
				pageSize: 50,
				skip: skip,
				continuationToken: continuationToken
			}
		};

		return requestBody;
	}

	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	private transformApiResponse(apiData: any): FoundryModel[] {
		// Handle the Azure AI Foundry API response structure
		let entities = [];

		// Check for entities in the response structure
		if (
			apiData?.indexEntitiesResponse?.value &&
			Array.isArray(apiData.indexEntitiesResponse.value)
		) {
			entities = apiData.indexEntitiesResponse.value;
		}
		// Fallback: check if entities are directly in the response
		else if (apiData?.entities && Array.isArray(apiData.entities)) {
			entities = apiData.entities;
		}

		if (entities.length === 0) {
			return [];
		}

		// eslint-disable-next-line @typescript-eslint/no-explicit-any
		return entities
			.map((entity: any) => this.transformSingleModel(entity))
			.filter((model: FoundryModel) => {
				// Filter out blocked models
				if (this.BLOCKED_MODEL_IDS.has(model.name)) {
					return false;
				}

				// Filter out models tagged to hide
				if (model.tags.some((tag) => tag.toLowerCase() === 'foundrylocal:hide')) {
					return false;
				}

				// Filter out test models unless they start with gpt-oss-
				if (model.isTestModel && !model.name.startsWith('gpt-oss-')) {
					return false;
				}

				// Prompt template validation: filter out models without prompt templates
				// UNLESS they start with "gpt-oss-" OR have a task type of chat-completion
				// Some models may not have promptTemplate but are still valid chat models
				if (!this.isValidChatModel(model)) {
					return false;
				}				// Platform-specific filtering: ARM64 with INT8 quantization
				if (this.isArm64Platform()) {
					// Extract model name without version
					const baseName = model.name.split(':')[0];
					if (this.UNSUPPORTED_ON_ARM64.has(baseName)) {
						return false;
					}
				}

				return true;
			});
	}

	// Helper to validate if a model is a valid chat model for prompt template filtering
	private isValidChatModel(model: FoundryModel): boolean {
		const normalizedTaskType =
			typeof model.taskType === 'string' ? model.taskType.toLowerCase() : '';
		const normalizedName = model.name.toLowerCase();
		
		// Allow speech-to-text / whisper models (they don't have prompt templates but are valid)
		if (normalizedTaskType.includes('automatic-speech-recognition') || 
			normalizedTaskType.includes('speech-to-text') ||
			normalizedName.includes('whisper')) {
			return true;
		}
		
		const validTaskTypeKeywords = [
			'chat',
			'completion',
			'text-generation',
			'text generation',
			'instruct',
			'instruction',
			'reasoning'
		];
		const hasValidTaskType =
			normalizedTaskType.length > 0 &&
			validTaskTypeKeywords.some((keyword) => normalizedTaskType.includes(keyword));

		return (
			model.name.startsWith('gpt-oss-') ||
			!!model.promptTemplate ||
			hasValidTaskType
		);
	}

	// eslint-disable-next-line @typescript-eslint/no-explicit-any
	private transformSingleModel(entity: any): FoundryModel {
		// Extract assetId (URI) - this is the unique identifier
		const uri = entity.assetId || entity.entityId || '';

		// Extract name from properties.name or entityResourceName
		const modelName = entity.properties?.name || entity.entityResourceName || entity.annotations?.name || 'Unknown Model';

		// Extract version from assetId or properties.version
		let version = entity.properties?.version?.toString() || entity.version || '1';
		if (uri && !version) {
			const versionMatch = uri.match(/\/versions\/(\d+)/);
			if (versionMatch) {
				version = versionMatch[1];
			}
		}

		// Construct name with version as per spec: {properties.name}:{version}
		const nameWithVersion = `${modelName}:${version}`;

		// Extract parent model URI
		const parentModelUri = entity.properties?.variantInfo?.parents?.[0]?.assetId || null;

		// Extract device and execution provider
		const device = entity.properties?.variantInfo?.variantMetadata?.device || '';
		const executionProvider = entity.properties?.variantInfo?.variantMetadata?.executionProvider || '';

		// Device support array
		const deviceSupport: string[] = device ? [device.toLowerCase()] : [];

		// Extract display name from systemCatalogData or annotations.name
		// systemCatalogData.displayName is the preferred clean name from the API
		let displayName = entity.annotations?.systemCatalogData?.displayName ||
			entity.annotations?.name;

		// If no display name found, create a clean one from the model name
		if (!displayName || displayName === modelName) {
			displayName = this.createDisplayName(this.extractAlias(modelName));
		}

		// Extract description
		const description = entity.annotations?.description || entity.description || `${displayName} model`;

		// Extract tags
		const tags: string[] = [];
		if (entity.annotations?.tags) {
			Object.entries(entity.annotations.tags).forEach(([key, value]) => {
				if (typeof value === 'string' && value && value !== 'true' && value !== 'false') {
					tags.push(`${key}:${value}`);
				} else if (value === true || value === 'true') {
					tags.push(key);
				}
			});
		}

		// Add labels to tags
		if (entity.annotations?.labels && Array.isArray(entity.annotations.labels)) {
			tags.push(...entity.annotations.labels.map((l: string) => `label:${l}`));
		}

		// Extract prompt template
		const promptTemplate = entity.annotations?.tags?.promptTemplate || null;

		// Extract tool calling support
		const supportsToolCalling = entity.annotations?.tags?.supportsToolCalling === 'true';

		// Extract alias
		const alias = entity.annotations?.tags?.alias || this.extractAlias(modelName);

		// Check if this is a test model
		const isTestModel = entity.annotations?.tags?.foundryLocal === 'test';

		// Extract publisher
		const publisher = entity.annotations?.systemCatalogData?.publisher || entity.annotations?.publisher || 'Azure';

		// Extract file size
		const fileSizeBytes = entity.properties?.variantInfo?.variantMetadata?.fileSizeBytes || 0;
		const vRamFootprintBytes = entity.properties?.variantInfo?.variantMetadata?.vRamFootprintBytes || 0;

		// Extract quantization info
		const quantization = entity.properties?.variantInfo?.variantMetadata?.quantization || [];

		// Detect acceleration from model name
		const acceleration = this.detectAcceleration(modelName);

		// Extract max output tokens
		const maxOutputTokens = entity.annotations?.systemCatalogData?.maxOutputTokens ||
			parseInt(entity.annotations?.tags?.maxOutputTokens || '0', 10) ||
			undefined;

		// Extract min FL version
		const minFLVersion = entity.properties?.minFLVersion || undefined;

		// Extract task type
		const taskType = entity.annotations?.tags?.task || 'chat-completion';

		// Extract license
		const license = entity.annotations?.tags?.license || entity.properties?.license || undefined;
		const licenseDescription = entity.annotations?.tags?.licenseDescription || undefined;

		// Extract model type
		const modelType = entity.properties?.variantInfo?.variantMetadata?.modelType || 'ONNX';

		return {
			id: uri,
			name: nameWithVersion,
			version: version,
			description: description,
			longDescription: entity.properties?.readme || entity.properties?.longDescription,
			deviceSupport: deviceSupport,
			tags: tags,
			publisher: publisher,
			acceleration: acceleration,
			lastModified: entity.lastModifiedDateTime || entity.properties?.lastModified || new Date().toISOString(),
			createdDate: entity.createdDateTime || entity.properties?.created || new Date().toISOString(),
			downloadCount: entity.properties?.downloadCount || 0,
			framework: modelType,
			license: license || licenseDescription,
			taskType: taskType,
			modelSize: this.formatModelSize(fileSizeBytes),
			inputFormat: entity.properties?.inputFormat,
			outputFormat: entity.properties?.outputFormat,
			sampleCode: entity.properties?.sampleCode,
			documentation: entity.properties?.documentation || entity.properties?.readme,
			githubUrl: entity.properties?.githubUrl,
			paperUrl: entity.properties?.paperUrl,
			demoUrl: entity.properties?.demoUrl,
			benchmarks: this.extractBenchmarks(entity.properties?.benchmarks),
			requirements: entity.properties?.requirements || [],
			compatibleVersions: entity.properties?.compatibleVersions || [],
			// Azure Foundry specific fields
			uri: uri,
			parentModelUri: parentModelUri,
			executionProvider: executionProvider,
			device: device,
			fileSizeBytes: fileSizeBytes,
			vRamFootprintBytes: vRamFootprintBytes,
			promptTemplate: promptTemplate,
			supportsToolCalling: supportsToolCalling,
			alias: alias,
			isTestModel: isTestModel,
			minFLVersion: minFLVersion,
			displayName: displayName,
			maxOutputTokens: maxOutputTokens
		};
	}

	private extractBenchmarks(
		benchmarkData: unknown
	): Array<{ metric: string; value: string; device: string }> {
		if (!benchmarkData) return [];

		if (Array.isArray(benchmarkData)) {
			return benchmarkData.map((b: Record<string, unknown>) => ({
				metric: (b.metric as string) || (b.name as string) || 'Unknown',
				value: (b.value as number | string)?.toString() || 'N/A',
				device: (b.device as string) || 'npu'
			}));
		}

		// Handle object format
		if (typeof benchmarkData === 'object') {
			const benchmarks: Array<{ metric: string; value: string; device: string }> = [];
			const data = benchmarkData as Record<string, unknown>;
			for (const metric in data) {
				if (Object.prototype.hasOwnProperty.call(data, metric)) {
					benchmarks.push({
						metric,
						value: (data[metric] as number | string)?.toString() || 'N/A',
						device: 'npu'
					});
				}
			}
			return benchmarks;
		}

		return [];
	}

	private formatModelSize(size: unknown): string {
		if (!size) return 'Unknown';

		if (typeof size === 'number') {
			// Convert bytes to GB (always display in GB for consistency)
			const sizeInGB = size / (1024 * 1024 * 1024);

			// Round to 1 decimal place
			const rounded = Math.round(sizeInGB * 10) / 10;

			return `${rounded} GB`;
		}

		return size.toString();
	}

	// Public method to format file size for display (e.g., in badges)
	formatFileSize(sizeInBytes: number | undefined): string {
		return this.formatModelSize(sizeInBytes);
	}

	private applyClientSideProcessing(
		models: FoundryModel[],
		filters: ApiFilters,
		sortOptions: ApiSortOptions
	): FoundryModel[] {
		let filteredModels = [...models];

		// Apply device filter
		if (filters.device) {
			const deviceFilter = filters.device;
			filteredModels = filteredModels.filter((model) => model.deviceSupport.includes(deviceFilter));
		}

		// Apply family filter (searches in model name/alias)
		if (filters.family) {
			const familyLower = filters.family.toLowerCase();
			filteredModels = filteredModels.filter((model) => {
				const nameMatch = model.name.toLowerCase().includes(familyLower);
				return nameMatch;
			});
		}

		// Apply search filter
		if (filters.searchTerm) {
			const searchLower = filters.searchTerm.toLowerCase();
			filteredModels = filteredModels.filter((model) => {
				const nameMatch = model.name.toLowerCase().indexOf(searchLower) >= 0;
				const descMatch = model.description.toLowerCase().indexOf(searchLower) >= 0;
				const tagMatch = model.tags.some(
					(tag: string) => tag.toLowerCase().indexOf(searchLower) >= 0
				);
				const publisherMatch = model.publisher.toLowerCase().indexOf(searchLower) >= 0;
				return nameMatch || descMatch || tagMatch || publisherMatch;
			});
		}

		// Apply acceleration filter
		if (filters.acceleration) {
			filteredModels = filteredModels.filter(
				(model) => model.acceleration === filters.acceleration
			);
		}

		// Sort models
		filteredModels.sort((a, b) => {
			const aVal: unknown = a[sortOptions.sortBy as keyof FoundryModel];
			const bVal: unknown = b[sortOptions.sortBy as keyof FoundryModel];

			// Handle special sorting cases
			if (sortOptions.sortBy === 'lastModified' || sortOptions.sortBy === 'createdDate') {
				const aDate = new Date(aVal as string | number | Date);
				const bDate = new Date(bVal as string | number | Date);
				if (sortOptions.sortOrder === 'asc') {
					return aDate.getTime() - bDate.getTime();
				} else {
					return bDate.getTime() - aDate.getTime();
				}
			} else if (sortOptions.sortBy === 'downloadCount' || sortOptions.sortBy === 'fileSizeBytes') {
				const aNum = Number(aVal) || 0;
				const bNum = Number(bVal) || 0;
				if (sortOptions.sortOrder === 'asc') {
					return aNum - bNum;
				} else {
					return bNum - aNum;
				}
			} else {
				// String comparison
				const aStr = String(aVal || '').toLowerCase();
				const bStr = String(bVal || '').toLowerCase();

				if (sortOptions.sortOrder === 'asc') {
					return aStr.localeCompare(bStr);
				} else {
					return bStr.localeCompare(aStr);
				}
			}
		});

		return filteredModels;
	}

	// Suffix patterns for extracting alias (sorted by length for proper matching)
	private static readonly ALIAS_SUFFIX_PATTERNS: string[] = [
		// Acceleration + device combinations (longest first)
		'-tensorrt-rtx-gpu',
		'-openvino-npu', '-openvino-cpu', '-openvino-gpu',
		'-vitis-gpu', '-vitis-cpu', '-vitis-npu',
		'-trt-rtx-gpu', '-tensorrt-gpu', '-webgpu-gpu', '-trtrtx-gpu',
		'-generic-gpu', '-generic-cpu', '-cuda-gpu',
		'-qnn-npu',
		// Device-only suffixes
		'-cuda', '-gpu', '-cpu', '-npu', '-fpga', '-asic',
		// Acceleration-only suffixes
		'-qnn', '-vitis', '-vitisai', '-openvino', '-trt-rtx', '-trtrtx', '-tensorrt', '-webgpu'
	].sort((a, b) => b.length - a.length);

	// Helper function to extract alias from model name
	private extractAlias(modelName: string): string {
		// Remove version suffix if present (e.g., "model-name:1" -> "model-name")
		let alias = modelName.toLowerCase();
		const colonIndex = alias.lastIndexOf(':');
		if (colonIndex > 0) {
			alias = alias.substring(0, colonIndex);
		}

		// Remove device-specific and acceleration-specific suffixes
		for (const suffix of FoundryModelService.ALIAS_SUFFIX_PATTERNS) {
			if (alias.endsWith(suffix)) {
				alias = alias.slice(0, -suffix.length);
				break;
			}
		}

		// For whisper models, keep the size variant (e.g., openai-whisper-large-v3)
		// This ensures each size gets its own card while device variants are grouped
		// No special handling needed - the suffix removal above handles device grouping

		return alias;
	}

	// Helper function to create display name from alias
	private createDisplayName(alias: string): string {
		// For whisper models, create a clean display name like "Whisper Large V3"
		if (alias.startsWith('openai-whisper-')) {
			const sizeVariant = alias.replace('openai-whisper-', '');
			return 'Whisper ' + sizeVariant
				.split('-')
				.map((word) => word.charAt(0).toUpperCase() + word.slice(1))
				.join(' ');
		}
		
		// Convert kebab-case to more readable format
		return alias
			.split('-')
			.map((word) => word.charAt(0).toUpperCase() + word.slice(1))
			.join(' ');
	}

	// Simple hash function to create a short unique identifier
	private hashString(str: string): string {
		let hash = 0;
		for (let i = 0; i < str.length; i++) {
			const char = str.charCodeAt(i);
			hash = ((hash << 5) - hash) + char;
			hash = hash & hash; // Convert to 32-bit integer
		}
		return Math.abs(hash).toString(36);
	}

	// Group models by alias and return grouped models
	async fetchGroupedModels(
		filters: ApiFilters = {},
		sortOptions: ApiSortOptions = { sortBy: 'lastModified', sortOrder: 'desc' }
	): Promise<GroupedFoundryModel[]> {
		// First get all individual models
		const allModels = await this.fetchModels(filters, sortOptions);

		// Group models by extracted alias to ensure all variants (CPU, CUDA, etc.) are combined
		// This ensures models like gpt-oss-20b with different device variants show as one card
		const modelGroups = new Map<string, FoundryModel[]>();

		for (const model of allModels) {
			// Always use extracted alias as the group key
			// This ensures qwen2.5-coder-0.5b and qwen2.5-coder-1.5b are NOT grouped together
			// but gpt-oss-20b-cuda-gpu and gpt-oss-20b-generic-cpu ARE grouped together
			const extractedAlias = this.extractAlias(model.name);
			const groupKey = `alias:${extractedAlias}`;

			if (!modelGroups.has(groupKey)) {
				modelGroups.set(groupKey, []);
			}
			const group = modelGroups.get(groupKey);
			if (group) {
				group.push(model);
			}
		}

		// Convert groups to GroupedFoundryModel objects
		const groupedModels: GroupedFoundryModel[] = [];

		for (const [groupKey, variants] of modelGroups) {
			const groupId = groupKey;
			// Deduplicate variants by device+acceleration+executionProvider combination
			// Keep the highest version for each unique combination
			const variantMap = new Map<string, FoundryModel>();

			for (const variant of variants) {
				const device = variant.device || variant.deviceSupport[0] || 'unknown';
				const acceleration = variant.acceleration || 'none';
				const execProvider = variant.executionProvider || 'none';
				const dedupeKey = `${device}-${acceleration}-${execProvider}`;

				const existing = variantMap.get(dedupeKey);
				if (!existing || variant.version > existing.version) {
					variantMap.set(dedupeKey, variant);
				}
			}

			// Use deduplicated variants
			const uniqueVariants = Array.from(variantMap.values());

			// Sort variants to get the primary one (usually the first alphabetically)
			uniqueVariants.sort((a, b) => a.name.localeCompare(b.name));
			const primaryModel = uniqueVariants[0];

			// Combine device support from all variants
			const deviceSupport = [...new Set(uniqueVariants.flatMap((v) => v.deviceSupport))].sort();

			// Combine tags from all variants
			const tags = [...new Set(uniqueVariants.flatMap((v) => v.tags))].sort();

			// Sum download counts
			const totalDownloads = uniqueVariants.reduce((sum, v) => sum + (v.downloadCount || 0), 0);

			// Get latest modification date
			const latestModified = uniqueVariants.reduce((latest, v) => {
				const vDate = new Date(v.lastModified);
				const latestDate = new Date(latest);
				return vDate > latestDate ? v.lastModified : latest;
			}, uniqueVariants[0].lastModified);

			// Get earliest creation date
			const earliestCreated = uniqueVariants.reduce((earliest, v) => {
				const vDate = new Date(v.createdDate);
				const earliestDate = new Date(earliest);
				return vDate < earliestDate ? v.createdDate : earliest;
			}, uniqueVariants[0].createdDate);

			// Get latest version
			const latestVersion = uniqueVariants.reduce((latest, v) => {
				// Simple version comparison - you might want to improve this
				return v.version > latest ? v.version : latest;
			}, uniqueVariants[0].version);

			// Get all accelerations from variants (prefer the first one found for the group)
			const accelerations = uniqueVariants.map((v) => v.acceleration).filter((a): a is string => !!a);
			const groupAcceleration =
				accelerations.length > 0 ? accelerations[0] : primaryModel.acceleration;

			// Get the maximum file size from all variants (most relevant for users)
			const fileSizes = uniqueVariants.map((v) => v.fileSizeBytes || 0).filter((s) => s > 0);
			const maxFileSizeBytes = fileSizes.length > 0 ? Math.max(...fileSizes) : 0;
			// For alias, use the original alias for display and user-facing purposes
			const extractedAlias = this.extractAlias(primaryModel.name);
			const alias = primaryModel.alias || extractedAlias;
			// Use displayName from primaryModel, fallback to createDisplayName
			const displayName = primaryModel.displayName || this.createDisplayName(alias);

			const groupedModel: GroupedFoundryModel = {
				id: groupId,
				alias: alias,
				displayName: displayName,
				description: primaryModel.description,
				longDescription: primaryModel.longDescription,
				deviceSupport,
				tags,
				publisher: primaryModel.publisher,
				acceleration: groupAcceleration,
				lastModified: latestModified,
				createdDate: earliestCreated,
				downloadCount: totalDownloads,
				framework: primaryModel.framework,
				license: primaryModel.license,
				taskType: primaryModel.taskType,
				modelSize: primaryModel.modelSize,
				fileSizeBytes: maxFileSizeBytes,
				variants: uniqueVariants,
				availableDevices: deviceSupport,
				totalDownloads,
				latestVersion,
				documentation: primaryModel.documentation,
				githubUrl: primaryModel.githubUrl,
				paperUrl: primaryModel.paperUrl,
				demoUrl: primaryModel.demoUrl,
				benchmarks: primaryModel.benchmarks,
				requirements: primaryModel.requirements,
				compatibleVersions: primaryModel.compatibleVersions
			};

			groupedModels.push(groupedModel);
		}

		// Apply sorting to grouped models
		groupedModels.sort((a, b) => {
			let aVal: string | number = '';
			let bVal: string | number = '';

			// Type-safe property access
			switch (sortOptions.sortBy) {
				case 'alias':
					aVal = a.alias;
					bVal = b.alias;
					break;
				case 'displayName':
					aVal = a.displayName;
					bVal = b.displayName;
					break;
				case 'description':
					aVal = a.description;
					bVal = b.description;
					break;
				case 'publisher':
					aVal = a.publisher;
					bVal = b.publisher;
					break;
				case 'totalDownloads':
				case 'downloadCount':
					aVal = a.totalDownloads || 0;
					bVal = b.totalDownloads || 0;
					break;
				case 'fileSizeBytes':
					aVal = a.fileSizeBytes || 0;
					bVal = b.fileSizeBytes || 0;
					break;
				case 'lastModified':
					aVal = a.lastModified;
					bVal = b.lastModified;
					break;
				case 'createdDate':
					aVal = a.createdDate;
					bVal = b.createdDate;
					break;
				default:
					aVal = a.displayName;
					bVal = b.displayName;
			}

			if (typeof aVal === 'number' && typeof bVal === 'number') {
				return sortOptions.sortOrder === 'asc' ? aVal - bVal : bVal - aVal;
			} else {
				const aStr = String(aVal).toLowerCase();
				const bStr = String(bVal).toLowerCase();
				return sortOptions.sortOrder === 'asc'
					? aStr.localeCompare(bStr)
					: bStr.localeCompare(aStr);
			}
		});

		return groupedModels;
	}

	async fetchModelById(modelId: string): Promise<FoundryModel | null> {
		try {
			const response = await fetch(`${FOUNDRY_API_ENDPOINT}?id=${encodeURIComponent(modelId)}`);

			if (!response.ok) {
				console.error(`Failed to fetch model ${modelId}: ${response.status}`);
				return null;
			}

			const apiData = await response.json();
			const models = this.transformApiResponse(apiData);

			return models.length > 0 ? models[0] : null;
		} catch (error) {
			console.error(`Error fetching model ${modelId}:`, error);
			return null;
		}
	}
}

export const foundryModelService = new FoundryModelService();
