import { CoreInterop } from './detail/coreInterop.js';
import { ModelLoadManager } from './detail/modelLoadManager.js';
import { Model } from './detail/model.js';
import { ModelVariant } from './detail/modelVariant.js';
import { ModelInfo } from './types.js';
import { IModel } from './imodel.js';

/**
 * Represents a catalog of AI models available in the system.
 * Provides methods to discover, list, and retrieve models and their variants.
 */
export class Catalog {
    private _name: string;
    private coreInterop: CoreInterop;
    private modelLoadManager: ModelLoadManager;
    private _models: Model[] = [];
    private modelAliasToModel: Map<string, Model> = new Map();
    private modelIdToModelVariant: Map<string, ModelVariant> = new Map();
    private lastFetch: number = 0;
    private updatePromise?: Promise<void>;

    constructor(coreInterop: CoreInterop, modelLoadManager: ModelLoadManager, catalogName?: string) {
        this.coreInterop = coreInterop;
        this.modelLoadManager = modelLoadManager;
        this._name = catalogName ?? this.coreInterop.executeCommand("get_catalog_name");
    }

    /**
     * Gets the name of the catalog.
     * @returns The name of the catalog.
     */
    public get name(): string {
        return this._name;
    }

    /** @internal */
    invalidateCache(): void {
        this.lastFetch = 0;
    }

    private async updateModels(force: boolean = false): Promise<void> {
        // TODO: make this configurable
        // Skip if the cache is still fresh, unless the caller forced a refresh
        // (e.g. self-heal after a cache miss caused by a manually-added BYOM
        // model dropped into the cache directory).
        if (!force && (Date.now() - this.lastFetch) < 6 * 60 * 60 * 1000) { // 6 hours
            return;
        }
        if (this.updatePromise) {
            // If a non-forced refresh is in flight and the caller asked for a
            // forced refresh (e.g. BYOM self-heal), the in-flight result may
            // pre-date the change that prompted the force. Chain a fresh
            // refresh after the current one so the force is not silently
            // dropped — the inner TTL re-check short-circuits if the chained
            // refresh started after our deadline.
            if (force) {
                const chained = this.updatePromise
                    .catch(() => undefined)
                    .then(() => this.runRefreshExclusive());
                return chained;
            }
            return this.updatePromise;
        }
        return this.runRefreshExclusive();
    }

    private runRefreshExclusive(): Promise<void> {
        this.updatePromise = this.fetchAndPopulateModels()
            .finally(() => { this.updatePromise = undefined; });
        return this.updatePromise;
    }

    private async fetchAndPopulateModels(): Promise<void> {
        const modelListJson = await this.coreInterop.executeCommandAsync("get_model_list");
        let modelsInfo: ModelInfo[] = [];
        try {
            modelsInfo = JSON.parse(modelListJson);
        } catch (error) {
            throw new Error(`Failed to parse model list JSON: ${error}`);
        }

        // Incremental refresh: preserve wrapper identity for ids/aliases that
        // survive the refresh so externally-held IModel references keep
        // working with up-to-date metadata and (for Model) keep any explicit
        // selectVariant() choice. New ids get fresh wrappers; removed ids get
        // evicted.

        const freshIds = new Set<string>();
        const freshAliasGroups = new Map<string, ModelInfo[]>();
        for (const info of modelsInfo) {
            freshIds.add(info.id);
            const group = freshAliasGroups.get(info.alias);
            if (group) {
                group.push(info);
            } else {
                freshAliasGroups.set(info.alias, [info]);
            }
        }

        for (const staleId of [...this.modelIdToModelVariant.keys()]) {
            if (!freshIds.has(staleId)) {
                this.modelIdToModelVariant.delete(staleId);
            }
        }
        for (const staleAlias of [...this.modelAliasToModel.keys()]) {
            if (!freshAliasGroups.has(staleAlias)) {
                this.modelAliasToModel.delete(staleAlias);
            }
        }

        for (const info of modelsInfo) {
            const existing = this.modelIdToModelVariant.get(info.id);
            if (existing) {
                existing._refreshInfo(info);
            } else {
                this.modelIdToModelVariant.set(
                    info.id,
                    new ModelVariant(info, this.coreInterop, this.modelLoadManager)
                );
            }
        }

        const refreshedAliasOrder: Model[] = [];
        for (const [alias, infos] of freshAliasGroups) {
            const variants = infos.map(i => this.modelIdToModelVariant.get(i.id)!);
            const existingModel = this.modelAliasToModel.get(alias);
            if (existingModel) {
                existingModel._refreshVariants(variants);
                refreshedAliasOrder.push(existingModel);
            } else {
                const m = new Model(variants[0]);
                for (let i = 1; i < variants.length; i++) {
                    m.addVariant(variants[i]);
                }
                this.modelAliasToModel.set(alias, m);
                refreshedAliasOrder.push(m);
            }
        }

        this._models = refreshedAliasOrder;
        this.lastFetch = Date.now();
    }

    /**
     * Lists all available models in the catalog.
     * This method is asynchronous as it may fetch the model list from a remote service or perform file I/O.
     * @returns A Promise that resolves to an array of IModel objects.
     */
    public async getModels(): Promise<IModel[]> {
        await this.updateModels();
        return this._models;
    }

