mod guest;
mod snapi;

use anyhow::{Context, Result};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use virtual_fs::{AsyncReadExt, FileSystem};
use wasmer::Table;
use wasmer::{
    imports,
    sys::{EngineBuilder, Features},
    ExternType, FunctionEnv, Imports, Instance, Memory, Module, Store, TypedFunction, Value,
};
use wasmer_cache::{Cache, FileSystemCache, Hash as CacheHash};
use wasmer_compiler_llvm::{LLVMOptLevel, LLVM};
use wasmer_types::ModuleHash;
use wasmer_wasix::{
    runners::wasi::{RuntimeOrEngine, WasiRunner},
    runtime::task_manager::tokio::TokioTaskManager,
    Pipe, PluggableRuntime, WasiError,
};

use crate::guest::callback::set_top_level_callback_state;
use crate::guest::napi::register_env_imports;
use crate::guest::napi::register_napi_imports;

const UNOFFICIAL_ENV_HANDLE: i32 = 1;

fn candidate_repo_roots(seed_paths: &[&Path]) -> Vec<PathBuf> {
    let mut out = Vec::new();

    if let Some(root) = std::env::var_os("UBI_REPO_ROOT") {
        out.push(PathBuf::from(root));
    }

    for seed in seed_paths {
        for ancestor in seed.ancestors() {
            out.push(ancestor.to_path_buf());
        }
    }

    let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    for ancestor in cwd.ancestors() {
        out.push(ancestor.to_path_buf());
    }

    out.sort();
    out.dedup();
    out
}

fn resolve_repo_root(seed_paths: &[&Path]) -> Option<PathBuf> {
    for root in candidate_repo_roots(seed_paths) {
        if root.join("node-lib").is_dir() && root.join("node").is_dir() {
            return std::fs::canonicalize(&root).ok().or(Some(root));
        }
    }
    None
}

#[derive(Debug, Clone)]
pub struct GuestMount {
    pub host_path: PathBuf,
    pub guest_path: PathBuf,
}

fn spawn_pipe_drain_thread(
    mut pipe: Pipe,
    mut sink: Box<dyn Write + Send>,
) -> std::thread::JoinHandle<Result<String>> {
    std::thread::spawn(move || {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .context("failed to create stdio drain runtime")?;
        let mut captured = Vec::new();
        let mut chunk = [0u8; 8192];
        loop {
            let n = runtime
                .block_on(pipe.read(&mut chunk))
                .context("failed reading WASIX stdio pipe")?;
            if n == 0 {
                break;
            }
            sink.write_all(&chunk[..n])
                .context("failed writing drained WASIX stdio")?;
            sink.flush()
                .context("failed flushing drained WASIX stdio")?;
            captured.extend_from_slice(&chunk[..n]);
        }
        String::from_utf8(captured).context("WASIX stdio was not valid UTF-8")
    })
}

// ============================================================
// Runtime state shared between host and WASM guest
// ============================================================

struct RuntimeEnv {
    memory: Option<Memory>,
    malloc_fn: Option<TypedFunction<i32, i32>>,
    table: Option<Table>,
    /// Maps value handle IDs to their guest-memory data pointers.
    /// Used for buffers/arraybuffers backed by guest linear memory,
    /// since V8 sandbox remaps external ArrayBuffer pointers.
    guest_data_ptrs: std::collections::HashMap<u32, u32>,
}

fn make_store() -> Store {
    let mut features = Features::default();
    features.exceptions(true);
    let mut compiler = LLVM::default();
    compiler.opt_level(LLVMOptLevel::Less);
    let engine = EngineBuilder::new(compiler)
        .set_features(Some(features))
        .engine();
    Store::new(engine)
}

fn wasmer_cache_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join("target")
        .join("wasmer-cache")
}

fn load_or_compile_module(store: &Store, wasm_bytes: &[u8]) -> Result<Module> {
    let key = CacheHash::generate(wasm_bytes);
    let mut cache = FileSystemCache::new(wasmer_cache_dir())
        .context("failed to create/access Wasmer cache directory")?;

    if let Ok(module) = unsafe { cache.load(store, key) } {
        return Ok(module);
    }

    let module = Module::new(store, wasm_bytes).context("failed to compile wasm module")?;
    let _ = cache.store(key, &module);
    Ok(module)
}

