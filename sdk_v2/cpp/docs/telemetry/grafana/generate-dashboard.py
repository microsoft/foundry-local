#!/usr/bin/env python3
"""Generate the importable Foundry Local Aria telemetry Grafana dashboard."""

from __future__ import annotations

import json
from pathlib import Path


PLUGIN_ID = "grafana-azure-data-explorer-datasource"
PLUGIN_VERSION = "7.2.8"
DATASOURCE_UID = "${DS_ARIA_ADX}"
CLUSTER_URI = "https://kusto.aria.microsoft.com"
DATABASE = "9d5ddaec61e24567b788a20aea324631"
OUTPUT = Path(__file__).with_name("foundry-local-telemetry-dashboard.json")

EMPTY_EXPRESSION = {
    "where": {"type": "and", "expressions": []},
    "groupBy": {"type": "and", "expressions": []},
    "reduce": {"type": "and", "expressions": []},
}

PROCESS_DIMENSIONS = """
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName),
         FLOS=coalesce(OsName, osName, os),
         FLVersion=coalesce(FoundryLocalVersion, Version, libraryVersion)
""".strip()

ACTION_DIMENSIONS = """
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(tostring(column_ifexists("AppName", "")),
                        tostring(column_ifexists("appName", ""))),
         FLOS=coalesce(tostring(column_ifexists("OsName", "")),
                       tostring(column_ifexists("hostOS", "")),
                       tostring(column_ifexists("os_type", ""))),
         FLVersion=coalesce(tostring(column_ifexists("FoundryLocalVersion", "")),
                            tostring(column_ifexists("Version", "")),
                            tostring(column_ifexists("AppVersion", ""))),
         FLUserAgent=coalesce(tostring(column_ifexists("UserAgent", "")),
                              tostring(column_ifexists("userAgent", ""))),
         FLAction=coalesce(tostring(column_ifexists("Action", "")),
                           tostring(column_ifexists("action", ""))),
         FLStatus=coalesce(tostring(column_ifexists("Status", "")),
                           tostring(column_ifexists("status", "")))
""".strip()

MODEL_DIMENSIONS = """
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName),
         FLOS=OsName,
         FLVersion=coalesce(FoundryLocalVersion, Version, AppVersion),
         FLUserAgent=coalesce(UserAgent, userAgent),
         FLModel=coalesce(ModelId, modelId),
         FLEP=coalesce(ExecutionProvider, executionProvider)
""".strip()

DOWNLOAD_DIMENSIONS = """
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName),
         FLOS=coalesce(OsName, hostOS, os_type),
         FLVersion=coalesce(FoundryLocalVersion, Version, AppVersion),
         FLUserAgent=coalesce(UserAgent, userAgent),
         FLModel=coalesce(ModelId, modelId),
         FLStatus=coalesce(Status, status)
""".strip()

ACTION_FILTERS = """
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLAction, $fl_action)
| where $__contains(FLStatus, $fl_status)
""".strip()

MODEL_FILTERS = """
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLModel, $fl_model)
| where $__contains(FLEP, $fl_ep)
""".strip()

PROCESS_FILTERS = """
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
""".strip()


def datasource() -> dict[str, str]:
    return {"type": PLUGIN_ID, "uid": DATASOURCE_UID}


def target(query: str, result_format: str = "table", ref_id: str = "A") -> dict[str, object]:
    return {
        "clusterUri": CLUSTER_URI,
        "database": DATABASE,
        "datasource": datasource(),
        "expression": EMPTY_EXPRESSION,
        "pluginVersion": PLUGIN_VERSION,
        "query": query.strip(),
        "querySource": "raw",
        "queryType": "KQL",
        "rawMode": True,
        "refId": ref_id,
        "resultFormat": result_format,
    }


