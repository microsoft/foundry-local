//! Public [`Model`] type backed by catalog-owned native handles.
//!
//! Mirrors the legacy SDK: a `Model` is either a single variant or a group of
//! variants sharing an alias. Selection is tracked Rust-side (an index) and all
//! operations delegate to the selected variant's native handle, so
//! [`Model::info`] / [`Model::id`] always reflect the current selection.

use std::fmt;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::Relaxed};
use std::sync::Arc;

use super::api::Api;
use super::info::build_model_info;
use super::native::NativeModel;
use super::task::spawn_blocking;
use crate::error::{FoundryLocalError, Result};
use crate::types::ModelInfo;

/// One specific variant: its native handle plus the cached, immutable metadata.
#[derive(Clone)]
pub(crate) struct VariantData {
    native: NativeModel,
    info: ModelInfo,
}

/// The public model type.
///
/// A `Model` may represent either a group of variants (as returned by
/// [`Catalog::get_model`](crate::Catalog::get_model)) or a single variant (as
/// returned by [`Catalog::get_model_variant`](crate::Catalog::get_model_variant)
/// or [`Model::variants`]).
pub struct Model {
    inner: ModelKind,
}

type DownloadProgressCallback = Box<dyn FnMut(f64) + Send + 'static>;

/// Builder for configuring and running a model download.
///
/// Use this builder when combining optional settings like progress and cancellation.
pub struct DownloadBuilder<'a> {
    model: &'a Model,
    progress: Option<DownloadProgressCallback>,
    cancel_flag: Option<Arc<AtomicBool>>,
}

impl<'a> DownloadBuilder<'a> {
    fn new(model: &'a Model) -> Self {
        Self {
            model,
            progress: None,
            cancel_flag: None,
        }
    }

    /// Report download progress as a percentage from 0.0 to 100.0.
    pub fn progress<F>(mut self, callback: F) -> Self
    where
        F: FnMut(f64) + Send + 'static,
    {
        self.progress = Some(Box::new(callback));
        self
    }

    /// Cancel the download when `cancel_flag` is set to `true`.
    pub fn cancel(mut self, cancel_flag: Arc<AtomicBool>) -> Self {
        self.cancel_flag = Some(cancel_flag);
        self
    }

    /// Run the configured download.
    pub async fn run(self) -> Result<()> {
        let native = self.model.selected_variant().native.clone();
        let progress = self.progress;
        let cancel_flag = self.cancel_flag;
        spawn_blocking(move || native.download(progress, cancel_flag)).await
    }
}
enum ModelKind {
    /// A single model variant (from `get_model_variant` or `variants()`).
    Variant(Arc<VariantData>),
    /// A group of variants sharing the same alias (from `get_model`).
    Group {
        alias: String,
        variants: Vec<Arc<VariantData>>,
        selected: AtomicUsize,
    },
}

impl Clone for Model {
    fn clone(&self) -> Self {
        Self {
            inner: match &self.inner {
                ModelKind::Variant(v) => ModelKind::Variant(v.clone()),
                ModelKind::Group {
                    alias,
                    variants,
                    selected,
                } => ModelKind::Group {
                    alias: alias.clone(),
                    variants: variants.clone(),
                    selected: AtomicUsize::new(selected.load(Relaxed)),
                },
            },
        }
    }
}

impl fmt::Debug for Model {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.inner {
            ModelKind::Variant(v) => f
                .debug_struct("Model::ModelVariant")
                .field("id", &v.info.id)
                .field("alias", &v.info.alias)
                .finish(),
            ModelKind::Group {
                alias,
                variants,
                selected,
            } => f
                .debug_struct("Model::Model")
                .field("alias", alias)
                .field("id", &variants[selected.load(Relaxed)].info.id)
                .field("variants_count", &variants.len())
                .field("selected_index", &selected.load(Relaxed))
                .finish(),
        }
    }
}

// ── Construction (crate-internal) ────────────────────────────────────────────

impl Model {
    /// Wrap a single leaf variant.
    pub(crate) fn from_variant(api: &Arc<Api>, native: NativeModel) -> Result<Self> {
        let info = build_model_info(api, &native)?;
        Ok(Self {
            inner: ModelKind::Variant(Arc::new(VariantData { native, info })),
        })
    }

