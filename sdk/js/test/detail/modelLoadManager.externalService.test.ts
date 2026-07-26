import { expect } from 'chai';
import { ModelLoadManager } from '../../src/detail/modelLoadManager.js';

describe('ModelLoadManager external service errors', function() {
    let originalFetch: typeof globalThis.fetch;

    beforeEach(function() {
        originalFetch = globalThis.fetch;
    });

    afterEach(function() {
        globalThis.fetch = originalFetch;
    });

    it('should report an HTTP error if the external service rejects the load request', async function() {
        globalThis.fetch = async () => new Response(null, {
            status: 404,
            statusText: 'Not Found'
        });
        const manager = new ModelLoadManager({} as any, 'http://localhost:5273/');

        try {
            await manager.load('missing-model');
            expect.fail('Should have thrown an HTTP error');
        } catch (error) {
            expect(error).to.be.instanceOf(Error);
            expect((error as Error).message).to.equal(
                'Error loading model missing-model from http://localhost:5273/: 404 Not Found'
            );
        }
    });

    it('should report a network error if the external service cannot be reached', async function() {
        globalThis.fetch = async () => {
            throw new Error('connection refused');
        };
        const manager = new ModelLoadManager({} as any, 'http://localhost:5273/');

        try {
            await manager.load('test-model');
            expect.fail('Should have thrown a network error');
        } catch (error) {
            expect(error).to.be.instanceOf(Error);
            expect((error as Error).message).to.equal(
                'Network error occurred while loading model test-model from http://localhost:5273/: connection refused'
            );
        }
    });
});
