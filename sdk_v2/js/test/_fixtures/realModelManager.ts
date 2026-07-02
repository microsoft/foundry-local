// Real-model fixture for v2 SDK integration tests that need an actual loaded
// inference session (model lifecycle, chat session, eventual streaming).
//
// Gated by the FOUNDRY_TEST_DATA_DIR environment variable, mirroring the C++
// SDK's SharedTestEnv pattern (see cpp-testing.instructions.md). When the
// env var is not set, tests must skip via `describe.skipIf(!haveTestModelCache)`
// rather than fail.
//
// CI policy: if running under CI (TF_BUILD or CI env var) AND the selected
// model is not already on-disk in the cache, tests skip instead of triggering
// a multi-gigabyte download. Local devs implicitly opt into downloads simply
// by setting FOUNDRY_TEST_DATA_DIR.
import { existsSync, statSync } from "node:fs";

import type { Catalog } from "../../src/catalog.js";
import { FoundryLocalManager } from "../../src/foundryLocalManager.js";
import type { IModel } from "../../src/imodel.js";

const envCache = process.env.FOUNDRY_TEST_DATA_DIR;
const cacheDirExists =
  envCache !== undefined && envCache.length > 0 && existsSync(envCache) && statSync(envCache).isDirectory();

/**
 * True iff `FOUNDRY_TEST_DATA_DIR` is set and points at a real directory.
 * Tests should gate on this via `describe.skipIf(!haveTestModelCache)`.
 */
export const haveTestModelCache: boolean = cacheDirExists;

export const testModelCacheDiagnostic = haveTestModelCache
  ? `[v2 SDK real-model tests] using cache dir ${envCache}`
  : "[v2 SDK real-model tests] SKIPPED — FOUNDRY_TEST_DATA_DIR is not set or does not exist";

const isCi: boolean = process.env.CI !== undefined || process.env.TF_BUILD !== undefined;
const debugRealModelFixture: boolean =
  isCi || process.env.FOUNDRY_JS_TEST_DEBUG === "1" || process.env.FOUNDRY_JS_TEST_DEBUG === "true";

function fixtureLog(message: string): void {
  if (!debugRealModelFixture) {
    return;
  }
  console.log(`[realModelManager] ${message}`);
}

function describeModel(model: IModel): string {
  const info = model.info;
  const size = info.fileSizeMb ?? Number.NaN;
  const sizeText = Number.isFinite(size) ? `${size}MB` : "size=unknown";
  return `${info.name} (task=${info.task}, device=${info.deviceType}, cached=${model.isCached}, ${sizeText})`;
}

export interface RealModelManagerOptions {
  /** Override the appName. Defaults to a fixed test id. */
  readonly appName?: string;
  /**
   * Catalog task to filter on when picking a fallback model. Defaults to
   * "chat-completion".
   */
  readonly task?: string;
  /**
   * Preferred model alias / name. If supplied AND the model is in the
   * catalog, it wins over the smallest-by-task fallback. Defaults to
   * "qwen2.5-0.5b" — alias of the smallest chat model we ship.
   */
  readonly namePreference?: string;
}

export interface RealModelManagerFixture {
  readonly manager: FoundryLocalManager;
  readonly catalog: Catalog;
  readonly model: IModel;
}

/**
 * Build a Manager pointed at `FOUNDRY_TEST_DATA_DIR`, pick a small chat
 * model, ensure it is on disk + loaded, and return the fixture. The caller
 * passes the returned fixture to {@link teardownRealModelManager}.
 *
 * Throws (rather than skips) if {@link haveTestModelCache} is false — callers
 * must gate the `describe` block themselves.
 */