    /// Wrap an alias-group model, eagerly loading its variants.
    pub(crate) fn from_group(api: &Arc<Api>, native: NativeModel) -> Result<Self> {
        let group_info = build_model_info(api, &native)?;
        let alias = group_info.alias.clone();

        let mut variants = Vec::new();
        for variant_native in native.get_variants()? {
            let info = build_model_info(api, &variant_native)?;
            variants.push(Arc::new(VariantData {
                native: variant_native,
                info,
            }));
        }

        // A leaf masquerading as a group: fall back to a single-variant model.
        if variants.is_empty() {
            return Ok(Self {
                inner: ModelKind::Variant(Arc::new(VariantData {
                    native,
                    info: group_info,
                })),
            });
        }

        // Prefer the first cached variant as the initial selection.
        let selected = variants.iter().position(|v| v.info.cached).unwrap_or(0);

        Ok(Self {
            inner: ModelKind::Group {
                alias,
                variants,
                selected: AtomicUsize::new(selected),
            },
        })
    }
}

// ── Private helpers ──────────────────────────────────────────────────────────

impl Model {
    fn selected_variant(&self) -> &VariantData {
        match &self.inner {
            ModelKind::Variant(v) => v.as_ref(),
            ModelKind::Group {
                variants, selected, ..
            } => variants[selected.load(Relaxed)].as_ref(),
        }
    }

    pub(crate) fn selected_native(&self) -> &NativeModel {
        &self.selected_variant().native
    }
}

// ── Public API ───────────────────────────────────────────────────────────────

impl Model {
    /// Unique identifier of the (selected) variant.
    pub fn id(&self) -> &str {
        &self.selected_variant().info.id
    }

    /// Alias shared by all variants of this model.
    pub fn alias(&self) -> &str {
        match &self.inner {
            ModelKind::Variant(v) => &v.info.alias,
            ModelKind::Group { alias, .. } => alias,
        }
    }

    /// Full catalog metadata for the (selected) variant.
    ///
    /// The native model is the source of truth. Each call returns a fresh
    /// point-in-time snapshot so cache state remains correct after download or
    /// removal.
    pub fn info(&self) -> Result<ModelInfo> {
        let variant = self.selected_variant();
        build_model_info(&variant.native.api, &variant.native)
    }

    /// Maximum context length (in tokens), or `None` if unknown.
    pub fn context_length(&self) -> Option<u64> {
        self.selected_variant().info.context_length
    }

    /// Comma-separated input modalities (e.g. `"text,image"`), or `None`.
    pub fn input_modalities(&self) -> Option<&str> {
        self.selected_variant().info.input_modalities.as_deref()
    }

    /// Comma-separated output modalities (e.g. `"text"`), or `None`.
    pub fn output_modalities(&self) -> Option<&str> {
        self.selected_variant().info.output_modalities.as_deref()
    }

    /// Capability tags (e.g. `"reasoning"`), or `None`.
    pub fn capabilities(&self) -> Option<&str> {
        self.selected_variant().info.capabilities.as_deref()
    }

    /// Whether the model supports tool/function calling, or `None`.
    pub fn supports_tool_calling(&self) -> Option<bool> {
        self.selected_variant().info.supports_tool_calling
    }

    /// Whether the (selected) variant is cached on disk.
    pub async fn is_cached(&self) -> Result<bool> {
        let native = self.selected_native().clone();
        spawn_blocking(move || native.is_cached()).await
    }

    /// Whether the (selected) variant is loaded into memory.
    pub async fn is_loaded(&self) -> Result<bool> {
        let native = self.selected_native().clone();
        spawn_blocking(move || native.is_loaded()).await
    }

    /// Download the (selected) variant.  If `progress` is provided it
    /// receives download progress as a percentage (0.0–100.0).
    pub async fn download<F>(&self, progress: Option<F>) -> Result<()>
    where
        F: FnMut(f64) + Send + 'static,
    {
        let native = self.selected_native().clone();
        let progress: Option<DownloadProgressCallback> =
            progress.map(|f| Box::new(f) as DownloadProgressCallback);
        spawn_blocking(move || native.download(progress, None)).await
    }

