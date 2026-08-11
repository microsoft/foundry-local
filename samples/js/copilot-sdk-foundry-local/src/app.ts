// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

/**
 * Basic Example — Copilot SDK + Foundry Local
 *
 * Demonstrates:
 *   - Bootstrapping Foundry Local (download, load, start web service)
 *   - Creating a BYOK session via Copilot SDK
 *   - Using Copilot's built-in tools (file reading) with a local model
 *   - Streaming responses and multi-turn conversation
 *
 * The app asks the local model to read its own source code using Copilot's
 * built-in `view` tool, then explain what it does — showing agentic tool
 * use powered entirely by on-device inference.
 *
 * Run:  npm start
 */

import { CopilotClient, approveAll } from "@github/copilot-sdk";
import { FoundryLocalManager } from "foundry-local-sdk";

const alias = "phi-4-mini";
const endpointUrl = "http://localhost:6543";

// Timeout for each model turn (ms).  Override with FOUNDRY_TIMEOUT_MS env var.
// Local models on CPU can be slow — increase this on less powerful hardware.
const TIMEOUT_MS = Number(process.env.FOUNDRY_TIMEOUT_MS) || 120_000;
const CLEANUP_TIMEOUT_MS = 5_000;

type Model = Awaited<ReturnType<FoundryLocalManager["catalog"]["getModel"]>>;

async function awaitBestEffort(operation: Promise<unknown>): Promise<void> {
    let timeoutId: ReturnType<typeof setTimeout> | undefined;
    const timeout = new Promise<void>((resolve) => {
        timeoutId = setTimeout(resolve, CLEANUP_TIMEOUT_MS);
        timeoutId.unref();
    });

    try {
        await Promise.race([operation.then(() => undefined, () => undefined), timeout]);
    } finally {
        if (timeoutId !== undefined) clearTimeout(timeoutId);
    }
}

// ---------------------------------------------------------------------------
// Helper: send a message and wait for the assistant's full reply.
// ---------------------------------------------------------------------------
async function sendMessage(
    session: Awaited<ReturnType<CopilotClient["createSession"]>>,
    prompt: string,
    timeoutMs = TIMEOUT_MS,
) {
    try {
        await session.sendAndWait({ prompt }, timeoutMs);
    } catch (err: any) {
        // Abort the failed turn so the session does not continue processing before the error propagates.
        console.error(`\n[sendMessage error: ${err?.message ?? err}]`);
        await awaitBestEffort(session.abort());
        throw err;
    }
}

async function main() {
    let manager: FoundryLocalManager | undefined;
    let model: Model | undefined;
    let client: CopilotClient | undefined;
    let session: Awaited<ReturnType<CopilotClient["createSession"]>> | undefined;
    let isUnwindingDueToError = false;

    try {
        // --- Initialize Foundry Local ---
        console.log("Initializing Foundry Local...");
        manager = FoundryLocalManager.create({
            appName: "foundry_local_samples",
            webServiceUrls: endpointUrl,
        });

        // Download and register all execution providers.
        let currentEp = '';
        await manager.downloadAndRegisterEps((epName, percent) => {
            if (epName !== currentEp) {
                if (currentEp !== '') process.stdout.write('\n');
                currentEp = epName;
            }
            process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
        });
        if (currentEp !== '') process.stdout.write('\n');

        model = await manager.catalog.getModel(alias);
        await model.download();
        await model.load();
        console.log(`Model: ${model.id}`);

        manager.startWebService();
        const endpoint = endpointUrl + "/v1";
        console.log(`Endpoint: ${endpoint}\n`);

        // --- Create a BYOK session with Copilot's built-in tools ---
        client = new CopilotClient();

        session = await client.createSession({
            onPermissionRequest: approveAll,
            model: model.id,
            provider: {
                type: "openai",
                baseUrl: endpoint,
                apiKey: "local",
                wireApi: "completions",
            },
            streaming: true,
            workingDirectory: process.cwd(),
            systemMessage: {
                content:
                    "You are a helpful AI assistant running locally via Foundry Local. You can use your tools to read files and answer questions about them.",
            },
        });

        // print out current directory
        console.log("Current working directory:", process.cwd());

        // Stream assistant text to stdout
        session.on("assistant.message_delta", (event) => {
            process.stdout.write(event.data.deltaContent);
        });
        session.on("tool.execution_start", (event) => {
            console.log(`\n  [Tool: ${(event as any).data?.toolName ?? "unknown"}]`);
        });

        // --- Turn 1: Ask the model to read and explain its own source ---
        console.log("--- Turn 1: Read and explain this app ---\n");
        process.stdout.write("Assistant: ");
        await sendMessage(
            session,
            "Read src/app.ts, then explain what this application does in a few sentences.",
        );
        console.log("\n");

        // --- Turn 2: Follow-up leveraging conversation context ---
        console.log("--- Turn 2: What technologies does it use? ---\n");
        process.stdout.write("Assistant: ");
        await sendMessage(session, "What key technologies and patterns does it demonstrate?");
        console.log("\n");

        console.log("Done!");
    } catch (error) {
        isUnwindingDueToError = true;
        throw error;
    } finally {
        // Service sessions must release the model before it can be unloaded.
        if (session) {
            await awaitBestEffort(session.disconnect());
        }
        if (client) {
            await awaitBestEffort(client.forceStop());
        }
        // Native cleanup may block behind a failed request; forced process exit lets the OS reclaim it.
        if (!isUnwindingDueToError) {
            if (manager) {
                console.log("Stopping web service...");
                try {
                    manager.stopWebService();
                } catch (e) {
                    console.warn("Warning: failed to stop web service:", e);
                }
            }
            if (model) {
                console.log("Unloading model...");
                await model.unload().catch((e) => {
                    console.warn("Warning: failed to unload model:", e);
                });
            }
        }
    }
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