def thresholds(red_at: float | None = None, green_at: float | None = None) -> dict[str, object]:
    steps: list[dict[str, object]] = [{"color": "green", "value": None}]
    if red_at is not None:
        steps.append({"color": "red", "value": red_at})
    if green_at is not None:
        steps.append({"color": "green", "value": green_at})
    return {"mode": "absolute", "steps": steps}


def panel(
    panel_id: int,
    title: str,
    panel_type: str,
    x: int,
    y: int,
    w: int,
    h: int,
    query: str,
    *,
    result_format: str = "table",
    description: str = "",
    unit: str = "short",
    decimals: int | None = None,
    panel_options: dict[str, object] | None = None,
    panel_thresholds: dict[str, object] | None = None,
) -> dict[str, object]:
    defaults: dict[str, object] = {
        "color": {"mode": "thresholds" if panel_type == "stat" else "palette-classic"},
        "mappings": [],
        "thresholds": panel_thresholds or thresholds(),
        "unit": unit,
    }
    if decimals is not None:
        defaults["decimals"] = decimals

    options: dict[str, object]
    if panel_type == "stat":
        options = {
            "colorMode": "value",
            "graphMode": "area",
            "justifyMode": "auto",
            "orientation": "auto",
            "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
            "textMode": "auto",
            "wideLayout": True,
        }
    elif panel_type == "timeseries":
        options = {
            "legend": {"calcs": [], "displayMode": "list", "placement": "bottom", "showLegend": True},
            "tooltip": {"mode": "multi", "sort": "desc"},
        }
    elif panel_type == "barchart":
        options = {
            "barRadius": 0,
            "barWidth": 0.8,
            "fullHighlight": False,
            "groupWidth": 0.7,
            "legend": {"calcs": [], "displayMode": "list", "placement": "bottom", "showLegend": True},
            "orientation": "horizontal",
            "showValue": "auto",
            "stacking": "none",
            "tooltip": {"mode": "single", "sort": "none"},
            "xTickLabelRotation": 0,
            "xTickLabelSpacing": 0,
        }
    else:
        options = {
            "cellHeight": "sm",
            "footer": {"countRows": False, "fields": "", "reducer": ["sum"], "show": False},
            "showHeader": True,
        }
    if panel_options:
        options.update(panel_options)

    return {
        "datasource": datasource(),
        "description": description,
        "fieldConfig": {"defaults": defaults, "overrides": []},
        "gridPos": {"h": h, "w": w, "x": x, "y": y},
        "id": panel_id,
        "options": options,
        "targets": [target(query, result_format)],
        "title": title,
        "type": panel_type,
    }


def row(panel_id: int, title: str, y: int) -> dict[str, object]:
    return {
        "collapsed": False,
        "gridPos": {"h": 1, "w": 24, "x": 0, "y": y},
        "id": panel_id,
        "panels": [],
        "title": title,
        "type": "row",
    }


def variable(name: str, label: str, query: str, *, description: str = "") -> dict[str, object]:
    query_target = target(query, "table", "StandardVariableQuery")
    return {
        "allValue": "all",
        "current": {"selected": True, "text": ["All"], "value": ["all"]},
        "datasource": datasource(),
        "definition": query.strip(),
        "description": description,
        "hide": 0,
        "includeAll": True,
        "label": label,
        "multi": True,
        "name": name,
        "options": [],
        "query": query_target,
        "refresh": 2,
        "regex": "",
        "skipUrlSync": False,
        "sort": 1,
        "type": "query",
    }


