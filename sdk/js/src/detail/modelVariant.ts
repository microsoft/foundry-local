import { CoreInterop } from './coreInterop.js';
import { ModelLoadManager } from './modelLoadManager.js';
import { ModelInfo } from '../types.js';
import { ChatClient } from '../openai/chatClient.js';
import { AudioClient } from '../openai/audioClient.js';
import { EmbeddingClient } from '../openai/embeddingClient.js';
import { LiveAudioTranscriptionSession } from '../openai/liveAudioSession.js';
import { ResponsesClient } from '../openai/responsesClient.js';
import { IModel } from '../imodel.js';

/**
 * Represents a specific variant of a model (e.g., a specific quantization or format).
 * Contains the low-level implementation for interacting with the model.
 * @internal
 */
export class ModelVariant implements IModel {
    private _modelInfo: ModelInfo;
    private coreInterop: CoreInterop;
    private modelLoadManager: ModelLoadManager;

    constructor(modelInfo: ModelInfo, coreInterop: CoreInterop, modelLoadManager: ModelLoadManager) {
        this._modelInfo = modelInfo;
        this.coreInterop = coreInterop;
        this.modelLoadManager = modelLoadManager;
    }

    /**
     * Replace the cached ModelInfo snapshot in place.
     *
     * Called by `Catalog.fetchAndPopulateModels` during incremental refresh so
     * wrapper identity is preserved across refreshes while still surfacing
     * fresh metadata (notably `cached`) on held references. `id` and `alias`
     * are immutable for a given variant; the caller must only invoke this with
     * a `modelInfo` whose id matches `this.id`. JS is single-threaded so no
     * torn-read concerns.
     *
     * @internal
     */
    public _refreshInfo(modelInfo: ModelInfo): void {
        this._modelInfo = modelInfo;
    }

    /**
     * Gets the unique identifier of the model variant.
     * @returns The model ID.
     */
    public get id(): string {
        return this._modelInfo.id;
    }

    /**
     * Gets the alias of the model.
     * @returns The model alias.
     */
    public get alias(): string {
        return this._modelInfo.alias;
    }

    /**
     * Gets the detailed information about the model variant.
     * @returns The ModelInfo object.
     */
    public get info(): ModelInfo {
        return this._modelInfo;
    }

    /**
     * A ModelVariant is a single variant, so variants returns itself.
     */
    public get variants(): IModel[] {
        return [this];
    }

    /**
     * SelectVariant is not supported on a ModelVariant.
     * Call Catalog.getModel() to get an IModel with all variants available.
     * @throws Error always.
     */
    public selectVariant(_variant: IModel): void {
        throw new Error(
            `selectVariant is not supported on a ModelVariant. ` +
            `Call Catalog.getModel("${this.alias}") to get an IModel with all variants available.`
        );
    }

    public get contextLength(): number | null {
        return this._modelInfo.contextLength ?? null;
    }

    public get inputModalities(): string | null {
        return this._modelInfo.inputModalities ?? null;
    }

    public get outputModalities(): string | null {
        return this._modelInfo.outputModalities ?? null;
    }

    public get capabilities(): string | null {
        return this._modelInfo.capabilities ?? null;
    }

    public get supportsToolCalling(): boolean | null {
        return this._modelInfo.supportsToolCalling ?? null;
    }

    /**
     * Checks if the model variant is cached locally.
     * @returns True if cached, false otherwise.
     */
    public get isCached(): boolean {
        const cachedModels: string[] = JSON.parse(this.coreInterop.executeCommand("get_cached_models"));
        return cachedModels.includes(this._modelInfo.id);
    }

    /**
     * Checks if the model variant is loaded in memory.
     * @returns True if loaded, false otherwise.
     */
    public async isLoaded(): Promise<boolean> {
        const loadedModels = await this.modelLoadManager.listLoaded();
        return loadedModels.includes(this._modelInfo.id);
    }

    /**
     * Downloads the model variant.
     * @param progressCallbackOrSignal - Optional progress callback (0-100) or AbortSignal.
     * @param signal - Optional AbortSignal when a progress callback is provided.
     */
    public async download(
        progressCallbackOrSignal?: ((progress: number) => void) | AbortSignal,
        signal?: AbortSignal
    ): Promise<void> {
        const progressCallback = typeof progressCallbackOrSignal === 'function'
            ? progressCallbackOrSignal
            : undefined;
        const abortSignal = typeof progressCallbackOrSignal === 'function'
            ? signal
            : progressCallbackOrSignal ?? signal;
        const request = { Params: { Model: this._modelInfo.id } };
        if (!progressCallback && !abortSignal) {
            await this.coreInterop.executeCommandAsync("download_model", request);
        } else {
            // Use the streaming path when progress or cancellation is needed.
            // Provide a no-op callback when only cancellation is requested so
            // the native callback mechanism is engaged.
            const cb = progressCallback ?? (() => {});
            await this.coreInterop.executeCommandStreaming("download_model", request, (chunk: string) => {
                const progressChunk = chunk.trim();
                if (progressChunk.length === 0) {
                    return;
                }

                const progress = Number(progressChunk);
                if (!Number.isNaN(progress)) {
                    cb(progress);
                }
            }, abortSignal);
        }
    }

    /**
     * Gets the local file path of the model variant.
     * @returns The local file path.
     */
    public get path(): string {
        const request = { Params: { Model: this._modelInfo.id } };
        return this.coreInterop.executeCommand("get_model_path", request);
    }

    /**
     * Loads the model variant into memory.
     * @returns A promise that resolves when the model is loaded.
     */
    public async load(): Promise<void> {
        await this.modelLoadManager.load(this._modelInfo.id);
    }

    /**
     * Removes the model variant from the local cache.
     */
    public removeFromCache(): void {
        this.coreInterop.executeCommand("remove_cached_model", { Params: { Model: this._modelInfo.id } });
    }

    /**
     * Unloads the model variant from memory.
     * @returns A promise that resolves when the model is unloaded.
     */
    public async unload(): Promise<void> {
        await this.modelLoadManager.unload(this._modelInfo.id);
    }

    /**
     * Creates a ChatClient for interacting with the model via chat completions.
     * @returns A ChatClient instance.
     */
    public createChatClient(): ChatClient {
        return new ChatClient(this._modelInfo.id, this.coreInterop);
    }

    /**
     * Creates an AudioClient for interacting with the model via audio operations.
     * @returns An AudioClient instance.
     */
    public createAudioClient(): AudioClient {
        return new AudioClient(this._modelInfo.id, this.coreInterop);
    }

    /**
     * Creates an EmbeddingClient for generating text embeddings with the model.
     * @returns An EmbeddingClient instance.
     */
    public createEmbeddingClient(): EmbeddingClient {
        return new EmbeddingClient(this._modelInfo.id, this.coreInterop);
    }

    /**
     * Creates a LiveAudioTranscriptionSession for real-time audio streaming ASR.
     * @returns A LiveAudioTranscriptionSession instance.
     */
    public createLiveTranscriptionSession(): LiveAudioTranscriptionSession {
        return new LiveAudioTranscriptionSession(this._modelInfo.id, this.coreInterop);
    }

    /**
     * Creates a ResponsesClient for interacting with the model via the Responses API.
     * @param baseUrl - The base URL of the Foundry Local web service.
     * @returns A ResponsesClient instance.
     */
    public createResponsesClient(baseUrl: string): ResponsesClient {
        return new ResponsesClient(baseUrl, this._modelInfo.id);
    }
}
