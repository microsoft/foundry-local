# Privacy

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may disable non-essential telemetry as described below. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

***

Foundry Local collects a small number of trace events with the goal of improving product quality. Official packages on supported platforms include the cross-platform 1DS telemetry SDK. Collection is subject to user consent and handled following Microsoft's privacy practices.

Telemetry is turned **ON** by default.

#### Technical Details

Foundry Local uses the cross-platform 1DS SDK (cpp_client_telemetry) to send trace events to Microsoft's telemetry backend over HTTPS. Based on user consent, this data is handled following GDPR and privacy regulations for anonymity and data access controls.

Non-essential telemetry can be disabled as follows. Foundry Local may still send a minimal ProcessInfo event.

- **Disable via manager config.** Set the disable-nonessential-telemetry option before creating the manager:
  - C++: `Configuration::SetDisableNonessentialTelemetry(true)`
  - C#: `Configuration.DisableNonessentialTelemetry = true`
  - JavaScript/TypeScript: `disableNonessentialTelemetry: true`
  - Python: `disable_nonessential_telemetry=True`
  - Native additional option: `DisableNonessentialTelemetry=true`

#### Collected events

| Event | Purpose |
| --- | --- |
| `ProcessInfo` | Startup application, operating-system, architecture, process, and coarse container/VM metadata |
| `Action`, `Error` | Operation timing, outcome, resolved model ID, and redacted diagnostic errors |
| `Session` | Embedded web-service usage-session start and end |
| `Model`, `AudioModel` | Inference timing, token counts, execution provider, and audio-format metrics |
| `Download` | Model download timing, byte/file counts, cache hits, and outcome |
| `CatalogFetch` | Live catalog refreshes and cached-model lookups, including timing and outcome |
| `EPDownloadAttempt`, `EPDownloadAndRegister` | Execution-provider download and registration outcomes |
| `HardwareInfo` | Coarse CPU/GPU/NPU and execution-provider availability at startup |

HTTP operations propagate a correlation ID to their nested inference events. SDK calls identify the calling language
through a versioned user agent. Model IDs remain fields on `Action`; there is no separate `ModelId` event, and locale
is not collected. General event families retain 100%. High-volume `OpenAIAudioTranscribe` and `AudioModel` events
retain 0.1% as a correlated pair, selected deterministically by operation correlation ID.

Telemetry strings are redacted at the final emission boundary, including strings in arrays and structured values.
Each string is capped at 40,960 UTF-8 bytes without splitting a character or a redaction marker.
