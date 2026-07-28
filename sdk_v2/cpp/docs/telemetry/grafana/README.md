# Foundry Local Grafana dashboard

`foundry-local-telemetry-dashboard.json` is an importable Grafana dashboard for Foundry Local SDK v2 client
telemetry in Aria.

## Import

1. Install the Grafana Azure Data Explorer datasource plugin
   (`grafana-azure-data-explorer-datasource`) version `7.2.8`. The dashboard targets Grafana `11.6.11`; newer
   Grafana versions must also satisfy the plugin's compatibility requirements.
2. Configure a datasource that can query:
   - Cluster: `https://kusto.aria.microsoft.com`
   - Database: `9d5ddaec61e24567b788a20aea324631`
3. In Grafana, select **Dashboards > New > Import**, upload
   `foundry-local-telemetry-dashboard.json`, and map **Aria Azure Data Explorer** to that datasource.

The dashboard defaults to UTC, the last seven days, and a five-minute refresh. Aria retention is approximately
12–14 days. Every query requires a populated `FoundryLocalVersion`, excluding legacy/unversioned telemetry from this
SDK v2 dashboard.

## Filters

All dashboard filters are time-aware and multi-select:

- App
- OS
- Foundry Local version
- User agent
- Action
- Status
- Model
- Execution provider

The filters use the ADX plugin's `$__contains` macro. Their **All** value must remain exactly `all`.

## Privacy

The dashboard does not project device identifiers, client IP addresses, or user identity fields. Device IDs are used
only inside an aggregate `count_distinct` calculation. Recent-error panels show the telemetry error text already
subject to Foundry Local's upload redaction rules.

## Regenerate

The JSON is generated from `generate-dashboard.py`:

```powershell
python sdk_v2\cpp\docs\telemetry\grafana\generate-dashboard.py
```

The queries were authored against the live Aria schemas for:

`action`, `audiomodel`, `catalogfetch`, `download`, `epdownloadandregister`, `epdownloadattempt`, `error`,
`hardwareinfo`, `model`, `modelid`, `processinfo`, and `session`.
