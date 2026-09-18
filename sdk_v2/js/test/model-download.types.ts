import type { IModel } from "../src/imodel.js";

declare const model: IModel;
declare const progress: (percent: number) => void;
declare const maybeProgress: ((percent: number) => void) | undefined;
declare const signal: AbortSignal;
declare const maybeSignal: AbortSignal | undefined;
declare const secondSignal: AbortSignal;

void model.download();
void model.download(undefined);
void model.download(signal);
void model.download(maybeSignal);
void model.download(progress);
void model.download(maybeProgress);
void model.download(progress, signal);
void model.download(undefined, signal);

// @ts-expect-error A second signal is only valid when the first argument is a progress callback.
void model.download(signal, secondSignal);
