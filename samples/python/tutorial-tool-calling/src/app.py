# <complete_code>
# <imports>
import json

from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    TextItem,
    ToolCallItem,
    ToolResultItem,
)
# </imports>


# <tool_definitions>
GET_WEATHER_SCHEMA = json.dumps({
    "type": "object",
    "properties": {
        "location": {"type": "string", "description": "The city or location"},
        "unit": {
            "type": "string",
            "enum": ["celsius", "fahrenheit"],
            "description": "Temperature unit",
        },
    },
    "required": ["location"],
})

CALCULATE_SCHEMA = json.dumps({
    "type": "object",
    "properties": {
        "expression": {"type": "string", "description": "The math expression to evaluate"},
    },
    "required": ["expression"],
})


def get_weather(location: str, unit: str = "celsius"):
    return {
        "location": location,
        "temperature": 22 if unit == "celsius" else 72,
        "unit": unit,
        "condition": "Sunny",
    }


def calculate(expression: str):
    allowed = set("0123456789+-*/(). ")
    if not all(c in allowed for c in expression):
        return {"error": "Invalid expression"}
    try:
        result = eval(expression)  # noqa: S307 - sandboxed via the allowed-char check
        return {"expression": expression, "result": result}
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}


tool_functions = {
    "get_weather": get_weather,
    "calculate": calculate,
}
# </tool_definitions>


def main():
    # <init>
    config = Configuration(app_name="foundry_local_samples")
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance

    current_ep = ""
    def ep_progress(ep_name: str, percent: float):
        nonlocal current_ep
        if ep_name != current_ep:
            if current_ep:
                print()
            current_ep = ep_name
        print(f"\r  {ep_name:<30}  {percent:5.1f}%", end="", flush=True)

    manager.download_and_register_eps(progress_callback=ep_progress)
    if current_ep:
        print()

    model = manager.catalog.get_model("qwen2.5-0.5b")
    model.download(lambda progress: print(f"\rDownloading model: {progress:.2f}%", end="", flush=True))
    print()
    model.load()
    print("Model loaded and ready.")
    # </init>

    # <tool_loop>
    # Tools register once on the session; the model sees them on every turn.
    with ChatSession(model) as session:
        session.add_tool_definition(
            "get_weather", "Get the current weather for a location", GET_WEATHER_SCHEMA
        )
        session.add_tool_definition(
            "calculate", "Perform a math calculation", CALCULATE_SCHEMA
        )

        # Seed the conversation with a system message on the first user turn.
        system_message = MessageItem.system(
            "You are a helpful assistant with access to tools. Use them when needed "
            "to answer questions accurately. Only call tools by the exact names provided "
            "(get_weather, calculate). For arithmetic, call 'calculate' with an expression "
            "like '7 * 8'."
        )

        print("\nTool-calling assistant ready! Type 'quit' to exit.\n")

        first_turn = True
        while True:
            # Print prompt explicitly: input()'s prompt goes through readline to stderr
            # when stdout isn't a TTY, which breaks captured-output scenarios.
            print("You: ", end="", flush=True)
            user_input = input()
            if user_input.strip().lower() in ("quit", "exit"):
                break

            with Request() as req:
                if first_turn:
                    req.add_item(system_message)
                    first_turn = False
                req.add_item(MessageItem.user(user_input))

                response = session.process_request(req)

            # Run any tool calls the model produced, then loop until the model emits text.
            while True:
                pending_results: list[ToolResultItem] = []
                final_text_parts: list[str] = []
                for item in response:
                    if isinstance(item, ToolCallItem):
                        args = json.loads(item.arguments) if item.arguments else {}
                        print(f"  Tool call: {item.name}({args})")

                        # Small models occasionally invent a tool name that wasn't registered.
                        # Surface a structured error back to the model so it can retry with a real name.
                        func = tool_functions.get(item.name)
                        if func is None:
                            result = {
                                "error": f"Unknown tool '{item.name}'. "
                                         f"Available: {sorted(tool_functions)}.",
                            }
                        else:
                            try:
                                result = func(**args)
                            except Exception as e:  # noqa: BLE001
                                result = {"error": str(e)}

                        pending_results.append(
                            ToolResultItem(item.call_id, json.dumps(result))
                        )
                    elif isinstance(item, MessageItem):
                        for part in item.parts:
                            if isinstance(part, TextItem):
                                final_text_parts.append(part.text)

                if not pending_results:
                    answer = "".join(final_text_parts).strip()
                    print(f"Assistant: {answer}\n")
                    break

                with Request() as follow_up:
                    for tool_result in pending_results:
                        follow_up.add_item(tool_result)
                    response = session.process_request(follow_up)
    # </tool_loop>

    model.unload()
    print("Model unloaded. Goodbye!")


if __name__ == "__main__":
    main()
# </complete_code>
