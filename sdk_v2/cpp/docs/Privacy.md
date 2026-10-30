# Privacy

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may disable non-essential telemetry as described below. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

***

Foundry Local collects trace events with the goal of improving product quality. Official packages on supported platforms include the cross-platform 1DS telemetry SDK. Collection is handled following Microsoft's privacy practices.

Telemetry is enabled by default.

#### Technical Details

Foundry Local uses the cross-platform 1DS SDK (cpp_client_telemetry) to send trace events to Microsoft's telemetry backend over HTTPS. This data is handled following GDPR and privacy regulations for anonymity and data access controls.

Prompts, model outputs, audio contents, raw device identifiers, and secrets are not collected.

Foundry Local collects a lightweight startup baseline (OS, architecture, processor count and total system RAM),
coarse CPU/GPU/NPU and execution-provider availability, and SDK/service diagnostics such as model selection,
downloads, catalogs, cache activity and request timing/token counts. Detailed hardware specifications and inference
resource measurements belong to ONNX Runtime GenAI rather than a second collector in Foundry Local. Availability
of those diagnostics depends on the GenAI runtime version and its telemetry controls.

Foundry events are labeled `TelemetryLayer=foundry_local`; model usage timing is labeled
`TimingScope=foundry_request`. GenAI generation timings have a different scope and must not be added to Foundry
request timings as independent work. Existing Foundry request correlation IDs remain separate from GenAI's
model/generator IDs; the layer label does not establish a cross-layer request join.

Non-essential telemetry can be disabled as follows.

- **Disable via manager config.** Set the disable-nonessential-telemetry option before creating the manager:
  - C++: `Configuration::SetDisableNonessentialTelemetry(true)`
  - C#: `Configuration.DisableNonessentialTelemetry = true`
  - JavaScript/TypeScript: `disableNonessentialTelemetry: true`
  - Python: `disable_nonessential_telemetry=True`
  - Rust: `FoundryLocalConfig::new("my_app").additional_setting("DisableNonessentialTelemetry", "true")`
  - Native additional option: `DisableNonessentialTelemetry=true`
- **Disable via environment.** Set `ORT_TELEMETRY_DISABLED=1` before creating the manager.

The configuration opt-out also disables non-essential GenAI telemetry through its process-wide API. Foundry Local
never automatically re-enables GenAI telemetry, including when a later manager enables Foundry telemetry.
`ORT_TELEMETRY_DISABLED` retains its existing Foundry behavior: non-essential upload is disabled, but `ProcessInfo`
may still upload. Updated GenAI runtimes also recognize that variable as a full GenAI opt-out.

For a full opt-out in both libraries, set `ORT_DISABLE_TELEMETRY=1` before either library initializes.
Foundry Local then creates no telemetry uploader or persistent telemetry identifier and uploads no events,
including `ProcessInfo`; local diagnostic logging remains available. The canonical variable accepts `1`, `true`,
`yes`, `y`, or `on`, ignoring case and surrounding whitespace. Both libraries retain a full opt-out for the process
lifetime, even if the variable is later cleared; GenAI's runtime API cannot reverse it.
