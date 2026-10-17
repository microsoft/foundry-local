// Foundry Local SDK - BYOM local registration E2E Demo
//
// Demonstrates the supported v2 BYOM flow:
//   1. Resolve/download a small catalog model.
//   2. Clone its on-disk model directory under a new location.
//   3. Explicitly register that directory in the LOCAL catalog.
//   4. Load it, run a typed ChatSession request, unload it.
//   5. Unregister the local model and delete the clone.

import { CatalogType, ChatSession, FoundryLocalManager, Item, ModelInfoBuilder, Request } from 'foundry-local-sdk';
import { promises as fs } from 'node:fs';
import * as path from 'node:path';
import process from 'node:process';

const PASS = '\x1b[92m[PASS]\x1b[0m';
const INFO = '\x1b[94m[INFO]\x1b[0m';
const SOURCE_ALIAS = 'qwen2.5-0.5b';
const BYOM_ALIAS = `fl-local-byom-${new Date().toISOString().replace(/[-:T.]/g, '').slice(0, 14)}`;
const BYOM_MODEL_ID = `${BYOM_ALIAS}:1`;
const BYOM_SUBDIR = 'byom_local_registration';
const PROMPT = 'What is the golden ratio? Answer in one short sentence.';

function section(title) {
    console.log(`\n${'='.repeat(72)}\n  ${title}\n${'='.repeat(72)}`);
}

async function copyTreeWithHardlinks(src, dst) {
    await fs.mkdir(dst, { recursive: true });
    const entries = await fs.readdir(src, { withFileTypes: true });
    for (const entry of entries) {
        const srcEntry = path.join(src, entry.name);
        const dstEntry = path.join(dst, entry.name);
        if (entry.isDirectory()) {
            await copyTreeWithHardlinks(srcEntry, dstEntry);
        } else {
            try {
                await fs.link(srcEntry, dstEntry);
            } catch {
                await fs.copyFile(srcEntry, dstEntry);
            }
        }
    }
}

async function cloneModelDir(src, dst, newName) {
    await copyTreeWithHardlinks(src, dst);
    const infPath = path.join(dst, 'inference_model.json');
    const srcInfoRaw = await fs.readFile(path.join(src, 'inference_model.json'), 'utf-8');
    const srcInfo = JSON.parse(srcInfoRaw);
    await fs.unlink(infPath);
    srcInfo.Name = newName;
    await fs.writeFile(infPath, JSON.stringify(srcInfo), 'utf-8');
}

function collectText(items) {
    const parts = [];
    for (const item of items) {
        if (item.type === 'text') {
            parts.push(item.text);
        } else if (item.type === 'message') {
            if (Array.isArray(item.parts)) {
                for (const part of item.parts) {
                    if (part.type === 'text') {
                        parts.push(part.text);
                    }
                }
            } else if (typeof item.content === 'string') {
                parts.push(item.content);
            }
        }
    }
    return parts.join('');
}

async function main() {
    console.log(`${INFO} BYOM alias for this run: ${BYOM_ALIAS}`);

    const manager = FoundryLocalManager.create({
        appName: 'foundry_local_byom_local_registration',
    });

    section('Step 1: Register execution providers');
    let currentEp = '';
    await manager.downloadAndRegisterEps((epName, percent) => {
        if (epName !== currentEp) {
            if (currentEp) process.stdout.write('\n');
            currentEp = epName;
        }
        process.stdout.write(`\r  EP [${epName.padEnd(24)}] ${percent.toFixed(2).padStart(6)}%`);
    });
    if (currentEp) process.stdout.write('\n');

    section(`Step 2: Resolve source catalog model '${SOURCE_ALIAS}'`);
    const source = await manager.catalog.getModel(SOURCE_ALIAS);
    const cpuVariant = source.variants.find((variant) => variant.info.runtime?.executionProvider === 'CPUExecutionProvider');
    if (!cpuVariant) {
        throw new Error('The BYOM demo requires a CPU execution-provider variant.');
    }
    source.selectVariant(cpuVariant);
    console.log(`  resolved to variant id: ${source.id}`);

    if (!source.isCached) {
        console.log(`  ${INFO} source not cached; downloading...`);
        await source.download((p) =>
            process.stdout.write(`\r  download: ${p.toFixed(2).padStart(6)}%`)
        );
        process.stdout.write('\n');
    }

    const srcPath = source.path;
    const publisherDir = path.dirname(path.dirname(srcPath));
    const cloneDir = path.join(publisherDir, BYOM_SUBDIR, BYOM_ALIAS);

    section('Step 3: Clone source model directory');
    console.log(`  cloning -> ${cloneDir}`);
    await cloneModelDir(srcPath, cloneDir, BYOM_ALIAS);

    const localCatalog = manager.getCatalog(CatalogType.Local);
    const metadata = new ModelInfoBuilder();
    metadata
        .setStringProperty('task', 'chat-completion')
        .setStringProperty('display_name', `${BYOM_ALIAS} local registration demo`);

    let registered = null;
    try {
        section('Step 4: Register BYOM in local catalog');
        registered = await localCatalog.registerModel(cloneDir, BYOM_MODEL_ID, metadata);
        console.log(`  ${PASS} registered ${registered.id}`);

        section('Step 5: Load and run typed ChatSession');
        await registered.load();
        const session = new ChatSession(registered);
        try {
            const response = await session.processRequest(
                new Request().addItem(Item.userMessage(PROMPT))
            );
            console.log(`  prompt: ${PROMPT}`);
            console.log(`  assistant: ${collectText(response.output)}`);
        } finally {
            metadata.dispose();
            session.dispose();
        }

        section('Step 6: Unload and unregister');
        await registered.unload();
        await localCatalog.unregisterModel(BYOM_MODEL_ID);
        registered = null;
        console.log(`  ${PASS} unregistered ${BYOM_MODEL_ID}`);
        return 0;
    } finally {
        if (registered !== null) {
            try {
                await registered.unload();
            } catch (err) {
                console.warn('Could not unload local model during cleanup:', err.message);
            }
            try {
                await localCatalog.unregisterModel(BYOM_MODEL_ID);
            } catch (err) {
                console.error('Could not unregister local model during cleanup:', err.message);
                throw err;
            }
        }
        await fs.rm(cloneDir, { recursive: true, force: true });
        try {
            await fs.rmdir(path.join(publisherDir, BYOM_SUBDIR));
        } catch (err) {
            if (err.code !== 'ENOTEMPTY') throw err;
        }
    }
}

main()
    .then((code) => process.exit(code))
    .catch((err) => {
        console.error(err);
        process.exit(1);
    });
