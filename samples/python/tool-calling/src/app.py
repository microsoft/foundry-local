# <complete_code>
# <imports>
import json

from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
    TextItemType,
    ToolCallItem,
    ToolChoice,
    ToolResultItem,
)
# </imports>


def get_weather(location, unit="celsius"):
    return {
        "location": location,
        "temperature": 22 if unit == "celsius" else 72,
        "unit": unit,
        "condition": "Sunny",
    }


def calculate(expression):
    allowed = set("0123456789+-*/(). ")
    if not all(c in allowed for c in expression):
        return {"error": "Invalid expression"}
    try:
        result = eval(expression)
        return {"expression": expression, "result": result}
    except Exception as e:
        return {"error": str(e)}


tool_functions = {
    "get_weather": get_weather,
    "calculate": calculate,
}


def extract_text(response) -> str:
    parts: list[str] = []
    for item in response:
        if isinstance(item, TextItem) and item.type == TextItemType.DEFAULT:
            parts.append(item.text)
        elif hasattr(item, "parts"):
            for part in item.parts:
                if isinstance(part, TextItem) and part.type == TextItemType.DEFAULT:
                    parts.append(part.text)
    return "".join(parts)


def process_tool_calls(session: ChatSession, response) -> str:
    while True:
        tool_calls = [item for item in response if isinstance(item, ToolCallItem)]
        if not tool_calls:
            return extract_text(response)

        with Request() as follow_up:
            follow_up.set_options(
                RequestOptions(
                    search=SearchOptions(max_output_tokens=512),
                    tool_choice=ToolChoice.AUTO,
                )
            )
            for tool_call in tool_calls:
                arguments = json.loads(tool_call.arguments)
                print(f"  Tool call: {tool_call.name}({arguments})")
                result = tool_functions[tool_call.name](**arguments)
                follow_up.add_item(ToolResultItem(tool_call.call_id, json.dumps(result)))

            with session.process_request(follow_up) as next_response:
                return process_tool_calls(session, next_response)


def main():
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
    model.download(
        lambda progress: print(
            f"\rDownloading model: {progress:.2f}%",
            end="",
            flush=True,
        )
    )
    print()
    model.load()
    print("Model loaded and ready.")

    try:
        with ChatSession(model) as session:
            session.set_options(
                RequestOptions(
                    search=SearchOptions(temperature=0.7, max_output_tokens=512),
                    tool_choice=ToolChoice.AUTO,
                )
            )
            session.add_tool_definition(
                "get_weather",
                "Get the current weather for a location",
                json.dumps(
                    {
                        "type": "object",
                        "properties": {
                            "location": {
                                "type": "string",
                                "description": "The city or location",
                            },
                            "unit": {
                                "type": "string",
                                "enum": ["celsius", "fahrenheit"],
                                "description": "Temperature unit",
                            },
                        },
                        "required": ["location"],
                    }
                ),
            )
            session.add_tool_definition(
                "calculate",
                "Perform a math calculation",
                json.dumps(
                    {
                        "type": "object",
                        "properties": {
                            "expression": {
                                "type": "string",
                                "description": "The math expression to evaluate",
                            }
                        },
                        "required": ["expression"],
                    }
                ),
            )

            messages = [
                MessageItem.system(
                    "You are a helpful assistant with access to tools. "
                    "Use them when needed to answer questions accurately."
                ),
                MessageItem.user("What is the weather in Seattle and what is 42 * 17?"),
            ]

            print("Sending request with tools...")
            with Request() as request:
                for message in messages:
                    request.add_item(message)
                with session.process_request(request) as response:
                    answer = process_tool_calls(session, response)
            print(f"\nAssistant: {answer}")
    finally:
        model.unload()
        print("Model unloaded.")


if __name__ == "__main__":
    main()
# </complete_code>
