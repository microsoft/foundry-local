# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
import logging
import sys

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.version import __version__
from foundry_local_sdk.logging_helper import LogLevel
from foundry_local_sdk.configuration import Configuration
from foundry_local_sdk.foundry_local_manager import FoundryLocalManager
from foundry_local_sdk.catalog import Catalog
from foundry_local_sdk.imodel import IModel
from foundry_local_sdk.ep_types import EpInfo, EpDownloadResult
from foundry_local_sdk.model_info import (
    ModelInfo,
    PromptTemplate,
    Runtime,
    Parameter,
    ModelSettings,
    DeviceType,
)
from foundry_local_sdk.items import (
    Item,
    TextItem,
    MessageItem,
    BytesItem,
    ImageItem,
    AudioItem,
    ToolCallItem,
    ToolResultItem,
    TensorItem,
    ItemType,
    TextItemType,
    MessageRole,
    TensorDataType,
)
from foundry_local_sdk.session_types import (
    FinishReason,
    RequestOptions,
    SearchOptions,
    TokenUsage,
    ToolChoice,
)
from foundry_local_sdk.item_queue import ItemQueue
from foundry_local_sdk.request import Request
from foundry_local_sdk.response import Response
from foundry_local_sdk.session import Session, ChatSession, AudioSession, EmbeddingsSession, StreamingResponse

_logger = logging.getLogger(__name__)
_logger.setLevel(logging.WARNING)
_sc = logging.StreamHandler(stream=sys.stdout)
_formatter = logging.Formatter(
    "[foundry-local] | %(asctime)s | %(levelname)-8s | %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
_sc.setFormatter(_formatter)
_logger.addHandler(_sc)
_logger.propagate = False

# Alias for callers who prefer the un-prefixed name.
Model = IModel

__all__ = [
    "FoundryLocalException",
    "__version__",
    "LogLevel",
    "Configuration",
    "FoundryLocalManager",
    "Catalog",
    "IModel",
    "Model",
    "ModelInfo",
    "PromptTemplate",
    "Runtime",
    "Parameter",
    "ModelSettings",
    "DeviceType",
    "EpInfo",
    "EpDownloadResult",
    "Item",
    "TextItem",
    "MessageItem",
    "BytesItem",
    "ImageItem",
    "AudioItem",
    "ToolCallItem",
    "ToolResultItem",
    "TensorItem",
    "ItemType",
    "TextItemType",
    "MessageRole",
    "TensorDataType",
    "ItemQueue",
    "FinishReason",
    "TokenUsage",
    "SearchOptions",
    "RequestOptions",
    "ToolChoice",
    "Request",
    "Response",
    "Session",
    "ChatSession",
    "AudioSession",
    "EmbeddingsSession",
    "StreamingResponse",
]