def build_panels() -> list[dict[str, object]]:
    panels: list[dict[str, object]] = []
    panels.append(row(1, "Overview", 0))
    panels.append(
        {
            "gridPos": {"h": 3, "w": 24, "x": 0, "y": 1},
            "id": 2,
            "options": {
                "content": (
                    "Foundry Local SDK v2 client telemetry from Aria. Filters are multi-select and time-aware. "
                    "Device IDs are used only for aggregate distinct counts; this dashboard never displays device, "
                    "client-IP, or user-identity values. Aria retention is approximately 12–14 days."
                ),
                "mode": "markdown",
            },
            "title": "Dashboard scope",
            "type": "text",
        }
    )
    panels.extend(
        [
            panel(
                3,
                "Process sessions",
                "stat",
                0,
                4,
                4,
                4,
                f"""
processinfo
| where $__timeFilter(EventInfo_Time)
{PROCESS_DIMENSIONS}
{PROCESS_FILTERS}
| where isnotempty(AppSessionGuid)
| summarize Value=count_distinct(AppSessionGuid)
""",
                description="Exact distinct process-wide application sessions represented by ProcessInfo.",
            ),
            panel(
                4,
                "Distinct devices",
                "stat",
                4,
                4,
                4,
                4,
                f"""
processinfo
| where $__timeFilter(EventInfo_Time)
{PROCESS_DIMENSIONS}
{PROCESS_FILTERS}
| where isnotempty(DeviceInfo_Id)
| summarize Value=count_distinct(DeviceInfo_Id)
""",
                description="Aggregate exact distinct device count; device identifiers are never projected.",
            ),
            panel(
                5,
                "Action success rate",
                "stat",
                8,
                4,
                4,
                4,
                f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
{ACTION_FILTERS}
| summarize Total=count(), Healthy=countif(FLStatus in ("Success", "Skipped"))
| where Total > 0
| project Value=100.0 * todouble(Healthy) / todouble(Total)
""",
                unit="percent",
                decimals=1,
                panel_thresholds={
                    "mode": "absolute",
                    "steps": [
                        {"color": "red", "value": None},
                        {"color": "yellow", "value": 95},
                        {"color": "green", "value": 99},
                    ],
                },
            ),
            panel(
                6,
                "Errors",
                "stat",
                12,
                4,
                4,
                4,
                """
error
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName),
         FLOS=OsName,
         FLVersion=coalesce(FoundryLocalVersion, AppVersion),
         FLUserAgent=UserAgent,
         FLAction=coalesce(Action, action)
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLAction, $fl_action)
| summarize Value=count()
""",
                panel_thresholds=thresholds(red_at=1),
            ),
            panel(
                7,
                "Inference calls",
                "stat",
                16,
                4,
                4,
                4,
                f"""
model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
{MODEL_FILTERS}
| summarize Value=count()
""",
            ),
            panel(
                8,
                "Transferred bytes",
                "stat",
                20,
                4,
                4,
                4,
                f"""
download
| where $__timeFilter(EventInfo_Time)
{DOWNLOAD_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLModel, $fl_model)
| where $__contains(FLStatus, $fl_status)
| where FLStatus == "Success"
| extend TransferredBytes=iff(TotalSizeBytes > AlreadyCachedBytes,
                              TotalSizeBytes - AlreadyCachedBytes,
                              long(0))
| summarize Value=sum(TransferredBytes)
""",
                description="Bytes transferred by successful downloads, excluding bytes already present in cache.",
                unit="bytes",
            ),
        ]
    )

    panels.append(row(9, "Adoption and environment", 8))
    panels.extend(
        [
            panel(
                10,
                "Process sessions by OS",
                "timeseries",
                0,
                9,
                12,
                8,
                f"""
processinfo
| where $__timeFilter(EventInfo_Time)
{PROCESS_DIMENSIONS}
{PROCESS_FILTERS}
| where isnotempty(AppSessionGuid)
| summarize Value=count_distinct(AppSessionGuid) by Time=bin(EventInfo_Time, $__interval), Series=FLOS
| order by Time asc
""",
                result_format="time_series",
            ),
            panel(
                11,
                "App / SDK version adoption",
                "table",
                12,
                9,
                12,
                8,
                f"""
processinfo
| where $__timeFilter(EventInfo_Time)
{PROCESS_DIMENSIONS}
{PROCESS_FILTERS}
| summarize Sessions=count_distinct(AppSessionGuid) by App=FLApp, Version=FLVersion, OS=FLOS
| top 50 by Sessions desc
""",
            ),
            panel(
                12,
                "User-agent mix",
                "table",
                0,
                17,
                12,
                8,
                f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
{ACTION_FILTERS}
| summarize Events=count(), Sessions=count_distinct(AppSessionGuid)
  by App=FLApp, UserAgent=FLUserAgent
| top 50 by Events desc
""",
            ),
            panel(
                13,
                "Hardware capability mix",
                "table",
                12,
                17,
                12,
                8,
                """
hardwareinfo
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| summarize Sessions=count_distinct(AppSessionGuid)
  by HasGPU, HasNPU, DeviceTypes, ExecutionProviders
| order by Sessions desc
""",
            ),
        ]
    )

    panels.append(row(14, "Reliability and latency", 25))
    panels.extend(
        [
            panel(
                15,
                "Action status over time",
                "timeseries",
                0,
                26,
                12,
                8,
                f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
{ACTION_FILTERS}
| summarize Value=count() by Time=bin(EventInfo_Time, $__interval), Series=FLStatus
| order by Time asc
""",
                result_format="time_series",
            ),
            panel(
                16,
                "Failure rate by action",
                "barchart",
                12,
                26,
                12,
                8,
                f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
{ACTION_FILTERS}
| summarize Total=count(),
            Failures=countif(FLStatus !in ("Success", "Skipped"))
  by Action=FLAction
| extend FailureRate=100.0 * todouble(Failures) / todouble(Total)
| top 15 by FailureRate desc
| project Action, FailureRate
""",
                unit="percent",
                decimals=1,
            ),
            panel(
                17,
                "P95 action latency",
                "barchart",
                0,
                34,
                12,
                8,
                f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
{ACTION_FILTERS}
| where TimeMs >= 0
| summarize P95=percentile(TimeMs, 95) by Action=FLAction
| top 15 by P95 desc
""",
                unit="ms",
            ),
            panel(
                18,
                "Recent errors",
                "table",
                12,
                34,
                12,
                8,
                """
error
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName),
         FLOS=OsName,
         FLVersion=coalesce(FoundryLocalVersion, AppVersion),
         FLUserAgent=UserAgent,
         FLAction=coalesce(Action, action)
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLAction, $fl_action)
| project Time=EventInfo_Time, Action=FLAction, ExceptionType, ExceptionMessage,
          App=FLApp, Version=FLVersion, UserAgent=FLUserAgent, CorrelationId
| top 100 by Time desc
""",
            ),
        ]
    )

    panels.append(row(19, "Inference and model usage", 42))
    panels.extend(
        [
            panel(
                20,
                "Inference volume by execution provider",
                "timeseries",
                0,
                43,
                12,
                8,
                f"""
model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
{MODEL_FILTERS}
| summarize Value=count() by Time=bin(EventInfo_Time, $__interval), Series=FLEP
| order by Time asc
""",
                result_format="time_series",
            ),
            panel(
                21,
                "Model performance",
                "table",
                12,
                43,
                12,
                8,
                f"""
model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
{MODEL_FILTERS}
| summarize Calls=count(), P50TotalMs=percentile(TotalTimeMs, 50),
            P95TotalMs=percentile(TotalTimeMs, 95), Tokens=sum(TotalTokens)
  by Model=FLModel, EP=FLEP, Stream
| top 100 by Calls desc
""",
            ),
            panel(
                22,
                "P95 inference latency",
                "timeseries",
                0,
                51,
                12,
                8,
                f"""
model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
{MODEL_FILTERS}
| where TotalTimeMs >= 0
| summarize Value=percentile(TotalTimeMs, 95)
  by Time=bin(EventInfo_Time, $__interval), Series=FLEP
| order by Time asc
""",
                result_format="time_series",
                unit="ms",
            ),
            panel(
                23,
                "Token volume by model",
                "barchart",
                12,
                51,
                12,
                8,
                f"""
model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
{MODEL_FILTERS}
| summarize Tokens=sum(TotalTokens) by Model=FLModel
| top 15 by Tokens desc
""",
            ),
            panel(
                24,
                "Audio usage",
                "table",
                0,
                59,
                24,
                7,
                """
audiomodel
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion,
         FLUserAgent=UserAgent, FLModel=ModelId, FLEP=ExecutionProvider
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLModel, $fl_model)
| where $__contains(FLEP, $fl_ep)
| summarize Calls=count(),
            P95LatencyMs=percentile(TotalTimeMs, 95),
            TotalTokens=sum(TotalTokens),
            AvgAudioDurationMs=avgif(AudioDurationMs, AudioDurationMs >= 0)
  by Model=FLModel, EP=FLEP, AudioSource, Language, Stream
