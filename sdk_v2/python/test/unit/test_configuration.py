# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for Configuration — pure Python, no native dependency."""
from __future__ import annotations

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
