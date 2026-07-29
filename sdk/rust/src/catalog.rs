//! Model catalog – discovers, caches, and looks up available models.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::detail::core_interop::CoreInterop;
use crate::detail::model::Model;
use crate::detail::model_variant::ModelVariant;
use crate::detail::ModelLoadManager;
use crate::error::{FoundryLocalError, Result};
use crate::types::ModelInfo;

/// How long the catalog cache remains valid before a refresh.
const CACHE_TTL: Duration = Duration::from_secs(6 * 60 * 60); // 6 hours

/// Shared flag allowing `ModelVariant` to signal that the catalog cache is
/// stale (e.g. after a download or removal).
#[derive(Clone, Debug)]
pub(crate) struct CacheInvalidator(Arc<AtomicBool>);

impl CacheInvalidator {
    fn new() -> Self {
        Self(Arc::new(AtomicBool::new(false)))
    }

    /// Mark the catalog cache as stale.
    pub fn invalidate(&self) {
        self.0.store(true, Ordering::Release);
    }

    /// Check and clear the invalidation flag.
    fn take(&self) -> bool {
        self.0.swap(false, Ordering::AcqRel)
    }
}

/// All mutable catalog data behind a single lock to prevent split-brain reads.
struct CatalogState {
    models_by_alias: HashMap<String, Arc<Model>>,
    variants_by_id: HashMap<String, Arc<Model>>,
    last_refresh: Option<Instant>,
}

/// The model catalog provides discovery and lookup for all available models.
pub struct Catalog {
    core: Arc<CoreInterop>,
    model_load_manager: Arc<ModelLoadManager>,
    name: String,
    state: Mutex<CatalogState>,
    /// Async gate ensuring only one refresh runs at a time.
    refresh_gate: tokio::sync::Mutex<()>,
    invalidator: CacheInvalidator,
}

impl Catalog {
    pub(crate) fn new(
        core: Arc<CoreInterop>,
        model_load_manager: Arc<ModelLoadManager>,
    ) -> Result<Self> {
        let name = core
            .execute_command("get_catalog_name", None)
            .unwrap_or_else(|_| "default".into());

        let invalidator = CacheInvalidator::new();
        let catalog = Self {
            core,
            model_load_manager,
            name,
            state: Mutex::new(CatalogState {
                models_by_alias: HashMap::new(),
                variants_by_id: HashMap::new(),
                last_refresh: None,
            }),
            refresh_gate: tokio::sync::Mutex::new(()),
            invalidator,
        };

        // Perform initial synchronous refresh during construction.
        catalog.force_refresh_sync()?;
        Ok(catalog)
    }

    /// Catalog name as reported by the native core.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Invalidate the catalog cache so the next access re-fetches models.
    pub(crate) fn invalidate_cache(&self) {
        self.invalidator.invalidate();
    }

    /// Refresh the catalog from the native core if the cache has expired or
    /// has been explicitly invalidated (e.g. after a download or removal).
    pub async fn update_models(&self) -> Result<()> {
        let invalidated = self.invalidator.take();

        // Fast path: check under data lock (held briefly).
        if !invalidated {
            let s = self.lock_state()?;
            if let Some(ts) = s.last_refresh {
                if ts.elapsed() < CACHE_TTL {
                    return Ok(());
                }
            }
        }

        // Slow path: acquire refresh gate so only one thread refreshes.
        let _gate = self.refresh_gate.lock().await;

        // Re-check after acquiring the gate — another thread may have refreshed.
        if !invalidated {
            let s = self.lock_state()?;
            if let Some(ts) = s.last_refresh {
                if ts.elapsed() < CACHE_TTL {
                    return Ok(());
                }
            }
        }

        self.force_refresh().await
    }

    /// Return all known models keyed by alias.
    pub async fn get_models(&self) -> Result<Vec<Arc<Model>>> {
        self.update_models().await?;
        let s = self.lock_state()?;
        Ok(s.models_by_alias.values().cloned().collect())
    }

