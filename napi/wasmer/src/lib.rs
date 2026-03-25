#[cfg(feature = "cli")]
pub mod cli;
mod ctx;
mod env;
mod guest;
mod snapi;

pub(crate) const NAPI_MODULE_NAME: &str = "napi";
pub(crate) const NAPI_EXTENSION_WASMER_MODULE_NAME: &str = "napi_extension_wasmer_v0";

pub use ctx::{NapiCtx, NapiCtxBuilder, NapiLimits, NapiRuntimeHooks, NapiSession};
pub(crate) use env::{GuestBackingStoreMapping, HostBufferCopy, RuntimeEnv};

pub fn module_needs_napi(module: &wasmer::Module) -> bool {
    NapiCtx::module_needs_napi(module)
}