| top 100 by Calls desc
""",
            ),
        ]
    )

    panels.append(row(25, "Downloads, catalog, and execution providers", 66))
    panels.extend(
        [
            panel(
                26,
                "Download outcomes",
                "timeseries",
                0,
                67,
                12,
                8,
                f"""
download
| where $__timeFilter(EventInfo_Time)
{DOWNLOAD_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLModel, $fl_model)
| where $__contains(FLStatus, $fl_status)
| summarize Value=count() by Time=bin(EventInfo_Time, $__interval), Series=FLStatus
| order by Time asc
""",
                result_format="time_series",
            ),
            panel(
                27,
                "Download performance",
                "table",
                12,
                67,
                12,
                8,
                f"""
download
| where $__timeFilter(EventInfo_Time)
{DOWNLOAD_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLModel, $fl_model)
| where $__contains(FLStatus, $fl_status)
| summarize Attempts=count(),
            P50DownloadMs=percentile(DownloadTimeMs, 50),
            P95DownloadMs=percentile(DownloadTimeMs, 95),
            P95LockWaitMs=percentile(LockWaitTimeMs, 95),
            TransferredBytes=sum(iff(FLStatus == "Success" and TotalSizeBytes > AlreadyCachedBytes,
                                     TotalSizeBytes - AlreadyCachedBytes,
                                     long(0))),
            CachedBytes=sum(AlreadyCachedBytes)
  by Model=FLModel, Status=FLStatus, WaitResult=DownloadWaitResult
