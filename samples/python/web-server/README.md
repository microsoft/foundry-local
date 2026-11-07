# Web server

Loads a model, starts the built-in Foundry Local web server, discovers its
bound URL, and sends a request to the OpenAI-compatible chat endpoint. The
sample then stops the server and unloads the model.

```bash
python -m pip install -r requirements.txt
python src/app.py
```
