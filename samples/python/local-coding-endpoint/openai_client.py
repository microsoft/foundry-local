from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor

from openai import OpenAI

ENDPOINT_URL = "http://127.0.0.1:5272/v1"
MODEL_ID = "qwen38-dflash2-coding:1"
DEFAULT_PROMPTS = (
    "Write a Python function that returns the first unique character in a string.",
    "Explain when to use a dataclass instead of a dictionary in Python.",
    "Find the bug: `for i in range(len(xs) + 1): print(xs[i])`.",
)


def complete(prompt: str) -> str:
    with OpenAI(base_url=ENDPOINT_URL, api_key="not-needed") as client:
        response = client.chat.completions.create(
            model=MODEL_ID,
            messages=[{"role": "user", "content": prompt}],
            temperature=0,
            max_tokens=256,
        )
    return response.choices[0].message.content or ""


def main() -> None:
    with ThreadPoolExecutor(max_workers=len(DEFAULT_PROMPTS)) as executor:
        futures = [executor.submit(complete, prompt) for prompt in DEFAULT_PROMPTS]
        for prompt, future in zip(DEFAULT_PROMPTS, futures, strict=True):
            print(f"\nUSER: {prompt}\nASSISTANT: {future.result()}")


if __name__ == "__main__":
    main()
