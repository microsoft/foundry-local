import {
  type DecisionRequest,
  type DecisionSession,
  type RankingRequest,
  type RankingSession,
  Request,
  TypedDecisionSession,
  type TypedDecisionSession as TypedDecisionSessionInstance,
} from "../src/index.js";

declare const ranking: RankingSession;
declare const decisions: DecisionSession;
const compatibleDecision: TypedDecisionSessionInstance = decisions;
void compatibleDecision;
new TypedDecisionSession({} as never);

const rankingRequest = {
  context: { priority: 1, tags: ["local", true] },
  question: "Which is best?",
  answers: ["first", "second"],
} satisfies RankingRequest;

const decisionRequest = {
  state: { enabled: true },
  questions: {
    quality: {
      type: "choice",
      criteria: { good: "Good", bad: "Bad" },
      instructions: "Choose one",
    },
  },
} satisfies DecisionRequest;

ranking.rank(rankingRequest).then((result) => result.ranked[0]?.prob);
decisions.decide(decisionRequest).then((result) => result.answers.quality?.type);
// @ts-expect-error Predictive sessions are one-shot and do not stream.
ranking.processStreamingRequest(new Request());

// @ts-expect-error Ranking answers are strings.
ranking.rank({ answers: [1, 2] });
// @ts-expect-error Decision question types are a closed union.
decisions.decide({ questions: { invalid: { type: "freeform" } } });
