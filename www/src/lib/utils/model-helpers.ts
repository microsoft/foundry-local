// Device icon mapping
const DEVICE_ICONS: Record<string, string> = {
	npu: '🧠',
	gpu: '🎮',
	cpu: '💻'
};

export function getDeviceIcon(device: string): string {
	return DEVICE_ICONS[device.toLowerCase()] || '🔧';
}

// Accelerator logo patterns and paths
const ACCELERATOR_LOGO_PATTERNS: Array<{ patterns: string[]; logo: string }> = [
	{
		patterns: ['-cuda-', '-cuda', '-tensorrt-', '-tensorrt', '-trt-rtx-', '-trt-rtx', '-trtrtx'],
		logo: '/logos/nvidia-logo.svg'
	},
	{ patterns: ['-qnn-', '-qnn'], logo: '/logos/qualcomm-logo.svg' },
	{ patterns: ['-vitis-', '-vitis', '-vitisai'], logo: '/logos/amd-logo.svg' },
	{ patterns: ['-openvino-', '-openvino'], logo: '/logos/intel-logo.svg' },
	{ patterns: ['-webgpu-', '-webgpu', 'webgpu', '-generic-gpu'], logo: '/logos/webgpu-logo.svg' }
];

export function getAcceleratorLogo(variantName: string): string | null {
	const name = variantName.toLowerCase();
	for (const { patterns, logo } of ACCELERATOR_LOGO_PATTERNS) {
		if (patterns.some((pattern) => name.includes(pattern))) {
			return logo;
		}
	}
	return null;
}

// Family detection.
// Priority-ordered keyword list: brand/product families come before base
// architectures so that derivative aliases (e.g. `llama-3.1-nemotron-*`) are
// bucketed by brand rather than by base model. `ministral` must precede
// `mistral` for the same reason.
const FAMILY_KEYWORDS: string[] = [
	// brand / product families first
	'nemotron',
	'gpt-oss',
	'deepseek',
	// base architectures / product families
	'ministral',
	'mistral',
	'phi',
	'qwen',
	'llama',
	'gemma',
	// speech / audio
	'whisper',
	'parakeet',
	// other open families
	'smollm',
	'olmo'
];

// Display-name overrides for families whose canonical casing isn't just
// capitalize-the-first-letter (e.g. `gpt-oss` -> `GPT-OSS`).
const FAMILY_DISPLAY_NAMES: Record<string, string> = {
	'gpt-oss': 'GPT-OSS',
	smollm: 'SmolLM',
	olmo: 'OLMo'
};

// Detect the family for a model given its alias (or fall back to name).
// Returns the matching keyword from FAMILY_KEYWORDS, or undefined if no
// known family keyword appears as a token in the identifier.
export function detectModelFamily(identifier: string | null | undefined): string | undefined {
	if (!identifier) return undefined;
	const s = identifier.toLowerCase();
	for (const k of FAMILY_KEYWORDS) {
		// Token-boundary match: the keyword must start the string or follow a
		// separator, and must NOT be followed by a letter. A trailing digit
		// is treated as a version boundary so `qwen3`, `qwen2.5`, `smollm3`
		// all match. Trailing `-`, `.`, `_`, `/`, or end-of-string also
		// count as a boundary. This blocks false positives like `phillm`
		// matching `phi`.
		const escaped = k.replace(/[-/\\^$*+?.()|[\]{}]/g, '\\$&');
		const re = new RegExp(`(^|[-_/])${escaped}(?![a-z])`);
		if (re.test(s)) return k;
	}
	return undefined;
}

export function getFamilyDisplayName(family: string): string {
	return FAMILY_DISPLAY_NAMES[family] ?? family.charAt(0).toUpperCase() + family.slice(1);
}