| top 100 by Attempts desc
""",
            ),
            panel(
                28,
                "Catalog fetch health",
                "table",
                0,
                75,
                12,
                8,
                """
catalogfetch
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion,
         FLUserAgent=UserAgent, FLStatus=Status
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLStatus, $fl_status)
| summarize Calls=count(),
            Failures=countif(FLStatus !in ("Success", "Skipped")),
            P95LatencyMs=percentile(TimeMs, 95),
            AvgModels=avg(ModelCount)
  by Operation, Endpoint, Region, Status=FLStatus
| top 100 by Calls desc
""",
            ),
            panel(
                29,
                "Catalog P95 latency",
                "timeseries",
                12,
                75,
                12,
                8,
                """
catalogfetch
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion,
         FLUserAgent=UserAgent, FLStatus=Status
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLStatus, $fl_status)
| summarize Value=percentile(TimeMs, 95)
  by Time=bin(EventInfo_Time, $__interval), Series=Operation
| order by Time asc
""",
                result_format="time_series",
                unit="ms",
            ),
            panel(
                30,
                "EP attempt outcomes",
                "timeseries",
                0,
                83,
                12,
                8,
                """
epdownloadattempt
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion,
         FLUserAgent=UserAgent, FLStatus=Status
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLStatus, $fl_status)
| summarize Value=count() by Time=bin(EventInfo_Time, $__interval), Series=FLStatus
| order by Time asc
""",
                result_format="time_series",
            ),
            panel(
                31,
                "EP provider phase performance",
                "table",
                12,
                83,
                12,
                8,
                """
