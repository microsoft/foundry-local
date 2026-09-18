import type { IModel } from "../src/imodel.js";

declare const model: IModel;
declare const progress: (percent: number) => void;
declare const signal: AbortSignal;

void model.download();
void model.download(signal);
void model.download(progress);
void model.download(progress, signal);
void model.download(undefined, signal);
