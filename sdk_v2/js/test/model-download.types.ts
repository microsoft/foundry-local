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
void model.download(signal);
void model.download(maybeSignal);

// @ts-expect-error Non-callback, non-signal values are rejected.
void model.download(42);