epdownloadandregister
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion, FLUserAgent=UserAgent
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| summarize Attempts=count(),
            DownloadFailures=countif(DownloadStatus !in ("Success", "Skipped")),
            RegisterFailures=countif(RegisterStatus !in ("Success", "Skipped")),
            P95DownloadMs=percentile(DownloadTimeMs, 95),
            P95RegisterMs=percentile(RegisterTimeMs, 95)
  by ProviderName, InitReadyState, DownloadReadyState, RegisterReadyState
| top 100 by Attempts desc
""",
            ),
        ]
    )

    panels.append(row(32, "Failure drilldown", 91))
    panels.extend(
        [
            panel(
                33,
                "Recent failed actions",
                "table",
                0,
                92,
                12,
                8,
                f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
{ACTION_FILTERS}
| where FLStatus !in ("Success", "Skipped")
| project Time=EventInfo_Time, Action=FLAction, Status=FLStatus, TimeMs,
          App=FLApp, Version=FLVersion, OS=FLOS, UserAgent=FLUserAgent, CorrelationId
| top 100 by Time desc
""",
            ),
            panel(
                34,
                "Recent catalog failures",
                "table",
                12,
                92,
                12,
                8,
                """
catalogfetch
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=AppName, FLOS=OsName, FLVersion=FoundryLocalVersion,
         FLUserAgent=UserAgent, FLStatus=Status
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where $__contains(FLStatus, $fl_status)
| where FLStatus !in ("Success", "Skipped")
| project Time=EventInfo_Time, Operation, Endpoint, Region, Status=FLStatus,
          TimeMs, ErrorMessage, CorrelationId
| top 100 by Time desc
""",
            ),
        ]
    )
    return panels


def build_variables() -> list[dict[str, object]]:
    return [
        variable(
            "fl_app",
            "App",
            """
processinfo
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend Value=coalesce(AppName, appName)
| where isnotempty(Value)
| distinct Value
| order by Value asc
| project __text=Value, __value=Value
""",
        ),
        variable(
            "fl_os",
            "OS",
            """
processinfo
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName), Value=coalesce(OsName, osName, os)
| where $__contains(FLApp, $fl_app)
| where isnotempty(Value)
| distinct Value
| order by Value asc
| project __text=Value, __value=Value
""",
        ),
        variable(
            "fl_version",
            "Foundry Local version",
            """
processinfo
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(AppName, appName), FLOS=coalesce(OsName, osName, os),
         Value=FoundryLocalVersion
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where isnotempty(Value)
| distinct Value
| order by Value desc
| project __text=Value, __value=Value
""",
        ),
        variable(
            "fl_user_agent",
            "User agent",
            f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where isnotempty(FLUserAgent)
| distinct FLUserAgent
| order by FLUserAgent asc
| project __text=FLUserAgent, __value=FLUserAgent
""",
        ),
        variable(
            "fl_action",
            "Action",
            f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where isnotempty(FLAction)
| distinct FLAction
| order by FLAction asc
| project __text=FLAction, __value=FLAction
""",
        ),
        variable(
            "fl_status",
            "Status",
            f"""
action
| where $__timeFilter(EventInfo_Time)
{ACTION_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where isnotempty(FLStatus)
| distinct FLStatus
| order by FLStatus asc
| project __text=FLStatus, __value=FLStatus
""",
        ),
        variable(
            "fl_model",
            "Model",
            f"""
let InferenceModels = model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
| project FLApp, FLOS, FLVersion, FLUserAgent, FLModel;
let DownloadModels = download
| where $__timeFilter(EventInfo_Time)
{DOWNLOAD_DIMENSIONS}
| project FLApp, FLOS, FLVersion, FLUserAgent, FLModel;
let ActionModels = modelid
| where $__timeFilter(EventInfo_Time)
| where isnotempty(FoundryLocalVersion)
| extend FLApp=coalesce(tostring(column_ifexists("AppName", "")),
                        tostring(column_ifexists("appName", ""))),
         FLOS=coalesce(tostring(column_ifexists("OsName", "")),
                       tostring(column_ifexists("hostOS", "")),
                       tostring(column_ifexists("os_type", ""))),
         FLVersion=coalesce(tostring(column_ifexists("FoundryLocalVersion", "")),
                            tostring(column_ifexists("Version", "")),
                            tostring(column_ifexists("AppVersion", ""))),
         FLUserAgent=coalesce(tostring(column_ifexists("UserAgent", "")),
                              tostring(column_ifexists("userAgent", ""))),
         FLModel=coalesce(tostring(column_ifexists("ModelId", "")),
                          tostring(column_ifexists("modelId", "")))
| project FLApp, FLOS, FLVersion, FLUserAgent, FLModel;
union InferenceModels, DownloadModels, ActionModels
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where isnotempty(FLModel)
| distinct FLModel
| order by FLModel asc
| project __text=FLModel, __value=FLModel
""",
        ),
        variable(
            "fl_ep",
            "Execution provider",
            f"""
model
| where $__timeFilter(EventInfo_Time)
{MODEL_DIMENSIONS}
| where $__contains(FLApp, $fl_app)
| where $__contains(FLOS, $fl_os)
| where $__contains(FLVersion, $fl_version)
| where $__contains(FLUserAgent, $fl_user_agent)
| where isnotempty(FLEP)
| distinct FLEP
| order by FLEP asc
| project __text=FLEP, __value=FLEP
""",
        ),
    ]


