mod ctx;
#[cfg(feature = "cli")]
pub mod cli;
mod env;
mod guest;
mod snapi;

pub use ctx::{NapiCtx, NapiCtxBuilder, NapiLimits, NapiRuntimeHooks, NapiSession};
pub(crate) use env::{GuestBackingStoreMapping, HostBufferCopy, RuntimeEnv};

pub fn module_needs_napi(module: &wasmer::Module) -> bool {
    NapiCtx::module_needs_napi(module)
}
