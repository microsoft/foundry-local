# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import re
from urllib.parse import urlparse

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.logging_helper import LogLevel

# Maps Python LogLevel → native flLogLevel integer values (from foundry_local_c.h)
_LOG_LEVEL_MAP: dict[LogLevel, int] = {
    LogLevel.VERBOSE: 0,      # FOUNDRY_LOCAL_LOG_VERBOSE
    LogLevel.DEBUG: 1,        # FOUNDRY_LOCAL_LOG_DEBUG
    LogLevel.INFORMATION: 2,  # FOUNDRY_LOCAL_LOG_INFO
    LogLevel.WARNING: 3,      # FOUNDRY_LOCAL_LOG_WARNING
    LogLevel.ERROR: 4,        # FOUNDRY_LOCAL_LOG_ERROR
    LogLevel.FATAL: 5,        # FOUNDRY_LOCAL_LOG_FATAL
}

# Regex for validating app_name (safe filesystem characters only)
_SAFE_APP_NAME = re.compile(r"^[A-Za-z0-9._-]+$")


class Configuration:
    """Configuration for Foundry Local SDK.

    Configuration values:
        app_name: Your application name. MUST be set to a valid name.
        foundry_local_core_path: Path to the Foundry Local Core native library.
            Accepted for v1 API compatibility only and has no runtime effect
            in v2. The native library and its ORT/GenAI dependencies are
            resolved at package import time, before any ``Configuration`` is
            constructed, so a per-instance value cannot influence loading.
            To override the library location in v2, set the
            ``FOUNDRY_LOCAL_LIB_DIR`` environment variable before importing
            ``foundry_local_sdk``.
        app_data_dir: Application data directory.
        model_cache_dir: Model cache directory.
        logs_dir: Log directory.
        log_level: Logging level. Default: LogLevel.WARNING.
        web: Optional configuration for the built-in web service.
        additional_settings: Additional settings that Foundry Local Core can consume.
        catalog_urls: Catalog URLs with optional per-catalog filter overrides.
            Each entry is a ``(url, filter)`` tuple where filter may be ``None``.
            Defaults to the Azure Foundry Local Catalog when empty or ``None``.
        catalog_region: Region hint forwarded to the catalog service
        disable_nonessential_telemetry: When True, disable non-essential telemetry. Foundry
            Local may still send a minimal ProcessInfo event. Defaults to False.
        external_service_url: URL of an external Foundry Local service. When
            set, the catalog operates in cache-only mode — it reads only the
            local disk cache populated by that external service and skips
            network and local-model scans. Reserved for future delegation of
            model load/unload to the external service.
    """

    class WebService:
        """Configuration settings if the optional web service is used."""

        def __init__(
            self,
            urls: str | None = None,
            external_url: str | None = None,
        ) -> None:
            """Initialize WebService configuration.

            Args:
                urls: URL(s) to bind to the web service.  Multiple URLs can be
                    specified as a semicolon-separated list.
                external_url: URI of a web service running in a separate
                    process. When set, the catalog operates in cache-only mode
                    against the cache populated by that service.
            """
            self.urls = urls
            self.external_url = external_url

    def __init__(
        self,
        app_name: str,
        foundry_local_core_path: str | None = None,
        app_data_dir: str | None = None,
        model_cache_dir: str | None = None,
        logs_dir: str | None = None,
        log_level: LogLevel | None = LogLevel.WARNING,
        web: Configuration.WebService | None = None,
        additional_settings: dict[str, str] | None = None,
        catalog_urls: list[tuple[str, str | None]] | None = None,
        catalog_region: str | None = None,
        disable_nonessential_telemetry: bool = False,
    ) -> None:
        self.app_name = app_name
        # v1-compat no-op: native loading happens at import time in
        # _native/lib_loader.py, driven by FOUNDRY_LOCAL_LIB_DIR.
        self.foundry_local_core_path = foundry_local_core_path
        self.app_data_dir = app_data_dir
        self.model_cache_dir = model_cache_dir
        self.logs_dir = logs_dir
        self.log_level = log_level
        self.web = web
        self.additional_settings = additional_settings
        self.catalog_urls = catalog_urls
        self.catalog_region = catalog_region
        self.disable_nonessential_telemetry = disable_nonessential_telemetry

    def validate(self) -> None:
        """Validate the configuration.

        Raises:
            FoundryLocalException: If the configuration is invalid.
        """
        if not self.app_name:
            raise FoundryLocalException(
                "Configuration AppName must be set to a valid application name."
            )

        if not bool(_SAFE_APP_NAME.match(self.app_name)):
            raise FoundryLocalException(
                "Configuration AppName value contains invalid characters."
            )

        if self.web is not None and self.web.external_url is not None:
            parsed = urlparse(self.web.external_url)
            if not parsed.port or parsed.port == 0:
                raise FoundryLocalException(
                    "Configuration Web.ExternalUrl has invalid port."
                )

    def as_dictionary(self) -> dict[str, str]:
        """Convert configuration to a dictionary of string key-value pairs.

        Returns:
            Dictionary containing configuration values as strings.

        Raises:
            FoundryLocalException: If AppName is not set to a valid value.
        """
        if not self.app_name:
            raise FoundryLocalException(
                "Configuration AppName must be set to a valid application name."
            )

        config_values: dict[str, str] = {
            "AppName": self.app_name,
            "LogLevel": str(self.log_level),
        }

        if self.app_data_dir:
            config_values["AppDataDir"] = self.app_data_dir

        if self.model_cache_dir:
            config_values["ModelCacheDir"] = self.model_cache_dir

        if self.logs_dir:
            config_values["LogsDir"] = self.logs_dir

        if self.web is not None:
            if self.web.urls is not None:
                config_values["WebServiceUrls"] = self.web.urls

        if self.additional_settings is not None:
            for key, value in self.additional_settings.items():
                if not key:
                    continue
                config_values[key] = value if value is not None else ""

        return config_values

    def _build_native(self) -> object:
        """Build and return an ``flConfiguration*`` cffi pointer.

        Called by ``FoundryLocalManager._initialize()``.  The caller is
        responsible for releasing the returned handle via
        ``api.config.Configuration_Release()`` once ``Manager_Create`` has
        copied from it.
        """
        from foundry_local_sdk._native.api import api, ffi  # local import — avoids circular dependency

        out = ffi.new("flConfiguration**")
        api.check_status(api.config.Create(self.app_name.encode("utf-8"), out))
        native_config = out[0]

        try:
            return self._apply_settings(native_config, api, ffi)
        except BaseException:
            # Release the partially-configured handle so we don't leak it on error.
            api.config.Configuration_Release(native_config)
            raise

    def _apply_settings(self, native_config, api, ffi) -> object:
        # Log level
        if self.log_level is not None:
            level_int = _LOG_LEVEL_MAP.get(self.log_level, 3)  # default WARNING
            api.check_status(api.config.SetDefaultLogLevel(native_config, level_int))

        # Optional directory overrides
        if self.app_data_dir is not None:
            api.check_status(
                api.config.SetAppDataDir(native_config, self.app_data_dir.encode("utf-8"))
            )
        if self.logs_dir is not None:
            api.check_status(
                api.config.SetLogsDir(native_config, self.logs_dir.encode("utf-8"))
            )
        if self.model_cache_dir is not None:
            api.check_status(
                api.config.SetModelCacheDir(native_config, self.model_cache_dir.encode("utf-8"))
            )

        # Catalog URLs — order determines priority; NULL filter means use catalog default
        if self.catalog_urls:
            for url, filter_override in self.catalog_urls:
                api.check_status(
                    api.config.AddCatalogUrl(
                        native_config,
                        url.encode("utf-8"),
                        filter_override.encode("utf-8") if filter_override is not None else ffi.NULL,
                    )
                )
        if self.catalog_region is not None:
            api.check_status(
                api.config.SetCatalogRegion(
                    native_config, self.catalog_region.encode("utf-8")
                )
            )
        # Web service configuration
        if self.web is not None:
            if self.web.urls is not None:
                # URLs may be semicolon-separated; add each individually.
                for url in self.web.urls.split(";"):
                    url = url.strip()
                    if url:
                        api.check_status(
                            api.config.AddWebServiceEndpoint(native_config, url.encode("utf-8"))
                        )
            if self.web.external_url is not None:
                api.check_status(
                    api.config.SetExternalServiceUrl(
                        native_config, self.web.external_url.encode("utf-8")
                    )
                )

        # Additional key/value settings
        additional_settings: dict[str, str] = {}
        if self.additional_settings:
            additional_settings.update(self.additional_settings)
        if self.disable_nonessential_telemetry:
            additional_settings["DisableNonessentialTelemetry"] = "true"

        if additional_settings:
            kvp_out = ffi.new("flKeyValuePairs**")
            api.root.CreateKeyValuePairs(kvp_out)
            kvp = kvp_out[0]
            try:
                for key, value in additional_settings.items():
                    if not key:
                        continue
                    api.root.AddKeyValuePair(
                        kvp,
                        key.encode("utf-8"),
                        (value if value is not None else "").encode("utf-8"),
                    )
                api.check_status(api.config.SetAdditionalOptions(native_config, kvp))
            finally:
                api.root.KeyValuePairs_Release(kvp)

        return native_config
