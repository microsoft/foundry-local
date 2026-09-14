from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor

from openai import OpenAI

DEFAULT_PROMPTS = (
    "Write a Python function that returns the first unique character in a string.",
    "Explain when to use a dataclass instead of a dictionary in Python.",
    "Find the bug: `for i in range(len(xs) + 1): print(xs[i])`.",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send concurrent coding requests to a Foundry Local endpoint."
    )
    parser.add_argument(
        "--url", default="http://127.0.0.1:5272/v1", help="OpenAI-compatible /v1 URL."
    )
    parser.add_argument(
        "--model-id",
        default="local-coding-cuda:1",
        help="Model ID printed by launcher.py.",
    )
    return parser.parse_args()


def complete(url: str, model_id: str, prompt: str) -> str:
    with OpenAI(base_url=url.rstrip("/"), api_key="not-needed") as client:
        response = client.chat.completions.create(
            model=model_id,
            messages=[{"role": "user", "content": prompt}],
            temperature=0,
            max_tokens=256,
        )
    return response.choices[0].message.content or ""


def main() -> None:
    args = parse_args()
    with ThreadPoolExecutor(max_workers=len(DEFAULT_PROMPTS)) as executor:
        futures = [
            executor.submit(complete, args.url, args.model_id, prompt)
            for prompt in DEFAULT_PROMPTS
        ]
        for prompt, future in zip(DEFAULT_PROMPTS, futures, strict=True):
            print(f"\nUSER: {prompt}\nASSISTANT: {future.result()}")


if __name__ == "__main__":
    main()
