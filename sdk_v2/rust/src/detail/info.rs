//! Builds the public [`ModelInfo`] from native `flModelInfo` accessors.

use super::api::{cstr_to_string, read_kvps, Api};
use super::ffi::*;
use super::native::NativeModel;
use crate::error::Result;
use crate::types::{DeviceType, ModelInfo, ModelSettings, Parameter, PromptTemplate, Runtime};

fn device_type(value: flDeviceType) -> DeviceType {
    match value {
        FOUNDRY_LOCAL_DEVICE_CPU => DeviceType::CPU,
        FOUNDRY_LOCAL_DEVICE_GPU => DeviceType::GPU,
        FOUNDRY_LOCAL_DEVICE_NPU => DeviceType::NPU,
        _ => DeviceType::Invalid,
    }
}

/// Build a fully-populated [`ModelInfo`] for the given model handle.
pub(crate) fn build_model_info(api: &Api, native: &NativeModel) -> Result<ModelInfo> {
    let info = native.info_ptr()?;
    let m = api.model_api();

    // SAFETY: `info` is a valid model-owned info pointer; all accessors below
    // return pointers owned by the model and valid for the duration of use.
    unsafe {
        let id = cstr_to_string((m.Info_GetId)(info)).unwrap_or_default();
        let name = cstr_to_string((m.Info_GetName)(info)).unwrap_or_default();
        let version = (m.Info_GetVersion)(info).max(0) as u64;
        let alias = cstr_to_string((m.Info_GetAlias)(info)).unwrap_or_default();
        let uri = cstr_to_string((m.Info_GetUri)(info)).unwrap_or_default();
        let task = cstr_to_string((m.Info_GetTask)(info));
        let execution_provider = cstr_to_string((m.Info_GetExecutionProvider)(info));
        let device = device_type((m.Info_GetDeviceType)(info));

        let str_prop = |key: &str| -> Option<String> {
            let c = std::ffi::CString::new(key).ok()?;
            cstr_to_string((m.Info_GetStringProperty)(info, c.as_ptr()))
        };
        let int_prop = |key: &str, default_value: i64| -> i64 {
            match std::ffi::CString::new(key) {
                Ok(c) => (m.Info_GetIntProperty)(info, c.as_ptr(), default_value),
                Err(_) => default_value,
            }
        };
        let opt_u64 = |v: i64| -> Option<u64> {
            if v >= 0 {
                Some(v as u64)
            } else {
                None
            }
        };
        let opt_bool = |v: i64| -> Option<bool> {
            match v {
                0 => Some(false),
                1 => Some(true),
                _ => None,
            }
        };

        let runtime = execution_provider.clone().map(|ep| Runtime {
            device_type: device.clone(),
            execution_provider: ep,
        });

        let prompt_template = build_prompt_template(api, (m.Info_GetPromptTemplates)(info));
        let model_settings = build_model_settings(api, (m.Info_GetModelSettings)(info));

        Ok(ModelInfo {
            id,
            name,
            version,
            alias,
            display_name: str_prop(FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR),
            provider_type: str_prop(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR)
                .unwrap_or_default(),
            uri,
            model_type: str_prop(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR).unwrap_or_default(),
            prompt_template,
            publisher: str_prop(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR),
            model_settings,
            license: str_prop(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR),
            license_description: str_prop(FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR),
            cached: native.is_cached()?,
            task,
            runtime,
            file_size_mb: opt_u64(int_prop(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT, -1)),
            supports_tool_calling: opt_bool(int_prop(
                FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT,
                -1,
            )),
            max_output_tokens: opt_u64(int_prop(
                FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT,
                -1,
            )),
            min_fl_version: str_prop(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR),
            created_at_unix: int_prop(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, 0).max(0)
                as u64,
            context_length: opt_u64(int_prop(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, -1)),
            input_modalities: str_prop(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR),
            output_modalities: str_prop(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR),
            capabilities: str_prop(FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR),
        })
    }
}

unsafe fn build_prompt_template(api: &Api, kvps: *const flKeyValuePairs) -> Option<PromptTemplate> {
    if kvps.is_null() {
        return None;
    }
    let pairs = read_kvps(api, kvps);
    if pairs.is_empty() {
        return None;
    }
    let get = |key: &str| -> Option<String> {
        pairs
            .iter()
            .find(|(k, _)| k == key)
            .and_then(|(_, v)| v.clone())
    };
    let template = PromptTemplate {
        system: get("system"),
        user: get("user"),
        assistant: get("assistant"),
        prompt: get("prompt"),
    };
    if template.system.is_none()
        && template.user.is_none()
        && template.assistant.is_none()
        && template.prompt.is_none()
    {
        None
    } else {
        Some(template)
    }
}

unsafe fn build_model_settings(api: &Api, kvps: *const flKeyValuePairs) -> Option<ModelSettings> {
    if kvps.is_null() {
        return None;
    }
    let pairs = read_kvps(api, kvps);
    if pairs.is_empty() {
        return None;
    }
    let parameters = pairs
        .into_iter()
        .map(|(name, value)| Parameter { name, value })
        .collect::<Vec<_>>();
    Some(ModelSettings {
        parameters: Some(parameters),
    })
}
