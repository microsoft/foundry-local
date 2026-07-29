import { ModelVariant } from './modelVariant.js';
import { ChatClient } from '../openai/chatClient.js';
import { AudioClient } from '../openai/audioClient.js';
import { EmbeddingClient } from '../openai/embeddingClient.js';
import { ResponsesClient } from '../openai/responsesClient.js';
import { LiveAudioTranscriptionSession } from '../openai/liveAudioSession.js';
import { IModel } from '../imodel.js';
import { ModelInfo } from '../types.js';

/**
 * Represents a high-level AI model that may have multiple variants (e.g., quantized versions, different formats).
 * Manages the selection and interaction with a specific model variant.
 */
export class Model implements IModel {
    private _alias: string;

    private _variants: ModelVariant[];
    private selectedVariant: ModelVariant;

    constructor(variant: ModelVariant) {
        this._alias = variant.alias;
        this._variants = [variant];
        this.selectedVariant = variant;
    }

    /**
     * Adds a new variant to this model.
     * Automatically selects the new variant if it is cached and the current one is not.
     * @param variant - The model variant to add.
     * @throws Error - If the variant's alias does not match the model's alias.
     * @internal
     */
    public addVariant(variant: ModelVariant): void {
        if (!variant || variant.alias !== this._alias) {
            throw new Error(`Variant alias "${variant?.alias}" does not match model alias "${this._alias}".`);
        }
        this._variants.push(variant);

        // Prefer the highest priority locally cached variant.
        if (variant.info.cached && !this.selectedVariant.info.cached) {
            this.selectedVariant = variant;
        }
    }

    /**
     * Replace the variant list in place while preserving wrapper identity.
     *
     * Called by `Catalog.fetchAndPopulateModels` during incremental refresh
     * so a user's held `Model` reference keeps pointing at the same object
     * across refreshes. Because `Catalog.fetchAndPopulateModels` reuses the
     * same `ModelVariant` wrappers for ids that survive a refresh, any
     * explicit `selectVariant()` choice that survives the refresh is
     * preserved without any extra work here.
     *
     * TODO: tighten the held-reference contract for the case where the
     * previously selected variant is removed by a refresh; today
     * `selectedVariant` keeps pointing at the dropped wrapper and callers
     * must explicitly re-select.
     *
     * @internal
     */
    public _refreshVariants(variants: ModelVariant[]): void {
        if (!variants || variants.length === 0) {
            throw new Error(`Cannot refresh model ${this._alias} with an empty variant list`);
        }

        this._variants = variants.slice();
    }

    /**
     * Selects a specific variant.
     * @param variant - The model variant to select. Must be one of the variants in `variants`.
     * @throws Error - If the variant does not belong to this model.
     */
    public selectVariant(variant: IModel): void {
        const matchingVariant = this._variants.find(v => v.id === variant.id);
        if (!variant.id || !matchingVariant) {
            throw new Error(`Input variant was not found in Variants.`);
        }
        this.selectedVariant = matchingVariant;
    }

    /**
     * Gets the ID of the currently selected variant.
     * @returns The ID of the selected variant.
     */
    public get id(): string {
        return this.selectedVariant.id;
    }

    /**
     * Gets the alias of the model.
     * @returns The model alias.
     */
    public get alias(): string {
        return this._alias;
    }

    /**
     * Gets the ModelInfo of the currently selected variant.
     * @returns The ModelInfo object.
     */
    public get info(): ModelInfo {
        return this.selectedVariant.info;
    }

    /**
     * Checks if the currently selected variant is cached locally.
     * @returns True if cached, false otherwise.
     */
    public get isCached(): boolean {
        return this.selectedVariant.isCached;
    }

    /**
     * Checks if the currently selected variant is loaded in memory.
     * @returns True if loaded, false otherwise.
     */
    public async isLoaded(): Promise<boolean> {
        return await this.selectedVariant.isLoaded();
    }

    /**
     * Gets all available variants for this model.
     * @returns An array of IModel objects.
     */
    public get variants(): IModel[] {
        return this._variants;
    }

    public get contextLength(): number | null {
        return this.selectedVariant.contextLength;
    }

    public get inputModalities(): string | null {
        return this.selectedVariant.inputModalities;
    }

    public get outputModalities(): string | null {
        return this.selectedVariant.outputModalities;
    }

    public get capabilities(): string | null {
        return this.selectedVariant.capabilities;
    }

    public get supportsToolCalling(): boolean | null {
        return this.selectedVariant.supportsToolCalling;
    }

    /**
     * Downloads the currently selected variant.
     * @param progressCallbackOrSignal - Optional progress callback or AbortSignal.
     * @param signal - Optional AbortSignal when a progress callback is provided.
     */
    public download(
        progressCallbackOrSignal?: ((progress: number) => void) | AbortSignal,
        signal?: AbortSignal
    ): Promise<void> {
        return this.selectedVariant.download(progressCallbackOrSignal, signal);
    }

    /**
     * Gets the local file path of the currently selected variant.
     * @returns The local file path.
     */
    public get path(): string {
        return this.selectedVariant.path;
    }

    /**
     * Loads the currently selected variant into memory.
     * @returns A promise that resolves when the model is loaded.
     */
    public async load(): Promise<void> {
        await this.selectedVariant.load();
    }

    /**
     * Removes the currently selected variant from the local cache.
     */
    public removeFromCache(): void {
        this.selectedVariant.removeFromCache();
    }

    /**
     * Unloads the currently selected variant from memory.
     * @returns A promise that resolves when the model is unloaded.
     */
    public async unload(): Promise<void> {
        await this.selectedVariant.unload();
    }

    /**
     * Creates a ChatClient for interacting with the model via chat completions.
     * @returns A ChatClient instance.
     */
    public createChatClient(): ChatClient {
        return this.selectedVariant.createChatClient();
    }

    /**
     * Creates an AudioClient for interacting with the model via audio operations.
     * @returns An AudioClient instance.
     */
    public createAudioClient(): AudioClient {
        return this.selectedVariant.createAudioClient();
    }

    /**
     * Creates an EmbeddingClient for generating text embeddings with the model.
     * @returns An EmbeddingClient instance.
     */
    public createEmbeddingClient(): EmbeddingClient {
        return this.selectedVariant.createEmbeddingClient();
    }

    /**
     * Creates a LiveAudioTranscriptionSession for real-time audio streaming ASR.
     * @returns A LiveAudioTranscriptionSession instance.
     */
    public createLiveTranscriptionSession(): LiveAudioTranscriptionSession {
        return this.selectedVariant.createLiveTranscriptionSession();
    }

    /**
     * Creates a ResponsesClient for interacting with the model via the Responses API.
     * @param baseUrl - The base URL of the Foundry Local web service.
     * @returns A ResponsesClient instance.
     */
    public createResponsesClient(baseUrl: string): ResponsesClient {
        return this.selectedVariant.createResponsesClient(baseUrl);
    }
}
