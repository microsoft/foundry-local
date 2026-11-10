# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for Configuration — pure Python, no native dependency."""
from __future__ import annotations

from types import SimpleNamespace
from unittest.mock import MagicMock, Mock

import pytest

from foundry_local_sdk import Configuration, FoundryLocalException, LogLevel


class TestConfigurationValidation:
    def test_default_construction_validates(self):
        c = Configuration(app_name="ValidName")
        c.validate()  # must not raise

    @pytest.mark.parametrize("bad_name", ["", "has spaces", "name/with/slashes", "name\\back"])
    def test_invalid_app_name_rejected(self, bad_name):
        c = Configuration(app_name=bad_name)
        with pytest.raises(FoundryLocalException):
            c.validate()

    def test_invalid_external_url_port_rejected(self):
        c = Configuration(
            app_name="App",
            web=Configuration.WebService(external_url="http://localhost/no-port"),
        )
        with pytest.raises(FoundryLocalException):
            c.validate()

    def test_shared_host_rejects_external_service(self):
        configuration = Configuration(
            app_name="App",
            use_shared_host=True,
            web=Configuration.WebService(external_url="http://127.0.0.1:1234"),
        )
        with pytest.raises(FoundryLocalException, match="cannot use an external service"):
            configuration.validate()


class TestSharedHostNativeConfiguration:
    def test_enabled_setting_is_marshaled_with_v3(self):
        configuration = Configuration(app_name="App", use_shared_host=True)
        native_api = MagicMock()
        native_api.api_version = 3
        pointer = object()

        assert configuration._apply_settings(pointer, native_api, MagicMock()) is pointer
        native_api.config.SetUseSharedHost.assert_called_once_with(pointer, True)

    def test_enabled_setting_rejects_v2_before_reading_any_extension(self):
        configuration = Configuration(app_name="App", use_shared_host=True)
        legacy_api = SimpleNamespace(api_version=2)

        with pytest.raises(FoundryLocalException, match="requires an API v3") as error:
            configuration._apply_settings(object(), legacy_api, MagicMock())
        assert error.value.error_code == 1

    def test_default_mode_uses_only_the_v2_prefix(self):
        configuration = Configuration(app_name="App")
        legacy_api = MagicMock()
        legacy_api.api_version = 2
        legacy_api.config = SimpleNamespace(SetDefaultLogLevel=Mock(), SetAdditionalOptions=Mock())
        pointer = object()

        assert configuration._apply_settings(pointer, legacy_api, MagicMock()) is pointer


class TestConfigurationDictionary:
    def test_minimal_settings_serialize(self):
        c = Configuration(app_name="App", log_level=LogLevel.INFORMATION)
        d = c.as_dictionary()
        assert d["AppName"] == "App"
        assert d["LogLevel"] == str(LogLevel.INFORMATION)
        assert d["UserAgent"].startswith("foundry-local-python/")
        assert "AppDataDir" not in d

    def test_all_directory_settings_round_trip(self):
        c = Configuration(
            app_name="App",
            app_data_dir="/data",
            model_cache_dir="/cache",
            logs_dir="/logs",
        )
        d = c.as_dictionary()
        assert d["AppDataDir"] == "/data"
        assert d["ModelCacheDir"] == "/cache"
        assert d["LogsDir"] == "/logs"

    def test_additional_settings_merged_in(self):
        c = Configuration(
            app_name="App",
            additional_settings={"Key1": "v1", "Key2": "v2", "": "ignored"},
        )
        d = c.as_dictionary()
        assert d["Key1"] == "v1"
        assert d["Key2"] == "v2"
        assert "" not in d

    def test_user_agent_can_be_overridden(self):
        c = Configuration(app_name="App", additional_settings={"UserAgent": "custom-client/1"})
        d = c.as_dictionary()
        assert d["UserAgent"] == "custom-client/1"

    def test_additional_settings_none_value_becomes_empty(self):
        c = Configuration(app_name="App", additional_settings={"K": None})
        d = c.as_dictionary()
        assert d["K"] == ""

    def test_web_urls_round_trip(self):
        c = Configuration(
            app_name="App",
            web=Configuration.WebService(urls="http://localhost:5273"),
        )
        d = c.as_dictionary()
        assert d["WebServiceUrls"] == "http://localhost:5273"


class TestConfigurationCatalogUrls:
    def test_catalog_urls_default_is_none(self):
        c = Configuration(app_name="App")
        assert c.catalog_urls is None

    def test_catalog_urls_stored_on_instance(self):
        urls = [("https://example.com/catalog", None), ("https://other/catalog", "some-filter")]
        c = Configuration(app_name="App", catalog_urls=urls)
        assert c.catalog_urls == urls


class TestConfigurationExtendedFields:
    def test_new_fields_default_to_none(self):
        c = Configuration(app_name="App")
        assert c.catalog_region is None
        assert c.disable_nonessential_telemetry is False

    def test_new_fields_round_trip(self):
        c = Configuration(
            app_name="App",
            catalog_region="westus2",
            disable_nonessential_telemetry=True,
        )
        assert c.catalog_region == "westus2"
        assert c.disable_nonessential_telemetry is True
