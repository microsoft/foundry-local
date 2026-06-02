/**
 * Foundry Local SDK - WinML 2.0 EP Verification Script (JavaScript)
 *
 * Verifies:
 *   1. Execution providers are discovered and registered
 *   2. Accelerated models appear in catalog after EP registration
 *   3. Streaming chat completions work on an accelerated model
 */

import { ChatSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";

const PASS = "\x1b[92m[PASS]\x1b[0m";
const FAIL = "\x1b[91m[FAIL]\x1b[0m";
const INFO = "\x1b[94m[INFO]\x1b[0m";
const WARN = "\x1b[93m[WARN]\x1b[0m";

const results = [];

function logResult(testName, passed, detail = "") {
  const status = passed ? PASS : FAIL;
  const msg = detail ? `${status} ${testName} - ${detail}` : `${status} ${testName}`;
  console.log(msg);
  results.push({ testName, passed });
}

function printSeparator(title) {
  console.log(`\n${"=".repeat(60)}`);
  console.log(`  ${title}`);
  console.log(`${"=".repeat(60)}\n`);
}

function isAcceleratedVariant(variant) {
  const runtime = variant.info?.runtime;
  return Boolean(runtime && ["GPU", "NPU"].includes(runtime.deviceType));
}

async function main() {
  // ── 0. Initialize FoundryLocalManager ──────────────────────
  printSeparator("Initialization");
  const manager = FoundryLocalManager.create({
    appName: "verify_winml",
    logLevel: "info",
  });
  console.log(`${INFO} FoundryLocalManager initialized.`);

  // ── 1. Discover & Register EPs ────────────────────────────
  printSeparator("Step 1: Discover & Register Execution Providers");
  let eps = [];
  try {
    eps = manager.discoverEps();
    console.log(`${INFO} Discovered ${eps.length} execution providers:`);
    for (const ep of eps) {
      console.log(`  - ${ep.name.padEnd(40)}  Registered: ${ep.isRegistered}`);
    }
    logResult("EP Discovery", true, `${eps.length} EP(s) found`);
  } catch (e) {
    logResult("EP Discovery", false, e.message);
  }

  if (!eps.length) {
    const detail = "No execution providers discovered on this machine";
    logResult("EP Download & Registration", false, detail);
    console.log(`\n${FAIL} ${detail}.`);
    printSummary();
    return;
  }

  try {
    let lastProgressEp = null;
    let lastProgressPercent = -1;
    const result = await manager.downloadAndRegisterEps((epName, percent) => {
      if (lastProgressEp && (lastProgressEp !== epName || percent < lastProgressPercent)) {
        process.stdout.write("\n");
      }
      lastProgressEp = epName;
      lastProgressPercent = percent;
      process.stdout.write(`\r  Downloading ${epName}: ${percent.toFixed(1)}%`);
    });
    if (lastProgressEp) {
      console.log();
    }

    console.log(`${INFO} EP registration result: success=${result.success}, status=${result.status}`);
    if (result.registeredEps?.length) {
      console.log(`  Registered: ${result.registeredEps.join(", ")}`);
    }
    if (result.failedEps?.length) {
      console.log(`  Failed:     ${result.failedEps.join(", ")}`);
    }

    const downloadOk = result.success;
    const detail = downloadOk && result.registeredEps?.length
      ? `${result.registeredEps.length} EP(s) registered`
      : result.status;
    logResult("EP Download & Registration", downloadOk, detail);
    if (!downloadOk) {
      printSummary();
      return;
    }
  } catch (e) {
    console.log();
    logResult("EP Download & Registration", false, e.message);
    printSummary();
    return;
  }

  // ── 2. List Models & Find Accelerated Variants ────────────
  printSeparator("Step 2: Model Catalog - Accelerated Models");
  const models = await manager.catalog.getModels();
  console.log(`${INFO} Total models in catalog: ${models.length}`);

  const acceleratedVariants = [];

  for (const model of models) {
    for (const variant of model.variants) {
      if (isAcceleratedVariant(variant)) {
        acceleratedVariants.push(variant);
      }
    }
  }

  console.log(`${INFO} Accelerated model variants: ${acceleratedVariants.length}`);
  for (const variant of acceleratedVariants) {
    const runtime = variant.info?.runtime;
    const ep = runtime?.executionProvider || "?";
    const device = runtime?.deviceType || "?";
    console.log(`  - ${variant.id.padEnd(50)}  Device: ${String(device).padEnd(3)}  EP: ${ep}`);
  }

  logResult(
    "Catalog - Accelerated models found",
    acceleratedVariants.length > 0,
    `${acceleratedVariants.length} accelerated variant(s)`,
  );

  if (!acceleratedVariants.length) {
    console.log(`\n${FAIL} No accelerated model variants are available.`);
    console.log(`${WARN} Ensure the system has a compatible accelerator and matching model variants installed.`);
    printSummary();
    process.exit(1);
  }

  // ── 3. Download & Load Model ──────────────────────────────
  printSeparator("Step 3: Download & Load Model");

  let chosen = null;
  let downloadedAny = false;
  let lastLoadError = null;
  for (const candidate of acceleratedVariants) {
    const ep = candidate.info?.runtime?.executionProvider || "unknown";
    console.log(`\n${INFO} Trying model: ${candidate.id} (EP: ${ep})`);

    try {
      await candidate.download((percent) => {
        process.stdout.write(`\r  Downloading model: ${percent.toFixed(1)}%`);
      });
      console.log();
      downloadedAny = true;
    } catch (e) {
      console.log();
      console.log(`${WARN} Skipping ${candidate.id}: download failed: ${e.message}`);
      lastLoadError = e;
      continue;
    }

    try {
      await candidate.load();
      chosen = candidate;
      break;
    } catch (e) {
      console.log(`${WARN} Skipping ${candidate.id}: load failed: ${e.message}`);
      lastLoadError = e;
    }
  }

  logResult(
    "Model Download",
    downloadedAny,
    downloadedAny ? "At least one accelerated variant downloaded" : lastLoadError?.message || "No accelerated variant could be downloaded",
  );

  if (!chosen) {
    logResult("Model Load", false, lastLoadError?.message || "No accelerated variant could be loaded on this machine");
    printSummary();
    process.exit(1);
  }

  logResult("Model Load", true, `Loaded ${chosen.id}`);

  // ── 4. Streaming Chat Completions (Native SDK) ────────────
  printSeparator("Step 4: Streaming Chat Completions (Native)");
  const session = new ChatSession(chosen);
  session.setOptions({ search: { temperature: 0, maxOutputTokens: 16 } });
  try {
    const request = new Request();
    request.addItem(Item.systemMessage("You are a helpful assistant."));
    request.addItem(Item.userMessage("What is 2 + 2? Reply with just the number."));

    let responseText = "";
    const start = Date.now();
    for await (const item of session.processStreamingRequest(request)) {
      if (item.type === "text" && item.text) {
        responseText += item.text;
        process.stdout.write(item.text);
      }
    }
    const elapsed = ((Date.now() - start) / 1000).toFixed(2);
    console.log();
    logResult("Streaming Chat (Native)", responseText.length > 0, `${responseText.length} chars in ${elapsed}s`);
  } catch (e) {
    logResult("Streaming Chat (Native)", false, e.message);
  } finally {
    session.dispose();
  }

  try {
    await chosen.unload();
    console.log(`${INFO} Model unloaded.`);
  } catch (e) {
    console.warn(`${WARN} Failed to unload model: ${e.message}`);
  }

  printSummary();
}

function printSummary() {
  printSeparator("Summary");
  const passed = results.filter((r) => r.passed).length;
  for (const { testName, passed: p } of results) {
    console.log(`  ${p ? "✓" : "✗"} ${testName}`);
  }
  console.log(`\n  ${passed}/${results.length} tests passed`);
  if (passed < results.length) process.exit(1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
