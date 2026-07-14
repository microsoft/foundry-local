### Description

This document explains how structured outputs work in Foundry Local. This feature enables constrained decoding using grammars that guide the decoding loop.

By default, Foundry Local will use LARK grammar with any tool to auto-select between text-only output and tool-call output. Depending on your use case, there are additional recommendations below for improved results.

### Usage

The `response_format` and `tool_choice` can be any of the following.

```py
tool_choice = [
    "none",
    {"type": "none"},
    "auto",
    {"type": "auto"},
    "required",
    {"type": "required"},
    {"type": "function", "function": {"name": "function_name"}},
]
```

- `none` or `{"type": "none"}` = no tools should be chosen
- `auto` or `{"type": "auto"}` = any tool should be chosen
- `required` or `{"type": "required"}` = a tool should be chosen
- `{"type": "function", "function": {"name": "function_name"}}` = a specific tool should be chosen

```py
response_format = [
    {"type": "text"},
    {"type": "json_object"},
    {"type": "json_schema"},
    {"type": "lark_grammar"},
    {"type": "json_schema", "json_schema": json_schema},
    {"type": "lark_grammar", "lark_grammar": lark_grammar},
]
```

- `{"type": "text"}` = text-only output
- `{"type": "json_object"}` = JSON-only output
- `{"type": "json_schema"}` = JSON-only output
- `{"type": "lark_grammar"}` = text-only output or tool-call output with JSON (when combined with `tool_choice`, this can become text-only output or tool-call output)
- `{"type": "json_schema", "json_schema": json_schema}` = same as `{"type": "json_schema"}` but follows the user-provided JSON schema stored in `json_schema`
- `{"type": "lark_grammar", "lark_grammar": lark_grammar}` = same as `{"type": "lark_grammar"}` but follows the user-provided LARK grammar stored in `lark_grammar`

Here is an example of how you can add `response_format` and `tool_choice` to your chat completions API call.

```py
response = client.chat.completions.create(
    model=manager.get_model_info(alias).id,
    messages=input_list,
    tools=tools,
    stream=True,
    response_format={"type": "lark_grammar"},
    tool_choice={"type": "required"},
)
```

### Context

Most models cannot reliably generate tool calls or switch between producing text and a tool call. Structured outputs improves both cases by allowing users to choose response formats and tool choices for guiding how the output should be structured.

### Recommendations

Default values if not provided:

```py
response_format = {"type": "lark_grammar"}
tool_choice = "auto"
```

For general text-based scenarios where tool calling is not needed:

```py
response_format = {"type": "text"}
tool_choice = "none"
```

For general text-based scenarios where JSON output is desired but tool calling is not needed:

```py
response_format = {"type": "json_object"} or response_format = {"type": "json_schema"}
tool_choice = "none"
```

For scenarios where text-based output or tool-calling output could potentially happen:

```py
response_format = {"type": "lark_grammar"}
tool_choice = "auto"
```

For tool-calling scenarios where a tool must be called:

```py
response_format = {"type": "lark_grammar"}
tool_choice = "required"
```

For tool-calling scenarios where a specific tool must be called:

```py
response_format = {"type": "lark_grammar"}
tool_choice = {"type": "function", "function": {"name": "function_name"}}
```

For scenarios where you have a specific JSON schema you want the model to follow:

```py
response_format = {"type": "json_schema", "json_schema": json_schema}
```

For scenarios where you have a specific LARK grammar you want the model to follow:

```py
response_format = {"type": "lark_grammar", "lark_grammar": lark_grammar}
```