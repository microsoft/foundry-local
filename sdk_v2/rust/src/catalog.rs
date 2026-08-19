//! Model catalog — discovery and lookup for available models.
//!
//! The native catalog (owned by the [`FoundryLocalManager`](crate::FoundryLocalManager))
//! caches the model list and refreshes itself, so this is a thin async wrapper
//! that preserves the legacy public surface.

use std::sync::Arc;

use crate::detail::api::Api;
use crate::detail::manager::NativeManager;
use crate::detail::model::Model;
use crate::detail::native::NativeCatalog;
use crate::detail::task::spawn_blocking;
use crate::error::{FoundryLocalError, Result};

/// The model catalog provides discovery and lookup for all available models.
pub struct Catalog {
    native: NativeCatalog,
    name: String,
}

impl Catalog {
    pub(crate) fn new(
        api: Arc<Api>,
        ptr: *mut crate::detail::ffi::flCatalog,
        manager: Arc<NativeManager>,
    ) -> Result<Self> {
        let native = NativeCatalog::new(api, ptr, manager);
        let name = native.name().unwrap_or_else(|_| "default".into());
        Ok(Self { native, name })
    }

    /// Catalog name as reported by the native core.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Refresh the catalog from the native core.
    ///
    /// **No-op.** The native catalog manages its own caching and refresh, so
    /// there is nothing for the SDK to do here. This method is retained only for
    /// API compatibility with the legacy SDK and always returns `Ok(())`.
    pub async fn update_models(&self) -> Result<()> {
        Ok(())
    }

    /// Return all known models keyed by alias.
    pub async fn get_models(&self) -> Result<Vec<Arc<Model>>> {
        let native = self.native.clone();
        spawn_blocking(move || {
            native
                .get_models()?
                .into_iter()
                .map(|m| Model::from_group(&native.api, m).map(Arc::new))
                .collect()
        })
        .await
    }

    /// Look up a model by its alias.
    pub async fn get_model(&self, alias: &str) -> Result<Arc<Model>> {
        if alias.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Model alias must be a non-empty string".into(),
            });
        }
        let native = self.native.clone();
        let alias = alias.to_owned();
        spawn_blocking(move || match native.get_model(&alias)? {
            Some(m) => Model::from_group(&native.api, m).map(Arc::new),
            None => {
                let available: Vec<String> = native
                    .get_models()
                    .ok()
                    .map(|models| {
                        models
                            .iter()
                            .filter_map(|m| {
                                m.info_ptr().ok().map(|info| unsafe {
                                    crate::detail::api::cstr_to_string((native
                                        .api
                                        .model_api()
                                        .Info_GetAlias)(
                                        info
                                    ))
                                    .unwrap_or_default()
                                })
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                Err(FoundryLocalError::ModelOperation {
                    reason: format!("Unknown model alias '{alias}'. Available: {available:?}"),
                })
            }
        })
        .await
    }

    /// Look up a specific model variant by its unique id.
    ///
    /// NOTE: This will return a `Model` representing a single variant. Use
    /// [`get_model`](Catalog::get_model) to obtain a `Model` with all
    /// available variants.
    pub async fn get_model_variant(&self, id: &str) -> Result<Arc<Model>> {
        if id.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Variant id must be a non-empty string".into(),
            });
        }
        let native = self.native.clone();
        let id = id.to_owned();
        spawn_blocking(move || match native.get_model_variant(&id)? {
            Some(m) => Model::from_variant(&native.api, m).map(Arc::new),
            None => Err(FoundryLocalError::ModelOperation {
                reason: format!("Unknown variant id '{id}'."),
            }),
        })
        .await
    }

    /// Return only the model variants that are currently cached on disk.
    pub async fn get_cached_models(&self) -> Result<Vec<Arc<Model>>> {
        let native = self.native.clone();
        spawn_blocking(move || {
            native
                .get_cached_models()?
                .into_iter()
                .map(|m| Model::from_variant(&native.api, m).map(Arc::new))
                .collect()
        })
        .await
    }

    /// Return model variants that are currently loaded into memory.
    pub async fn get_loaded_models(&self) -> Result<Vec<Arc<Model>>> {
        let native = self.native.clone();
        spawn_blocking(move || {
            native
                .get_loaded_models()?
                .into_iter()
                .map(|m| Model::from_variant(&native.api, m).map(Arc::new))
                .collect()
        })
        .await
    }

    /// Resolve the latest catalog version for the provided model or variant.
    pub async fn get_latest_version(&self, model_or_model_variant: &Model) -> Result<Arc<Model>> {
        let native = self.native.clone();
        let target = model_or_model_variant.selected_native().clone();
        spawn_blocking(move || {
            let latest = native.get_latest_version(&target)?;
            Model::from_variant(&native.api, latest).map(Arc::new)
        })
        .await
    }
}
