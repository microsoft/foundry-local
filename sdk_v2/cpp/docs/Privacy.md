# Privacy

## Data Collection

The software may collect information about you and your use of the software and send it to Microsoft. Microsoft may use this information to provide services and improve our products and services. You may disable non-essential telemetry as described below. There are also some features in the software that may enable you and Microsoft to collect data from users of your applications. If you use these features, you must comply with applicable law, including providing appropriate notices to users of your applications together with a copy of Microsoft's privacy statement. Our privacy statement is located at https://go.microsoft.com/fwlink/?LinkID=824704. You can learn more about data collection and use in the help documentation and our privacy statement. Your use of the software operates as your consent to these practices.

***

Foundry Local collects a small number of trace events with the goal of improving product quality. Official packages on supported platforms include the cross-platform 1DS telemetry SDK. Collection is subject to user consent and handled following Microsoft's privacy practices.

Telemetry is turned **ON** by default.

#### Technical Details

Foundry Local uses the cross-platform 1DS SDK (cpp_client_telemetry) to send trace events to Microsoft's telemetry backend over HTTPS. Based on user consent, this data is handled following GDPR and privacy regulations for anonymity and data access controls.

All telemetry uploads can be disabled by setting `ORT_DISABLE_TELEMETRY=1` before creating a Foundry Local manager.
The legacy `ORT_TELEMETRY_DISABLED` name is also accepted. Telemetry events may still be written to the application's
local diagnostic logger.

Non-essential telemetry can be disabled as follows. Foundry Local may still send a minimal ProcessInfo event.

- **Disable via manager config.** Set the disable-nonessential-telemetry option before creating the manager:
  - C++: `Configuration::SetDisableNonessentialTelemetry(true)`
  - C#: `Configuration.DisableNonessentialTelemetry = true`
  - JavaScript/TypeScript: `disableNonessentialTelemetry: true`
  - Python: `disable_nonessential_telemetry=True`
  - Native additional option: `DisableNonessentialTelemetry=true`