export async function setupRealModelManager(opts: RealModelManagerOptions = {}): Promise<RealModelManagerFixture> {
  fixtureLog(
    `setup start: haveTestModelCache=${haveTestModelCache} envCache=${envCache ?? "(unset)"} CI=${isCi} opts=${JSON.stringify(opts)}`,
  );
  if (!haveTestModelCache || envCache === undefined) {
    throw new Error(
      "setupRealModelManager called without FOUNDRY_TEST_DATA_DIR — gate the describe with `skipIf(!haveTestModelCache)`",
    );
  }

  const manager = FoundryLocalManager.create({
    appName: opts.appName ?? "foundry-local-js-sdk-v2-real-tests",
    modelCacheDir: envCache,
  });
  const catalog = manager.catalog;
  const namePref = opts.namePreference ?? "qwen2.5-0.5b";
  const task = opts.task ?? "chat-completion";
  const all = await catalog.getModels();
  fixtureLog(`catalog loaded: modelCount=${all.length} namePref='${namePref}' task='${task}'`);

  const normalizeVersionSuffix = (value: string): string => value.replace(/-\d+$/, "");
  const nameMatchesPreference = (candidateName: string, preference: string): boolean => {
    if (candidateName === preference) {
      return true;
    }

    const candidateBase = normalizeVersionSuffix(candidateName);
    const prefBase = normalizeVersionSuffix(preference);

    // Handles versioned/unversioned pairs in either direction, e.g.
    //  - preference: openai-whisper-tiny-generic-cpu
    //  - candidate : openai-whisper-tiny-generic-cpu-4
    return candidateBase === prefBase;
  };

  const bySize = (a: IModel, b: IModel): number =>
    (a.info.fileSizeMb ?? Number.POSITIVE_INFINITY) - (b.info.fileSizeMb ?? Number.POSITIVE_INFINITY);

  const preferCachedThenSize = (models: IModel[]): IModel | undefined => {
    fixtureLog(`preferCachedThenSize called with ${models.length} candidate(s)`);
    const cached = models.filter((m) => m.isCached);
    fixtureLog(`preferCachedThenSize cached subset count=${cached.length}`);
    if (cached.length > 0) {
      const selected = cached.sort(bySize)[0];
      fixtureLog(`preferCachedThenSize selected cached candidate: ${describeModel(selected)}`);
      return selected;
    }
    const selected = models.sort(bySize)[0];
    if (selected !== undefined) {
      fixtureLog(`preferCachedThenSize selected non-cached candidate: ${describeModel(selected)}`);
    } else {
      fixtureLog("preferCachedThenSize had no candidates to select");
    }
    return selected;
  };

  // Preference 1: exact name / alias hit (V1 throws on miss, so swallow).
  let model: IModel | undefined;
  try {
    fixtureLog(`preference stage 1: attempting catalog.getModel('${namePref}')`);
    const exact = await catalog.getModel(namePref);
    fixtureLog(`preference stage 1: getModel hit ${describeModel(exact)}`);
    if (exact.info.deviceType === "CPU") {
      model = exact;
      fixtureLog("preference stage 1 accepted exact hit (CPU)");
    } else {
      fixtureLog(`preference stage 1 rejected exact hit due to deviceType='${exact.info.deviceType}'`);
    }
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    fixtureLog(`preference stage 1 miss/error from getModel('${namePref}'): ${message}`);
    model = undefined;
  }

  // Preference 1b: robust preference matching with CPU + cache-first behavior.
  // Handles versioned/unversioned names in either direction.
  if (model === undefined) {
    const preferredMatches = all.filter((m) => {
      const info = m.info;
      return info.deviceType === "CPU" && nameMatchesPreference(info.name, namePref);
    });
    fixtureLog(`preference stage 1b: robust name match candidates=${preferredMatches.length}`);
    if (preferredMatches.length > 0) {
      fixtureLog(
        `preference stage 1b candidates: ${preferredMatches
          .map((candidate) => `${candidate.info.name}[cached=${candidate.isCached}]`)
          .join(", ")}`,
      );
    }
    model = preferCachedThenSize(preferredMatches);
    if (model !== undefined) {
      fixtureLog(`preference stage 1b selected ${describeModel(model)}`);
    }
  }

  // Preference 2: smallest model matching the task filter.
  if (model === undefined) {
    const matching = all.filter((m) => {
      const info = m.info;
      return info.task === task && info.deviceType === "CPU";
    });
    fixtureLog(`preference stage 2: task/device candidates=${matching.length}`);
    if (matching.length > 0) {
      fixtureLog(
        `preference stage 2 candidates: ${matching
          .map((candidate) => `${candidate.info.name}[cached=${candidate.isCached}]`)
          .join(", ")}`,
      );
    }
    if (matching.length === 0) {
      fixtureLog(`preference stage 2 found no candidates for task='${task}' and device='CPU'`);
      manager.dispose();
      throw new Error(
        `No catalog model matches task='${task}' deviceType='CPU' (and preference '${namePref}' missing)`,
      );
    }
    model = preferCachedThenSize(matching);
    if (model !== undefined) {
      fixtureLog(`preference stage 2 selected ${describeModel(model)}`);
    }
  }
  if (model === undefined) {
    fixtureLog("final selection failed unexpectedly (model is undefined)");
    manager.dispose();
    throw new Error("Unreachable: model selection failed");
  }

  fixtureLog(`final selected model: ${describeModel(model)}`);

  // CI gate: refuse to trigger a real download.
  if (isCi && !model.isCached) {
    fixtureLog(`CI gate triggered skip: selected model is not cached (${model.info.name})`);
    manager.dispose();
    throw new SkipFixture(
      `[CI] selected model '${model.info.name}' is not in the cache; skipping to avoid a download.`,
    );
  }

  fixtureLog(`loading selected model '${model.info.name}'`);

  await model.load();
  fixtureLog(`model loaded successfully '${model.info.name}'`);
  return { manager, catalog, model };
}

export function teardownRealModelManager(fixture: RealModelManagerFixture | undefined): void {
  if (fixture === undefined) return;
  if (!fixture.manager.disposed) {
    fixture.manager.dispose();
  }
}

/**
 * Sentinel error type that tests recognise as "we couldn't run this fixture
 * for an environmental reason — skip the suite". The caller catches this
 * in a beforeAll and flips a module-level flag to gate `describe.skipIf`.
 */
export class SkipFixture extends Error {
  override name = "SkipFixture";
}
