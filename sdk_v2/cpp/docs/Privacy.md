# Privacy

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may disable non-essential telemetry as described below. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

***

Foundry Local collects trace events with the goal of improving product quality. Official packages on supported platforms include the cross-platform 1DS telemetry SDK. Collection is handled following Microsoft's privacy practices.

Telemetry is turned **ON** by default.

#### Technical Details

Foundry Local uses the cross-platform 1DS SDK (cpp_client_telemetry) to send trace events to Microsoft's telemetry backend over HTTPS. This data is handled following GDPR and privacy regulations for anonymity and data access controls.

Foundry Local sends one essential `ProcessInfo` event containing the host application name and version, Foundry Local version, process name, operating system name and version, CPU architecture and count, total memory, and container, virtual-machine, emulator, and device-ID status classifications. On Android and iOS, 1DS may use its platform-provided device identifier. On other supported platforms, Foundry Local sends a deterministic SHA-256 pseudonym of an installation identifier. Raw device identifiers are not included in Foundry Local event properties.

Non-essential events may include:

- SDK User-Agent, application session and request correlation identifiers, action status, elapsed time, and sanitized error diagnostics.
- Model identifier, execution provider, streaming mode, message and token counts, timing, and memory metrics.
- Audio source, supported language code, duration, sample rate, and channel count. Audio events use deterministic 1% sampling.
- Model-download status, timing, byte and file counts, cache reuse, wait result, and concurrency.
- Execution-provider download and registration status, provider name, readiness state, attempts, and timing.
- Catalog operation, built-in endpoint dimensions or `custom`, region, format, model count, status, and timing.
- Hardware device types and available execution-provider counts.

Prompt contents, model outputs, audio contents, raw device identifiers, and authentication secrets are not intentionally collected. Free-text diagnostics use the same path redaction and length limits as ONNX Runtime, and known secret-valued event properties are replaced before upload.

Non-essential telemetry can be disabled as follows. These controls do not disable the essential `ProcessInfo` event.

- **Disable via manager config.** Set the disable-nonessential-telemetry option before creating the manager:
  - C++: `Configuration::SetDisableNonessentialTelemetry(true)`
  - C#: `Configuration.DisableNonessentialTelemetry = true`
  - JavaScript/TypeScript: `disableNonessentialTelemetry: true`
  - Python: `disable_nonessential_telemetry=True`
  - Native additional option: `DisableNonessentialTelemetry=true`
- **Disable via environment.** Set `ORT_TELEMETRY_DISABLED=1` before creating the manager. For Foundry Local, this variable suppresses non-essential events but does not suppress `ProcessInfo`.