    /// Look up a model by its alias.
    pub async fn get_model(&self, alias: &str) -> Result<Arc<Model>> {
        if alias.trim().is_empty() {
            return Err(FoundryLocalError::Validation {
                reason: "Model alias must be a non-empty string".into(),
            });
        }
        self.update_models().await?;
        if let Some(model) = self.lock_state()?.models_by_alias.get(alias).cloned() {
            return Ok(model);
        }

        // Self-heal: the alias may belong to a BYOM model added to the cache
        // directory after our last catalog refresh.
        self.invalidator.invalidate();
        self.update_models().await?;
        let s = self.lock_state()?;
        s.models_by_alias.get(alias).cloned().ok_or_else(|| {
            let available: Vec<&str> = s.models_by_alias.keys().map(|k| k.as_str()).collect();
            FoundryLocalError::ModelOperation {
                reason: format!("Unknown model alias '{alias}'. Available: {available:?}"),
            }
        })
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
        self.update_models().await?;
        if let Some(variant) = self.lock_state()?.variants_by_id.get(id).cloned() {
            return Ok(variant);
        }

        // Self-heal: the id may belong to a BYOM model added to the cache
        // directory after our last catalog refresh.
        self.invalidator.invalidate();
        self.update_models().await?;
        let s = self.lock_state()?;
        s.variants_by_id.get(id).cloned().ok_or_else(|| {
            let available: Vec<&str> = s.variants_by_id.keys().map(|k| k.as_str()).collect();
            FoundryLocalError::ModelOperation {
                reason: format!("Unknown variant id '{id}'. Available: {available:?}"),
            }
        })
    }

    /// Return only the model variants that are currently cached on disk.
    pub async fn get_cached_models(&self) -> Result<Vec<Arc<Model>>> {
        self.update_models().await?;
        let raw = self
            .core
            .execute_command_async("get_cached_models".into(), None)
            .await?;
        if raw.trim().is_empty() {
            return Ok(Vec::new());
        }
        let cached_ids: Vec<String> = serde_json::from_str(&raw)?;
        self.resolve_model_ids(&cached_ids).await
    }

    /// Return model variants that are currently loaded into memory.
    pub async fn get_loaded_models(&self) -> Result<Vec<Arc<Model>>> {
        self.update_models().await?;
        let loaded_ids = self.model_load_manager.list_loaded().await?;
        self.resolve_model_ids(&loaded_ids).await
    }

    /// Resolve a list of model ids against the in-memory catalog, self-healing
    /// once if any id is unknown (e.g. a manually-added BYOM model the SDK has
    /// not yet seen). Preserves the input order of `model_ids` (minus unknowns).
    async fn resolve_model_ids(&self, model_ids: &[String]) -> Result<Vec<Arc<Model>>> {
        let needs_refresh = {
            let s = self.lock_state()?;
            model_ids
                .iter()
                .any(|id| !s.variants_by_id.contains_key(id))
        };

        if needs_refresh {
            self.invalidator.invalidate();
            self.update_models().await?;
        }

        let s = self.lock_state()?;
        Ok(model_ids
            .iter()
            .filter_map(|id| s.variants_by_id.get(id).cloned())
            .collect())
    }

    /// Resolve the latest catalog version for the provided model or variant.
    pub async fn get_latest_version(&self, model_or_model_variant: &Model) -> Result<Arc<Model>> {
        self.update_models().await?;
        let s = self.lock_state()?;

        let model = s
            .models_by_alias
            .get(model_or_model_variant.alias())
            .ok_or_else(|| FoundryLocalError::ModelOperation {
                reason: format!(
                    "Model with alias '{}' not found in catalog.",
                    model_or_model_variant.alias()
                ),
            })?;

        let latest = model
            .variants()
            .into_iter()
            .find(|variant| variant.info().name == model_or_model_variant.info().name)
            .ok_or_else(|| FoundryLocalError::Internal {
                reason: format!(
                    "Mismatch between model (alias:{}) and model variant (alias:{}).",
                    model.alias(),
                    model_or_model_variant.alias()
                ),
            })?;

        Ok(latest)
    }