// Accelerator color patterns
const ACCELERATOR_COLOR_PATTERNS: Array<{ patterns: string[]; color: string }> = [
	{
		patterns: ['-cuda-', '-cuda', '-tensorrt-', '-tensorrt', '-trt-rtx-', '-trt-rtx', '-trtrtx'],
		color: '#76B900'
	},
	{ patterns: ['-qnn-', '-qnn'], color: '#3253DC' },
	{ patterns: ['-vitis-', '-vitis', '-vitisai'], color: 'var(--amd-color, #000000)' },
	{ patterns: ['-openvino-', '-openvino'], color: '#0071C5' },
	{ patterns: ['-webgpu-', '-webgpu', 'webgpu', '-generic-gpu'], color: '#005A9C' }
];

export function getAcceleratorColor(variantName: string): string {
	const name = variantName.toLowerCase();
	for (const { patterns, color } of ACCELERATOR_COLOR_PATTERNS) {
		if (patterns.some((pattern) => name.includes(pattern))) {
			return color;
		}
	}
	return 'currentColor';
}

// Variant label patterns - ordered by specificity (most specific first)
const VARIANT_LABEL_PATTERNS: Array<{ patterns: string[]; label: string; requiresAll?: boolean }> =
	[
		{ patterns: ['-cuda-', '-trt-rtx-'], label: 'CUDA + TensorRT', requiresAll: true },
		{ patterns: ['-cuda-', '-tensorrt-'], label: 'CUDA + TensorRT', requiresAll: true },
		{ patterns: ['-cuda-', '-trtrtx'], label: 'CUDA + TensorRT', requiresAll: true },
		{ patterns: ['-cuda-gpu', '-cuda-'], label: 'CUDA' },
		{ patterns: ['-generic-gpu', 'webgpu'], label: 'WebGPU' },
		{ patterns: ['-qnn-'], label: 'QNN' },
		{ patterns: ['-vitis-'], label: 'Vitis' },
		{ patterns: ['-openvino-'], label: 'OpenVINO' },
		{ patterns: ['-trt-rtx-', '-tensorrt-', '-trtrtx-', '-trtrtx'], label: 'TensorRT' },
		{ patterns: ['-generic-cpu'], label: 'Generic' }
	];

export function getVariantLabel(variant: { name: string; deviceSupport: string[] }): string {
	const modelName = variant.name.toLowerCase();
	const device = variant.deviceSupport[0]?.toUpperCase() || '';

	for (const { patterns, label, requiresAll } of VARIANT_LABEL_PATTERNS) {
		if (requiresAll) {
			if (patterns.every((pattern) => modelName.includes(pattern))) {
				return `${device} (${label})`;
			}
		} else {
			if (patterns.some((pattern) => modelName.includes(pattern))) {
				return `${device} (${label})`;
			}
		}
	}

	return device;
}

// Acceleration to logo mapping
const ACCELERATION_LOGOS: Record<string, string> = {
	cuda: '/logos/nvidia-logo.svg',
	'trt-rtx': '/logos/nvidia-logo.svg',
	trtrtx: '/logos/nvidia-logo.svg',
	qnn: '/logos/qualcomm-logo.svg',
	vitis: '/logos/amd-logo.svg',
	openvino: '/logos/intel-logo.svg',
	webgpu: '/logos/webgpu-logo.svg'
};

export function getAcceleratorLogoFromAcceleration(acceleration: string): string | null {
	return ACCELERATION_LOGOS[acceleration.toLowerCase()] || null;
}

// Acceleration to color mapping
const ACCELERATION_COLORS: Record<string, string> = {
	cuda: '#76B900',
	'trt-rtx': '#76B900',
	trtrtx: '#76B900',
	qnn: '#3253DC',
	vitis: 'var(--amd-color, #000000)',
	openvino: '#0071C5',
	webgpu: '#005A9C'
};

export function getAcceleratorColorFromAcceleration(acceleration: string): string {
	return ACCELERATION_COLORS[acceleration.toLowerCase()] || 'currentColor';
}
