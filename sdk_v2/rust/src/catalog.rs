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
use crate::types::{CatalogType, ModelInfoBuilder};

/// The model catalog provides discovery and lookup for all available models.
pub struct Catalog {
    native: NativeCatalog,
    name: String,
    catalog_type: CatalogType,
}

impl Catalog {
    pub(crate) fn new(
        api: Arc<Api>,
        ptr: *mut crate::detail::ffi::flCatalog,
        manager: Arc<NativeManager>,
        catalog_type: CatalogType,
    ) -> Result<Self> {
        let native = NativeCatalog::new(api, ptr, manager);
        let name = native.name().unwrap_or_else(|_| "default".into());
        Ok(Self {
            native,
            name,
            catalog_type,
        })
    }

    /// Catalog name as reported by the native core.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// The source type represented by this catalog.
    pub fn catalog_type(&self) -> CatalogType {
        self.catalog_type
    }

    /// Create mutable metadata backed by the same native runtime as this catalog.
    pub fn create_model_info(&self) -> Result<ModelInfoBuilder> {
        ModelInfoBuilder::new(Arc::clone(&self.native.api), self.native.manager())
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

    /// Return catalog versions for an alias, optionally narrowed to one model name.
    ///
    /// `max_versions` limits the results per model name; `0` returns all versions.
    pub async fn get_model_versions(
        &self,
        model_alias: &str,
        model_name: Option<&str>,
        max_versions: u32,
    ) -> Result<Vec<Arc<Model>>> {
        if model_alias.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Model alias must be a non-empty string".into(),
            });
        }
        let max_versions =
            i32::try_from(max_versions).map_err(|_| FoundryLocalError::Validation {
                reason: "max_versions must not exceed i32::MAX".into(),
            })?;
        let native = self.native.clone();
        let model_alias = model_alias.to_owned();
        let model_name = model_name.map(str::to_owned);
        spawn_blocking(move || {
            native
                .get_model_versions(&model_alias, model_name.as_deref(), max_versions)?
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

    /// Register existing model assets in the local catalog without transferring ownership of those assets.
    pub async fn register_model(
        &self,
        model_path: &str,
        model_id: &str,
        metadata: ModelInfoBuilder,
    ) -> Result<Arc<Model>> {
        if self.catalog_type != CatalogType::Local {
            return Err(FoundryLocalError::Validation {
                reason: "Models can only be registered in the local catalog".into(),
            });
        }
        if model_path.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Model path must be a non-empty string".into(),
            });
        }
        if model_id.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Model id must be a non-empty string".into(),
            });
        }

        let native = self.native.clone();
        let model_path = model_path.to_owned();
        let model_id = model_id.to_owned();
        spawn_blocking(move || {
            let model = native.register_model(&model_path, &model_id, metadata.as_ptr())?;
            Model::from_variant(&native.api, model).map(Arc::new)
        })
        .await
    }

    /// Remove a local registration by alias or model id without deleting the caller-owned model assets.
    pub async fn unregister_model(&self, alias_or_model_id: &str) -> Result<()> {
        if self.catalog_type != CatalogType::Local {
            return Err(FoundryLocalError::Validation {
                reason: "Models can only be unregistered from the local catalog".into(),
            });
        }
        if alias_or_model_id.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Alias or model id must be a non-empty string".into(),
            });
        }

        let native = self.native.clone();
        let alias_or_model_id = alias_or_model_id.to_owned();
        spawn_blocking(move || native.unregister_model(&alias_or_model_id)).await
    }
}