// ============================================================
// Public API
// ============================================================

pub fn run_wasm_main_i32(wasm_path: &Path) -> Result<i32> {
    let wasm_bytes = std::fs::read(wasm_path)
        .with_context(|| format!("failed to read wasm file at {}", wasm_path.display()))?;
    let mut store = make_store();
    let module = load_or_compile_module(&store, &wasm_bytes)?;

    let memory_type = module
        .imports()
        .find_map(|import| {
            if import.module() == "env" && import.name() == "memory" {
                if let wasmer::ExternType::Memory(ty) = import.ty() {
                    return Some(*ty);
                }
            }
            None
        })
        .context("module does not import env.memory")?;
    let memory = Memory::new(&mut store, memory_type).context("failed to create memory")?;

    let func_env = FunctionEnv::new(
        &mut store,
        RuntimeEnv {
            memory: Some(memory.clone()),
            malloc_fn: None,
            table: None,
            guest_data_ptrs: std::collections::HashMap::new(),
        },
    );

    let mut import_object = imports! {
        "env" => {
            "memory" => memory,
        },
    };
    register_env_imports(&mut store, &mut import_object);
    register_napi_imports(&mut store, &func_env, &mut import_object);

    let instance = Instance::new(&mut store, &module, &import_object)
        .context("failed to instantiate wasm module")?;
    let main_fn: TypedFunction<(), i32> = instance
        .exports
        .get_typed_function(&store, "main")
        .context("no main export found")?;
    let result = main_fn.call(&mut store).unwrap_or(-1);
    Ok(result)
}