    /// Configure and run a model download with a builder.
    ///
    /// Use this for call sites that need progress, cancellation, or future
    /// download options.
    pub fn download_builder(&self) -> DownloadBuilder<'_> {
        DownloadBuilder::new(self)
    }

    /// Return the local file-system path of the (selected) variant.
    pub async fn path(&self) -> Result<PathBuf> {
        let native = self.selected_native().clone();
        let id = self.id().to_owned();
        let path = spawn_blocking(move || native.path()).await?;
        match path {
            Some(p) => Ok(PathBuf::from(p)),
            None => Err(FoundryLocalError::ModelOperation {
                reason: format!("Error getting path for model {id}. Has it been downloaded?"),
            }),
        }
    }

    /// Load the (selected) variant into memory.
    pub async fn load(&self) -> Result<()> {
        let native = self.selected_native().clone();
        spawn_blocking(move || native.load()).await
    }

    /// Unload the (selected) variant from memory.
    pub async fn unload(&self) -> Result<()> {
        let native = self.selected_native().clone();
        spawn_blocking(move || native.unload()).await
    }

    /// Remove the (selected) variant from the local cache.
    pub async fn remove_from_cache(&self) -> Result<()> {
        let native = self.selected_native().clone();
        spawn_blocking(move || native.remove_from_cache()).await
    }

    /// Create a [`ChatClient`](crate::openai::ChatClient) bound to the (selected) variant.
    #[deprecated(
        since = "2.0.0",
        note = "The OpenAI direct clients are deprecated; use `ChatSession::new(&model)` instead."
    )]
    #[allow(deprecated)]
    pub fn create_chat_client(&self) -> crate::openai::ChatClient {
        let v = self.selected_variant();
        crate::openai::ChatClient::new(&v.info.id, v.native.clone())
    }

    /// Create an [`AudioClient`](crate::openai::AudioClient) bound to the (selected) variant.
    #[deprecated(
        since = "2.0.0",
        note = "The OpenAI direct clients are deprecated; use `AudioSession::new(&model)` instead."
    )]
    #[allow(deprecated)]
    pub fn create_audio_client(&self) -> crate::openai::AudioClient {
        let v = self.selected_variant();
        crate::openai::AudioClient::new(&v.info.id, v.native.clone())
    }

    /// Create an [`EmbeddingClient`](crate::openai::EmbeddingClient) bound to the (selected) variant.
    #[deprecated(
        since = "2.0.0",
        note = "The OpenAI direct clients are deprecated; use `EmbeddingsSession::new(&model)` \
                instead."
    )]
    #[allow(deprecated)]
    pub fn create_embedding_client(&self) -> crate::openai::EmbeddingClient {
        let v = self.selected_variant();
        crate::openai::EmbeddingClient::new(&v.info.id, v.native.clone())
    }

    /// Available variants of this model.
    ///
    /// For a single-variant model (e.g. from
    /// [`Catalog::get_model_variant`](crate::Catalog::get_model_variant)),
    /// this returns a single-element list containing itself.
    pub fn variants(&self) -> Vec<Arc<Model>> {
        match &self.inner {
            ModelKind::Variant(v) => {
                vec![Arc::new(Model {
                    inner: ModelKind::Variant(v.clone()),
                })]
            }
            ModelKind::Group { variants, .. } => variants
                .iter()
                .map(|v| {
                    Arc::new(Model {
                        inner: ModelKind::Variant(v.clone()),
                    })
                })
                .collect(),
        }
    }

    /// Select a variant to use for subsequent operations.
    ///
    /// The `variant` must be one of the models returned by [`variants`](Model::variants).
    ///
    /// # Errors
    ///
    /// Returns an error if the variant does not belong to this model.
    /// For single-variant models this always returns an error — use
    /// [`Catalog::get_model`](crate::Catalog::get_model) to obtain a model
    /// with all variants available.
    pub fn select_variant(&self, variant: &Model) -> Result<()> {
        self.select_variant_by_id(variant.id())
    }

    /// Select a variant by its unique id string.
    ///
    /// This is a convenience method for cases where you have a variant id
    /// from an external source. Prefer [`select_variant`](Model::select_variant)
    /// when you already have a [`Model`] reference from [`variants`](Model::variants).
    ///
    /// # Errors
    ///
    /// Returns an error if no variant with the given id exists.
    /// For single-variant models this always returns an error — use
    /// [`Catalog::get_model`](crate::Catalog::get_model) to obtain a model
    /// with all variants available.
    pub fn select_variant_by_id(&self, id: &str) -> Result<()> {
        match &self.inner {
            ModelKind::Variant(v) => Err(FoundryLocalError::ModelOperation {
                reason: format!(
                    "Selecting a variant is not supported on a single-variant model. \
                     Call Catalog::get_model(\"{}\") to get a model with all variants available.",
                    v.info.alias
                ),
            }),
            ModelKind::Group {
                variants,
                selected,
                alias,
            } => match variants.iter().position(|v| v.info.id == id) {
                Some(pos) => {
                    selected.store(pos, Relaxed);
                    Ok(())
                }
                None => {
                    let available: Vec<&str> =
                        variants.iter().map(|v| v.info.id.as_str()).collect();
                    Err(FoundryLocalError::ModelOperation {
                        reason: format!(
                            "Variant '{id}' not found for model '{alias}'. Available: {available:?}",
                        ),
                    })
                }
            },
        }
    }
}
