import { describe, it } from 'mocha';
import { expect } from 'chai';
import { Catalog } from '../src/catalog.js';
import { DeviceType, type ModelInfo } from '../src/types.js';
import { getTestManager, TEST_MODEL_ALIAS } from './testUtils.js';

describe('Catalog Tests', () => {
    it('should initialize with catalog name', function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        expect(catalog.name).to.be.a('string');
        expect(catalog.name.length).to.be.greaterThan(0);
    });

    it('should list models', async function() {
        this.timeout(10000);
        const manager = getTestManager();
        const catalog = manager.catalog;
        const models = await catalog.getModels();
        
        expect(models).to.be.an('array');
        expect(models.length).to.be.greaterThan(0);
        
        // Verify we can find our test model
        const testModel = models.find(m => m.alias === TEST_MODEL_ALIAS);
        expect(testModel).to.not.be.undefined;
    });

    it('should get model by alias', async function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        const model = await catalog.getModel(TEST_MODEL_ALIAS);
        
        expect(model.alias).to.equal(TEST_MODEL_ALIAS);
    });

    it('should throw when getting model with empty, null, or undefined alias', async function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        
        const invalidAliases: any[] = ['', null, undefined];
        for (const invalidAlias of invalidAliases) {
            try {
                await catalog.getModel(invalidAlias);
                expect.fail(`Should have thrown an error for ${invalidAlias === '' ? 'empty' : invalidAlias} alias`);
            } catch (error) {
                expect(error).to.be.instanceOf(Error);
                expect((error as Error).message).to.include('Model alias must be a non-empty string');
            }
        }
    });

    it('should throw when getting model with unknown alias', async function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        
        const unknownAlias = 'definitely-not-a-real-model-alias-12345';
        try {
            await catalog.getModel(unknownAlias);
            expect.fail('Should have thrown an error for unknown alias');
        } catch (error) {
            expect(error).to.be.instanceOf(Error);
            expect((error as Error).message).to.include(`Model with alias '${unknownAlias}' not found`);
            expect((error as Error).message).to.include('Available models:');
        }
    });

    it('should get cached models', async function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        const cachedModels = await catalog.getCachedModels();
        
        expect(cachedModels).to.be.an('array');
        // We expect at least one cached model since we are pointing to a populated cache
        expect(cachedModels.length).to.be.greaterThan(0);
        
        const testModel = cachedModels.find(m => m.alias === TEST_MODEL_ALIAS);
        expect(testModel).to.not.be.undefined;
    });

    it('should throw when getting model variant with empty, null, or undefined ID', async function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        
        const invalidIds: any[] = ['', null, undefined];
        for (const invalidId of invalidIds) {
            try {
                await catalog.getModelVariant(invalidId);
                expect.fail(`Should have thrown an error for ${invalidId === '' ? 'empty' : invalidId} model ID`);
            } catch (error) {
                expect(error).to.be.instanceOf(Error);
                expect((error as Error).message).to.include('Model ID must be a non-empty string');
            }
        }
    });

    it('should throw when getting model variant with unknown ID', async function() {
        const manager = getTestManager();
        const catalog = manager.catalog;
        
        const unknownId = 'definitely-not-a-real-model-id-12345';
        try {
            await catalog.getModelVariant(unknownId);
            expect.fail('Should have thrown an error for unknown model ID');
        } catch (error) {
            expect(error).to.be.instanceOf(Error);
            expect((error as Error).message).to.include(`Model variant with ID '${unknownId}' not found`);
            expect((error as Error).message).to.include('Available variants:');
        }
    });

    it('should resolve latest version for model and variant inputs', async function() {
        // Mirror the C# test by using synthetic model data sorted by version descending.
        const testModelInfos: ModelInfo[] = [
            {
                id: 'test-model:3',
                name: 'test-model',
                version: 3,
                alias: 'test-alias',
                displayName: 'Test Model',
                providerType: 'test',
                uri: 'test://model/3',
                modelType: 'ONNX',
                runtime: { deviceType: DeviceType.CPU, executionProvider: 'CPUExecutionProvider' },
                cached: false,
                createdAtUnix: 1700000003
            },
            {
                id: 'test-model:2',
                name: 'test-model',
                version: 2,
                alias: 'test-alias',
                displayName: 'Test Model',
                providerType: 'test',
                uri: 'test://model/2',
                modelType: 'ONNX',
                runtime: { deviceType: DeviceType.CPU, executionProvider: 'CPUExecutionProvider' },
                cached: false,
                createdAtUnix: 1700000002
            },
            {
                id: 'test-model:1',
                name: 'test-model',
                version: 1,
                alias: 'test-alias',
                displayName: 'Test Model',
                providerType: 'test',
                uri: 'test://model/1',
                modelType: 'ONNX',
                runtime: { deviceType: DeviceType.CPU, executionProvider: 'CPUExecutionProvider' },
                cached: false,
                createdAtUnix: 1700000001
            }
        ];

        const mockCoreInterop = {
            executeCommand(command: string): string {
                if (command === 'get_model_list') {
                    return JSON.stringify(testModelInfos);
                }
                if (command === 'get_cached_models') {
                    return '[]';
                }
                throw new Error(`Unexpected command: ${command}`);
            },
            executeCommandAsync(command: string): Promise<string> {
                return Promise.resolve(this.executeCommand(command));
            }
        } as any;

        const mockLoadManager = {
            listLoaded: async () => []
        } as any;

        const catalog = new Catalog(mockCoreInterop, mockLoadManager, 'TestCatalog');

        const model = await catalog.getModel('test-alias');
        expect(model).to.not.be.undefined;

        const variants = model.variants;
        expect(variants).to.have.length(3);

        const latestVariant = variants[0];
        const middleVariant = variants[1];
        const oldestVariant = variants[2];

        expect(latestVariant.id).to.equal('test-model:3');
        expect(middleVariant.id).to.equal('test-model:2');
        expect(oldestVariant.id).to.equal('test-model:1');

        const result1 = await catalog.getLatestVersion(latestVariant);
        expect(result1.id).to.equal('test-model:3');

        const result2 = await catalog.getLatestVersion(middleVariant);
        expect(result2.id).to.equal('test-model:3');

        const result3 = await catalog.getLatestVersion(oldestVariant);
        expect(result3.id).to.equal('test-model:3');

        model.selectVariant(latestVariant);
        const resultFromModel = await catalog.getLatestVersion(model);
        expect(resultFromModel).to.equal(model);
    });

    it('should self-heal getModel and getModelVariant on cache miss', async function() {
        // Mirrors the Python self-heal test: warm catalog returns no models;
        // the second (forced) refresh from getModel/getModelVariant surfaces
        // the BYOM that was dropped into the cache after the SDK warmed up.
        const byomModelInfos: ModelInfo[] = [
            {
                id: 'byom-self-heal:1',
                name: 'byom-self-heal',
                version: 1,
                alias: 'byom-self-heal',
                displayName: 'BYOM Self Heal',
                providerType: 'local',
                uri: 'local://byom-self-heal/1',
                modelType: 'ONNX',
                runtime: { deviceType: DeviceType.CPU, executionProvider: 'CPUExecutionProvider' },
                cached: true,
                createdAtUnix: 1700000001
            }
        ];

        let modelListCalls = 0;

        const mockCoreInterop = {
            executeCommand(command: string): string {
                if (command === 'get_catalog_name') {
                    return 'TestCatalog';
                }
                throw new Error(`Unexpected sync command: ${command}`);
            },
            executeCommandAsync(command: string): Promise<string> {
                if (command === 'get_model_list') {
                    modelListCalls += 1;
                    return Promise.resolve(modelListCalls > 1 ? JSON.stringify(byomModelInfos) : '[]');
                }
                if (command === 'get_cached_models') {
                    return Promise.resolve('[]');
                }
                return Promise.reject(new Error(`Unexpected async command: ${command}`));
            }
        } as any;

        const mockLoadManager = {
            listLoaded: async () => []
        } as any;

        const catalog = new Catalog(mockCoreInterop, mockLoadManager, 'TestCatalog');

        const model = await catalog.getModel('byom-self-heal');
        expect(model).to.not.be.undefined;
        expect(model.alias).to.equal('byom-self-heal');

        const variant = await catalog.getModelVariant('byom-self-heal:1');
        expect(variant).to.not.be.undefined;
        expect(variant.id).to.equal('byom-self-heal:1');

        // First lookup: warm + forced refresh (2 calls).
        // Second lookup finds the now-populated alias/variant map; the inner
        // TTL-gated updateModels() short-circuits, so no extra fetches.
        expect(modelListCalls).to.equal(2);
    });

    it('should self-heal getCachedModels when core returns an unknown id', async function() {
        // Core always reports the BYOM as cached; the SDK has to self-heal in
        // resolveModelIds (forced updateModels) to learn it exists in the
        // catalog and surface it from getCachedModels.
        const byomModelInfos: ModelInfo[] = [
            {
                id: 'byom-cached:1',
                name: 'byom-cached',
                version: 1,
                alias: 'byom-cached',
                displayName: 'BYOM Cached',
                providerType: 'local',
                uri: 'local://byom-cached/1',
                modelType: 'ONNX',
                runtime: { deviceType: DeviceType.CPU, executionProvider: 'CPUExecutionProvider' },
                cached: true,
                createdAtUnix: 1700000001
            }
        ];

        let modelListCalls = 0;

        const mockCoreInterop = {
            executeCommand(command: string): string {
                if (command === 'get_catalog_name') {
                    return 'TestCatalog';
                }
                throw new Error(`Unexpected sync command: ${command}`);
            },
            executeCommandAsync(command: string): Promise<string> {
                if (command === 'get_model_list') {
                    modelListCalls += 1;
                    return Promise.resolve(modelListCalls > 1 ? JSON.stringify(byomModelInfos) : '[]');
                }
                if (command === 'get_cached_models') {
                    return Promise.resolve('["byom-cached:1"]');
                }
                return Promise.reject(new Error(`Unexpected async command: ${command}`));
            }
        } as any;

        const mockLoadManager = {
            listLoaded: async () => []
        } as any;

        const catalog = new Catalog(mockCoreInterop, mockLoadManager, 'TestCatalog');

        const cached = await catalog.getCachedModels();
        expect(cached).to.have.length(1);
        expect(cached[0].id).to.equal('byom-cached:1');
        expect(modelListCalls).to.equal(2);
    });

    describe('Incremental refresh', () => {
        // Catalog.updateModels incremental-refresh behavior. The refresh path
        // is shared by all Catalog methods. These tests pin down the contract
        // that externally-held IModel references and per-Model variant
        // selection survive across forced refreshes when the underlying model
        // id is still present in the fresh catalog. They guard against
        // regressing to the clear-and-rebuild pattern that churned wrapper
        // identity on every refresh.

        const makeInfo = (
            id: string, alias: string, cached: boolean, contextLength: number = 1024
        ): ModelInfo => ({
            id,
            name: alias,
            version: Number(id.split(':')[1]),
            alias,
            displayName: alias,
            providerType: 'local',
            uri: `local://${alias}/${id.split(':')[1]}`,
            modelType: 'ONNX',
            runtime: { deviceType: DeviceType.CPU, executionProvider: 'CPUExecutionProvider' },
            cached,
            createdAtUnix: 1700000001,
            contextLength
        });

        const makeCatalog = (states: ModelInfo[][]) => {
            const state = { call: 0 };
            const mockCoreInterop = {
                executeCommand(command: string): string {
                    if (command === 'get_catalog_name') {
                        return 'TestCatalog';
                    }
                    throw new Error(`Unexpected sync command: ${command}`);
                },
                executeCommandAsync(command: string): Promise<string> {
                    if (command === 'get_model_list') {
                        const idx = Math.min(state.call, states.length - 1);
                        state.call += 1;
                        return Promise.resolve(JSON.stringify(states[idx]));
                    }
                    if (command === 'get_cached_models') {
                        return Promise.resolve('[]');
                    }
                    return Promise.reject(new Error(`Unexpected async command: ${command}`));
                }
            } as any;
            const mockLoadManager = { listLoaded: async () => [] } as any;
            return new Catalog(mockCoreInterop, mockLoadManager, 'TestCatalog');
        };

        // Reach into private members via `as any` so the tests can probe the
        // internal maps without triggering the public self-heal paths.
        const internals = (c: Catalog) => c as any;

        it('should preserve identity and propagate info on refresh', async function() {
            // A held IModel / ModelVariant reference must survive a forced
            // refresh when the id is still present (identity preserved) AND
            // surface fresh ModelInfo through the same reference (in-place
            // info refresh, not replace-and-rebuild). Mutating info is the
            // natural way to prove the post-refresh wrapper IS the pre-refresh
            // wrapper, not a fresh one that compares equal.
            const firstState = [makeInfo('a:1', 'alpha', false, 1024)];
            const secondState = [makeInfo('a:1', 'alpha', true, 2048)];
            const catalog = makeCatalog([firstState, secondState]);

            const firstModel = await catalog.getModel('alpha');
            const firstVariant = await catalog.getModelVariant('a:1');
            expect(firstVariant.info.cached).to.equal(false);
            expect(firstVariant.info.contextLength).to.equal(1024);

            await internals(catalog).updateModels(true);

            const secondModel = await catalog.getModel('alpha');
            const secondVariant = await catalog.getModelVariant('a:1');
            expect(firstModel === secondModel).to.equal(true);
            expect(firstVariant === secondVariant).to.equal(true);
            // Same held reference now reflects the fresh ModelInfo.
            expect(firstVariant.info.cached).to.equal(true);
            expect(firstVariant.info.contextLength).to.equal(2048);
        });

        it('selection survives refresh', async function() {
            // When the selected variant's id is still present after a
            // refresh, selectVariant() survives so subsequent ops target the
            // user's pick. Wrapper identity is preserved by
            // Catalog.fetchAndPopulateModels reusing the same ModelVariant
            // for surviving ids, so _refreshVariants does not need to touch
            // selectedVariant.
            const v1 = makeInfo('multi:1', 'multi', true);
            const v2 = makeInfo('multi:2', 'multi', true);
            const both = [v1, v2];
            const catalog = makeCatalog([both, both]);

            const model = await catalog.getModel('multi');
            const variantV2 = model.variants.find(v => v.id === 'multi:2')!;
            model.selectVariant(variantV2);
            expect(model.id).to.equal('multi:2');

            await internals(catalog).updateModels(true);
            expect(model.id).to.equal('multi:2');
            expect(model.variants.length).to.equal(2);
        });

        it('should apply adds and removes on refresh', async function() {
            // Ids no longer present must be evicted from both
            // modelIdToModelVariant and modelAliasToModel; new ids must be
            // inserted as fresh wrappers. Both directions are symmetric and
            // share setup, so we cover them in one test against a single
            // refresh that does both at once. We probe the internal maps
            // directly to avoid triggering the public self-heal paths.
            const firstState = [
                makeInfo('a:1', 'alpha', true),
                makeInfo('b:1', 'beta', true)
            ];
            const secondState = [
                makeInfo('a:1', 'alpha', true),
                makeInfo('byom:1', 'byom-new', true)
            ];
            const catalog = makeCatalog([firstState, secondState]);

            // Warm the catalog and confirm the pre-state.
            await catalog.getModels();
            expect(internals(catalog).modelAliasToModel.has('alpha')).to.equal(true);
            expect(internals(catalog).modelAliasToModel.has('beta')).to.equal(true);
            expect(internals(catalog).modelAliasToModel.has('byom-new')).to.equal(false);

            await internals(catalog).updateModels(true);

            expect(internals(catalog).modelAliasToModel.has('alpha')).to.equal(true);
            expect(internals(catalog).modelAliasToModel.has('beta')).to.equal(false);
            expect(internals(catalog).modelIdToModelVariant.has('b:1')).to.equal(false);
            expect(internals(catalog).modelAliasToModel.has('byom-new')).to.equal(true);
            expect(internals(catalog).modelIdToModelVariant.has('byom:1')).to.equal(true);
        });

        it('should not swallow a forced refresh that arrives during a non-forced refresh', async function() {
            // The Catalog can have a non-forced updateModels() in flight when
            // a different caller (e.g. _resolveModelIds self-heal) asks for
            // force=true. Previously updateModels(true) returned the in-flight
            // promise, dropping the force on the floor. We now chain a fresh
            // refresh so the force is honored. JS-only — Python serializes
            // _update_models under a single lock, C# does the same.
            const firstState = [makeInfo('a:1', 'alpha', true)];
            const secondState = [
                makeInfo('a:1', 'alpha', true),
                makeInfo('byom:1', 'byom-new', true)
            ];
            const state = { call: 0, gateRelease: undefined as (() => void) | undefined };
            const gate = new Promise<void>((res) => { state.gateRelease = res; });

            const mockCoreInterop = {
                executeCommand(command: string): string {
                    if (command === 'get_catalog_name') return 'TestCatalog';
                    throw new Error(`Unexpected sync command: ${command}`);
                },
                executeCommandAsync(command: string): Promise<string> {
                    if (command === 'get_model_list') {
                        const idx = Math.min(state.call, 1);
                        state.call += 1;
                        const data = idx === 0 ? firstState : secondState;
                        if (idx === 0) {
                            return gate.then(() => JSON.stringify(data));
                        }
                        return Promise.resolve(JSON.stringify(data));
                    }
                    if (command === 'get_cached_models') return Promise.resolve('[]');
                    return Promise.reject(new Error(`Unexpected async command: ${command}`));
                }
            } as any;
            const mockLoadManager = { listLoaded: async () => [] } as any;
            const catalog = new Catalog(mockCoreInterop, mockLoadManager, 'TestCatalog');

            const inFlight = internals(catalog).updateModels(false);
            const forced = internals(catalog).updateModels(true);
            state.gateRelease!();
            await Promise.all([inFlight, forced]);

            // Two fetches must have happened — the first gated one, plus the
            // chained forced one. If the force were swallowed, state.call
            // would be 1 and the byom would not be visible.
            expect(state.call).to.equal(2);
            expect(internals(catalog).modelIdToModelVariant.has('byom:1')).to.equal(true);
        });
    });
});
