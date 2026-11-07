# Tool calling

Registers a JSON-schema function tool on a native `ChatSession`, handles the
model's `ToolCallItem` for Paris, and returns a mock weather `ToolResultItem`
for the final response with tool calling disabled.
The sample selects `qwen2.5-0.5b` and verifies that the selected catalog
variant explicitly reports tool-calling support. This keeps the download small
and uses a model whose current variants publish the metadata required by
structured tool calling.

```bash
python -m pip install -r requirements.txt
python src/app.py
```