pub fn run_wasix_main_capture_stdio(
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String, String)> {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("failed to create tokio runtime for WASIX")?;
    let _guard = runtime.enter();

    let (stdout_tx, stdout_rx) = Pipe::channel();
    let (stderr_tx, stderr_rx) = Pipe::channel();
    let stdout_thread = spawn_pipe_drain_thread(stdout_rx, Box::new(std::io::stdout()));
    let stderr_thread = spawn_pipe_drain_thread(stderr_rx, Box::new(std::io::stderr()));
    let exit_code = {
        let wasm_bytes = std::fs::read(wasm_path)
            .with_context(|| format!("failed to read wasm file at {}", wasm_path.display()))?;
        let store = make_store();
        let engine = store.engine().clone();
        let module = load_or_compile_module(&store, &wasm_bytes)?;
        let module_hash = ModuleHash::sha256(&wasm_bytes);

        let imported_memory_type = module.imports().find_map(|import| {
            if import.module() == "env" && import.name() == "memory" {
                if let ExternType::Memory(ty) = import.ty() {
                    return Some(*ty);
                }
            }
            None
        });

        let imported_table_type = module.imports().find_map(|import| {
            if import.module() == "env" && import.name() == "__indirect_function_table" {
                if let ExternType::Table(ty) = import.ty() {
                    return Some(*ty);
                }
            }
            None
        });

        let mut runner = WasiRunner::new();
        runner
            .with_stdout(Box::new(stdout_tx))
            .with_stderr(Box::new(stderr_tx))
            .with_args(args.iter().cloned());
        runner
            .capabilities_mut()
            .threading
            .enable_asynchronous_threading = false;

        let repo_root = resolve_repo_root(&[wasm_path]);
        if repo_root.is_some() || !extra_mounts.is_empty() {
            let host_handle = tokio::runtime::Handle::current();
            if let Some(repo_root) = repo_root {
                let node_lib_dir = repo_root.join("node-lib");
                let node_lib_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                    virtual_fs::host_fs::FileSystem::new(host_handle.clone(), node_lib_dir.clone())
                        .with_context(|| {
                            format!("failed to create host fs for {}", node_lib_dir.display())
                        })?,
                );
                runner.with_mount("/node-lib".to_string(), node_lib_fs);

                let node_deps_dir = repo_root.join("node/deps");
                if node_deps_dir.is_dir() {
                    let node_deps_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                        virtual_fs::host_fs::FileSystem::new(
                            host_handle.clone(),
                            node_deps_dir.clone(),
                        )
                        .with_context(|| {
                            format!("failed to create host fs for {}", node_deps_dir.display())
                        })?,
                    );
                    runner.with_mount("/node/deps".to_string(), node_deps_fs);
                }
            }

            for mount in extra_mounts {
                let host_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                    virtual_fs::host_fs::FileSystem::new(
                        host_handle.clone(),
                        mount.host_path.clone(),
                    )
                    .with_context(|| {
                        format!("failed to create host fs for {}", mount.host_path.display())
                    })?,
                );
                runner.with_mount(mount.guest_path.display().to_string(), host_fs);
            }
        }

        let shared_func_env: Arc<Mutex<Option<FunctionEnv<RuntimeEnv>>>> =
            Arc::new(Mutex::new(None));
        let task_manager = Arc::new(TokioTaskManager::new(tokio::runtime::Handle::current()));
        let mut runtime = PluggableRuntime::new(task_manager);
        runtime.set_engine(engine.clone());
        let shared_func_env_for_imports = Arc::clone(&shared_func_env);
        runtime.with_additional_imports(move |store| {
            let mut import_object = Imports::new();
            register_env_imports(store, &mut import_object);

            let runtime_env = RuntimeEnv {
                memory: None,
                malloc_fn: None,
                table: None,
                guest_data_ptrs: std::collections::HashMap::new(),
            };
            let func_env = FunctionEnv::new(store, runtime_env);
            *shared_func_env_for_imports
                .lock()
                .expect("poisoned func env mutex") = Some(func_env.clone());
            register_napi_imports(store, &func_env, &mut import_object);

            if let Some(memory_type) = imported_memory_type {
                let memory = Memory::new(store, memory_type)?;
                import_object.define("env", "memory", memory.clone());
                func_env.as_mut(store).memory = Some(memory);
            }

            if let Some(table_type) = imported_table_type {
                let table = Table::new(store, table_type, Value::FuncRef(None))?;
                import_object.define("env", "__indirect_function_table", table.clone());
                func_env.as_mut(store).table = Some(table);
            }

            Ok(import_object)
        });
        let shared_func_env_for_instance = Arc::clone(&shared_func_env);
        runtime.with_instance_setup(move |store, instance| {
            let func_env = shared_func_env_for_instance
                .lock()
                .expect("poisoned func env mutex")
                .clone()
                .context("missing runtime function env during instance setup")?;

            for export_name in ["ubi_guest_malloc", "malloc"] {
                if let Ok(malloc) = instance
                    .exports
                    .get_typed_function::<i32, i32>(&*store, export_name)
                {
                    func_env.as_mut(store).malloc_fn = Some(malloc);
                    break;
                }
            }

            func_env.as_mut(store).table = instance
                .exports
                .get_table("__indirect_function_table")
                .ok()
                .cloned();
            set_top_level_callback_state(store, func_env.as_ref(store).table.clone());
            Ok(())
        });
        match runner.run_wasm(
            RuntimeOrEngine::Runtime(Arc::new(runtime)),
            "guest-test",
            module,
            module_hash,
        ) {
            Ok(()) => 0,
            Err(err) => {
                if let Some(WasiError::Exit(code)) = err.downcast_ref::<WasiError>() {
                    i32::from(*code)
                } else {
                    return Err(err).context("failed to run WASIX module through WasiRunner");
                }
            }
        }
    };

    let stdout = stdout_thread
        .join()
        .map_err(|_| anyhow::anyhow!("stdout drain thread panicked"))??;
    let stderr = stderr_thread
        .join()
        .map_err(|_| anyhow::anyhow!("stderr drain thread panicked"))??;
    Ok((exit_code, stdout, stderr))
}

pub fn run_wasix_main_capture_stdout(
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String)> {
    let (exit_code, stdout, _stderr) = run_wasix_main_capture_stdio(wasm_path, args, extra_mounts)?;
    Ok((exit_code, stdout))
}
