import type { IModel } from "../src/imodel.js";

declare const model: IModel;
declare const progress: (percent: number) => void;
declare const maybeProgress: ((percent: number) => void) | undefined;
declare const signal: AbortSignal;
declare const maybeSignal: AbortSignal | undefined;

void model.download();
void model.download(undefined);
void model.download(progress);
void model.download(maybeProgress);
void model.download(progress, signal);
void model.download(undefined, signal);
void model.download(progress, maybeSignal);

// Existing structural implementations remain compatible after adding the optional signal parameter.
declare const legacyDownload: (progressCallback?: (percent: number) => void) => Promise<void>;
const compatibleDownload: IModel["download"] = legacyDownload;
void compatibleDownload;

// @ts-expect-error AbortSignal remains the optional second argument so the original callback-first API is preserved.
void model.download(signal);
