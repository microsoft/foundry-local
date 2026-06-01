// JS-side error helpers. The native layer tags wrapper exceptions as
// `Error` instances with `name === "FoundryLocalError"` and a numeric `code`
// matching the wrapper's flErrorCode. This module exposes a type guard for
// consumers who want to branch on those.

/**
 * Numeric `code` values matching the C ABI's `flErrorCode` enum.
 *
 * Frozen plain object rather than a TS `enum` to keep the public surface
 * idiomatic ESM and to avoid `enum` emit overhead. Mirrors
 * `sdk_v2/cpp/include/foundry_local/foundry_local_c.h` `flErrorCode`.
 */
export const FlErrorCode = Object.freeze({
  Ok: 0,
  NotImplemented: 1,
  Internal: 2,
  InvalidArgument: 3,
  InvalidUsage: 4,
  OperationCancelled: 5,
} as const);

export type FlErrorCode = (typeof FlErrorCode)[keyof typeof FlErrorCode];

export interface FoundryLocalError extends Error {
  name: "FoundryLocalError";
  code: number;
}

export function isFoundryLocalError(value: unknown): value is FoundryLocalError {
  return (
    value instanceof Error &&
    value.name === "FoundryLocalError" &&
    typeof (value as { code?: unknown }).code === "number"
  );
}