    /**
     * Retrieves a model by its alias.
     * This method is asynchronous as it may ensure the catalog is up-to-date by fetching from a remote service.
     * @param alias - The alias of the model to retrieve.
     * @returns A Promise that resolves to the IModel object if found, otherwise throws an error.
     * @throws Error - If alias is null, undefined, or empty.
     */
    public async getModel(alias: string): Promise<IModel> {
        if (typeof alias !== 'string' || alias.trim() === '') {
            throw new Error('Model alias must be a non-empty string.');
        }
        await this.updateModels();
        let model = this.modelAliasToModel.get(alias);
        if (!model) {
            // Self-heal: the alias may belong to a BYOM model added to the
            // cache directory after our last catalog refresh.
            await this.updateModels(true);
            model = this.modelAliasToModel.get(alias);
        }
        if (!model) {
            const availableAliases = Array.from(this.modelAliasToModel.keys()).join(', ');
            throw new Error(`Model with alias '${alias}' not found. Available models: ${availableAliases || '(none)'}`);
        }
        return model;
    }

    /**
     * Retrieves a specific model variant by its ID.
     * NOTE: This will return an IModel with a single variant. Use getModel to get an IModel with all available
     * variants.
     * This method is asynchronous as it may ensure the catalog is up-to-date by fetching from a remote service.
     * @param modelId - The unique identifier of the model variant.
     * @returns A Promise that resolves to the IModel object if found, otherwise throws an error.
     * @throws Error - If modelId is null, undefined, or empty.
     */
    public async getModelVariant(modelId: string): Promise<IModel> {
        if (typeof modelId !== 'string' || modelId.trim() === '') {
            throw new Error('Model ID must be a non-empty string.');
        }
        await this.updateModels();
        let variant = this.modelIdToModelVariant.get(modelId);
        if (!variant) {
            // Self-heal: the id may belong to a BYOM model added to the cache
            // directory after our last catalog refresh.
            await this.updateModels(true);
            variant = this.modelIdToModelVariant.get(modelId);
        }
        if (!variant) {
            const availableIds = Array.from(this.modelIdToModelVariant.keys()).join(', ');
            throw new Error(`Model variant with ID '${modelId}' not found. Available variants: ${availableIds || '(none)'}`);
        }
        return variant;
    }

    /**
     * Retrieves a list of all locally cached model variants.
     * This method is asynchronous as it may involve file I/O or querying the underlying core.
     * @returns A Promise that resolves to an array of cached IModel objects.
     */
    public async getCachedModels(): Promise<IModel[]> {
        await this.updateModels();
        const cachedModelListJson = await this.coreInterop.executeCommandAsync("get_cached_models");
        let cachedModelIds: string[] = [];
        try {
            cachedModelIds = JSON.parse(cachedModelListJson);
        } catch (error) {
            throw new Error(`Failed to parse cached model list JSON: ${error}`);
        }
        return this.resolveModelIds(cachedModelIds);
    }

    /**
     * Retrieves a list of all currently loaded model variants.
     * This operation is asynchronous because checking the loaded status may involve querying
     * the underlying core or an external service, which can be an I/O bound operation.
     * @returns A Promise that resolves to an array of loaded IModel objects.
     */
    public async getLoadedModels(): Promise<IModel[]> {
        await this.updateModels();
        let loadedModelIds: string[] = [];
        try {
            loadedModelIds = await this.modelLoadManager.listLoaded();
        } catch (error) {
            throw new Error(`Failed to list loaded models: ${error}`);
        }
        return this.resolveModelIds(loadedModelIds);
    }

    /**
     * Resolve a list of model ids against the in-memory catalog, self-healing once
     * if any id is unknown (e.g. a manually-added BYOM model the SDK has not yet seen).
     * Preserves the input order of `modelIds` (minus unknowns), deduplicating variants.
     */
    private async resolveModelIds(modelIds: string[]): Promise<IModel[]> {
        if (modelIds.some(id => !this.modelIdToModelVariant.has(id))) {
            await this.updateModels(true);
        }

        const resolved: IModel[] = [];
        const seen = new Set<IModel>();
        for (const modelId of modelIds) {
            const variant = this.modelIdToModelVariant.get(modelId);
            if (variant && !seen.has(variant)) {
                resolved.push(variant);
                seen.add(variant);
            }
        }
        return resolved;
    }

    /**
     * Get the latest version of a model.
     * This is used to check if a newer version of a model is available in the catalog for download.
     * @param modelOrModelVariant - The model to check for the latest version.
     * @returns The latest version of the model. Will match the input if it is the latest version.
     */
    public async getLatestVersion(modelOrModelVariant: IModel): Promise<IModel> {
        await this.updateModels();

        // Resolve to the parent Model by alias
        const model = this.modelAliasToModel.get(modelOrModelVariant.alias);
        if (!model) {
            throw new Error(`Model with alias '${modelOrModelVariant.alias}' not found in catalog.`);
        }

        // variants are sorted by version, so the first one matching the name is the latest version
        const latest = model.variants.find(v => v.info.name === modelOrModelVariant.info.name);
        if (!latest) {
            throw new Error(
                `Internal error. Mismatch between model (alias:${model.alias}) and ` +
                `model variant (alias:${modelOrModelVariant.alias}).`
            );
        }

        // if input was the latest return the input (could be model or model variant)
        // otherwise return the latest model variant
        return latest.id === modelOrModelVariant.id ? modelOrModelVariant : latest;
    }
}