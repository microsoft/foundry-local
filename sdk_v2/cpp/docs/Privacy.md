# Privacy

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may disable non-essential telemetry as described below. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

***

Foundry Local collects trace events with the goal of improving product quality. Official packages on supported platforms include the cross-platform 1DS telemetry SDK. Collection is handled following Microsoft's privacy practices.

Telemetry is enabled by default.

#### Technical Details

Foundry Local uses the cross-platform 1DS SDK (cpp_client_telemetry) to send trace events to Microsoft's telemetry backend over HTTPS. This data is handled following GDPR and privacy regulations for anonymity and data access controls.

Prompts, model outputs, audio contents, raw device identifiers, and secrets are not collected.

Non-essential telemetry can be disabled as follows.

- **Disable via manager config.** Set the disable-nonessential-telemetry option before creating the manager:
  - C++: `Configuration::SetDisableNonessentialTelemetry(true)`
  - C#: `Configuration.DisableNonessentialTelemetry = true`
  - JavaScript/TypeScript: `disableNonessentialTelemetry: true`
  - Python: `disable_nonessential_telemetry=True`
  - Native additional option: `DisableNonessentialTelemetry=true`
- **Disable via environment.** Set `ORT_TELEMETRY_DISABLED=1` before creating the manager.