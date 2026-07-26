import { CoreInterop } from './coreInterop.js';
import packageJson from '../../package.json' with { type: "json" };
const { version } = packageJson;

/**
 * Manages the loading and unloading of models.
 * Handles communication with the core system or an external service (future support).
 */
export class ModelLoadManager {
    private coreInterop: CoreInterop;
    private externalServiceUrl?: string;
    private headers: HeadersInit;

    constructor(coreInterop: CoreInterop, externalServiceUrl?: string) {
        this.coreInterop = coreInterop;
        this.externalServiceUrl = externalServiceUrl;
        this.headers = {
            'User-Agent': `foundry-local-js-sdk/${version}`
        };
    }

    /**
     * Loads a model into memory.
     * @param modelId - The ID of the model to load.
     * @throws Error - If loading via external service fails.
     */
    public async load(modelId: string): Promise<void> {
        if (this.externalServiceUrl) {
            const url = new URL(`models/load/${encodeURIComponent(modelId)}`, this.externalServiceUrl);
            let response: Response;
            try {
                response = await fetch(url.toString(), { headers: this.headers });
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                throw new Error(`Network error occurred while loading model ${modelId} from ${this.externalServiceUrl}: ${message}`);
            }
            if (!response.ok) {
                throw new Error(`Error loading model ${modelId} from ${this.externalServiceUrl}: ${response.status} ${response.statusText}`);
            }
            return;
        }
        await this.coreInterop.executeCommandAsync("load_model", { Params: { Model: modelId } });
    }

    /**
     * Unloads a model from memory.
     * @param modelId - The ID of the model to unload.
     * @throws Error - If unloading via external service fails.
     */
    public async unload(modelId: string): Promise<void> {
        if (this.externalServiceUrl) {
            const url = new URL(`models/unload/${encodeURIComponent(modelId)}`, this.externalServiceUrl);
            const response = await fetch(url.toString(), { headers: this.headers });
            if (!response.ok) {
                throw new Error(`Error unloading model ${modelId} from ${this.externalServiceUrl}: ${response.statusText}`);
            }
            return;
        }
        await this.coreInterop.executeCommandAsync("unload_model", { Params: { Model: modelId } });
    }

    /**
     * Lists the IDs of all currently loaded models.
     * @returns An array of loaded model IDs.
     * @throws Error - If listing via external service fails or if JSON parsing fails.
     */
    public async listLoaded(): Promise<string[]> {
        if (this.externalServiceUrl) {
            const url = new URL('models/loaded', this.externalServiceUrl);
            const response = await fetch(url.toString(), { headers: this.headers });
            if (!response.ok) {
                throw new Error(`Error listing loaded models from ${this.externalServiceUrl}: ${response.statusText}`);
            }
            const list = await response.json();
            return list || [];
        }
        const response = await this.coreInterop.executeCommandAsync("list_loaded_models");
        try {
            return JSON.parse(response);
        } catch (error) {
            throw new Error(`Failed to decode JSON response: ${error}. Response was: ${response}`);
        }
    }
}