def build_dashboard() -> dict[str, object]:
    return {
        "__inputs": [
            {
                "description": "Azure Data Explorer datasource configured for the Foundry Local Aria cluster.",
                "label": "Aria Azure Data Explorer",
                "name": "DS_ARIA_ADX",
                "pluginId": PLUGIN_ID,
                "pluginName": "Azure Data Explorer",
                "type": "datasource",
            }
        ],
        "__requires": [
            {"type": "grafana", "id": "grafana", "name": "Grafana", "version": "10.4.0"},
            {"type": "datasource", "id": PLUGIN_ID, "name": "Azure Data Explorer", "version": PLUGIN_VERSION},
            {"type": "panel", "id": "stat", "name": "Stat", "version": ""},
            {"type": "panel", "id": "timeseries", "name": "Time series", "version": ""},
            {"type": "panel", "id": "table", "name": "Table", "version": ""},
            {"type": "panel", "id": "barchart", "name": "Bar chart", "version": ""},
            {"type": "panel", "id": "text", "name": "Text", "version": ""},
        ],
        "annotations": {
            "list": [
                {
                    "builtIn": 1,
                    "datasource": {"type": "grafana", "uid": "-- Grafana --"},
                    "enable": True,
                    "hide": True,
                    "iconColor": "rgba(0, 211, 255, 1)",
                    "name": "Annotations & Alerts",
                    "type": "dashboard",
                }
            ]
        },
        "editable": True,
        "fiscalYearStartMonth": 0,
        "graphTooltip": 1,
        "id": None,
        "links": [],
        "liveNow": False,
        "panels": build_panels(),
        "refresh": "5m",
        "schemaVersion": 39,
        "tags": ["foundry-local", "sdk-v2", "telemetry", "aria", "adx"],
        "templating": {"list": build_variables()},
        "time": {"from": "now-7d", "to": "now"},
        "timepicker": {},
        "timezone": "utc",
        "title": "Foundry Local SDK v2 Telemetry",
        "uid": "foundry-local-sdk-v2-telemetry",
        "version": 1,
        "weekStart": "",
    }


def main() -> None:
    OUTPUT.write_text(json.dumps(build_dashboard(), indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
