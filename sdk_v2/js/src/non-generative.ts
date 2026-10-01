/** JSON values accepted by ranking context and typed-decision structured fields. */
export type StructuredValue =
  | null
  | boolean
  | number
  | string
  | ReadonlyArray<StructuredValue>
  | { readonly [key: string]: StructuredValue };

/** Request contract used by `POST /v1/rank` and {@link RankingSession.rank}. */
export interface RankingRequest {
  readonly context?: StructuredValue;
  readonly question?: string;
  readonly answers: ReadonlyArray<string>;
  readonly model?: string;
  readonly temperature?: number;
}

export interface RankedCandidate {
  readonly rank: number;
  readonly candidate: string;
  readonly prob: number;
}

/** Result contract returned by `POST /v1/rank` and {@link RankingSession.rank}. */
export interface RankingResult {
  readonly model: string;
  readonly ranked: ReadonlyArray<RankedCandidate>;
}

export type DecisionQuestionType = "noul" | "choice" | "score";

export interface DecisionQuestion {
  readonly type: DecisionQuestionType;
  readonly criteria?: StructuredValue;
  readonly instructions?: StructuredValue;
}

/** Request contract used by `POST /v1/systemone` and `DecisionSession.decide`. */
export interface DecisionRequest {
  readonly state?: StructuredValue;
  readonly questions: Readonly<Record<string, DecisionQuestion>>;
  readonly model?: string;
  readonly temperature?: number;
}

interface TypedDecisionAnswerBase {
  readonly confidence?: number;
  readonly probabilities?: Readonly<Record<string, number>>;
  readonly legend?: Readonly<Record<string, string>>;
}

export type DecisionAnswer =
  | (TypedDecisionAnswerBase & {
      readonly type: "noul";
      readonly noul: number;
    })
  | (TypedDecisionAnswerBase & {
      readonly type: "choice";
      readonly choice: string;
    })
  | (TypedDecisionAnswerBase & {
      readonly type: "score";
      readonly score: number;
    });

/** Result contract returned by `POST /v1/systemone` and `DecisionSession.decide`. */
export interface DecisionResult {
  readonly model: string;
  readonly answers: Readonly<Record<string, DecisionAnswer>>;
  readonly usage: {
    readonly billing_units: number;
  };
}

/** @deprecated Use {@link DecisionRequest}. */
export type TypedDecisionRequest = DecisionRequest;
/** @deprecated Use {@link DecisionQuestion}. */
export type TypedDecisionQuestion = DecisionQuestion;
/** @deprecated Use {@link DecisionQuestionType}. */
export type TypedDecisionQuestionType = DecisionQuestionType;
/** @deprecated Use {@link DecisionAnswer}. */
export type TypedDecisionAnswer = DecisionAnswer;
/** @deprecated Use {@link DecisionResult}. */
export type TypedDecisionResult = DecisionResult;