    async fn force_refresh(&self) -> Result<()> {
        let raw = self
            .core
            .execute_command_async("get_model_list".into(), None)
            .await?;
        self.apply_model_list(&raw)
    }

    /// Synchronous refresh used only during construction (before a tokio
    /// runtime may be available).
    fn force_refresh_sync(&self) -> Result<()> {
        let raw = self.core.execute_command("get_model_list", None)?;
        self.apply_model_list(&raw)
    }

    fn apply_model_list(&self, raw: &str) -> Result<()> {
        let infos: Vec<ModelInfo> = if raw.trim().is_empty() {
            Vec::new()
        } else {
            serde_json::from_str(raw)?
        };

        let mut alias_map_build: HashMap<String, Model> = HashMap::new();
        let mut id_map: HashMap<String, Arc<Model>> = HashMap::new();

        for info in infos {
            let id = info.id.clone();
            let alias = info.alias.clone();
            let variant = ModelVariant::new(
                info,
                Arc::clone(&self.core),
                Arc::clone(&self.model_load_manager),
                self.invalidator.clone(),
            );
            id_map.insert(id, Arc::new(Model::from_variant(variant.clone())));

            alias_map_build
                .entry(alias)
                .or_insert_with_key(|a| Model::from_group(a.clone(), Arc::clone(&self.core)))
                .add_variant(variant);
        }

        let fresh_alias_map: HashMap<String, Arc<Model>> = alias_map_build
            .into_iter()
            .map(|(k, v)| (k, Arc::new(v)))
            .collect();

        // Incremental refresh: hold the data lock for the entire compare-and-
        // swap so the merged maps are computed against the same `old` snapshot
        // they will replace. Reuse the existing `Arc<Model>` whenever the
        // per-key `(id, cached)` fingerprint is unchanged so externally held
        // references keep working with up-to-date metadata and (for aliased
        // models) keep any explicit `select_variant` choice across refreshes.
        let mut s = self.lock_state()?;

        let merged_alias_map: HashMap<String, Arc<Model>> = fresh_alias_map
            .into_iter()
            .map(|(alias, fresh_arc)| {
                let reuse = s
                    .models_by_alias
                    .get(&alias)
                    .filter(|old_arc| alias_fingerprint(old_arc) == alias_fingerprint(&fresh_arc))
                    .map(Arc::clone);
                (alias, reuse.unwrap_or(fresh_arc))
            })
            .collect();

        let merged_id_map: HashMap<String, Arc<Model>> = id_map
            .into_iter()
            .map(|(id, fresh_arc)| {
                let reuse = s
                    .variants_by_id
                    .get(&id)
                    .filter(|old_arc| {
                        variant_fingerprint(old_arc) == variant_fingerprint(&fresh_arc)
                    })
                    .map(Arc::clone);
                (id, reuse.unwrap_or(fresh_arc))
            })
            .collect();

        s.models_by_alias = merged_alias_map;
        s.variants_by_id = merged_id_map;
        s.last_refresh = Some(Instant::now());

        Ok(())
    }

    fn lock_state(&self) -> Result<std::sync::MutexGuard<'_, CatalogState>> {
        self.state.lock().map_err(|_| FoundryLocalError::Internal {
            reason: "catalog state mutex poisoned".into(),
        })
    }
}

/// Fingerprint of an alias-grouped `Model`: the `(id, cached)` of every
/// variant in catalog order. Two fingerprints are equal exactly when reusing
/// the old `Arc<Model>` would surface the same `ModelInfo` data as a freshly
/// built one — i.e. no variants were added, removed, reordered, or had their
/// `cached` flag flipped. Per the [crate::types::ModelInfo] contract, all
/// other fields on a given `id` are treated as immutable.
fn alias_fingerprint(model: &Model) -> Vec<(String, bool)> {
    model
        .variants()
        .iter()
        .map(|v| (v.info().id.clone(), v.info().cached))
        .collect()
}

/// Fingerprint of a single-variant `Model`: just its `(id, cached)`.
fn variant_fingerprint(model: &Model) -> (String, bool) {
    (model.info().id.clone(), model.info().cached)
}
