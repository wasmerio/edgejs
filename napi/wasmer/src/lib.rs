mod guest;
mod snapi;

use anyhow::{Context, Result};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use virtual_fs::{AsyncReadExt, FileSystem, RootFileSystemBuilder, TmpFileSystem};
use wasmer::Table;
use wasmer::{
    imports,
    sys::{EngineBuilder, Features},
    FunctionEnv, Instance, Memory, Module, Store, TypedFunction,
};
use wasmer_cache::{Cache, FileSystemCache, Hash as CacheHash};
use wasmer_compiler_llvm::{LLVMOptLevel, LLVM};
use wasmer_wasix::{Pipe, WasiEnv, WasiError};

use crate::guest::callback::with_top_level_callback_state;
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

fn ensure_guest_dir(root_fs: &TmpFileSystem, dir: &Path) {
    let mut current = PathBuf::new();
    for component in dir.components() {
        current.push(component.as_os_str());
        if current == Path::new("/") {
            continue;
        }
        let _ = root_fs.create_dir(&current);
    }
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
        let mut store = make_store();
        let module = load_or_compile_module(&store, &wasm_bytes)?;
        let mut builder = WasiEnv::builder("guest-test")
            .engine(store.engine().clone())
            .stdout(Box::new(stdout_tx))
            .stderr(Box::new(stderr_tx));
        if !args.is_empty() {
            builder = builder.args(args.iter().map(|s| s.as_str()));
        }
        let repo_root = resolve_repo_root(&[wasm_path]);
        if repo_root.is_some() || !extra_mounts.is_empty() {
            let mut mapped_dirs = Vec::new();
            if repo_root.is_some() {
                mapped_dirs.push("/node-lib");
                mapped_dirs.push("/node/deps");
            }
            for mount in extra_mounts {
                if let Some(path) = mount.guest_path.to_str() {
                    mapped_dirs.push(path);
                }
            }
            let root_fs = RootFileSystemBuilder::default().build_ext(&mapped_dirs);
            let host_handle = tokio::runtime::Handle::current();
            if let Some(repo_root) = repo_root {
                let node_lib_dir = repo_root.join("node-lib");
                let node_lib_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                    virtual_fs::host_fs::FileSystem::new(host_handle.clone(), node_lib_dir.clone())
                        .with_context(|| {
                            format!("failed to create host fs for {}", node_lib_dir.display())
                        })?,
                );
                TmpFileSystem::mount(&root_fs, "/node-lib".into(), &node_lib_fs, "/".into())
                    .with_context(|| {
                        format!("failed to mount {} at /node-lib", node_lib_dir.display())
                    })?;

                let node_deps_dir = repo_root.join("node/deps");
                if node_deps_dir.is_dir() {
                    let _ = root_fs.create_dir(Path::new("/node"));
                    let node_deps_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                        virtual_fs::host_fs::FileSystem::new(
                            host_handle.clone(),
                            node_deps_dir.clone(),
                        )
                        .with_context(|| {
                            format!("failed to create host fs for {}", node_deps_dir.display())
                        })?,
                    );
                    TmpFileSystem::mount(&root_fs, "/node/deps".into(), &node_deps_fs, "/".into())
                        .with_context(|| {
                            format!("failed to mount {} at /node/deps", node_deps_dir.display())
                        })?;
                }
            }

            for mount in extra_mounts {
                let guest_path = mount.guest_path.clone();
                let guest_parent = guest_path.parent().unwrap_or_else(|| Path::new("/"));
                if guest_parent != Path::new("/") {
                    ensure_guest_dir(&root_fs, guest_parent);
                }
                let host_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                    virtual_fs::host_fs::FileSystem::new(
                        host_handle.clone(),
                        mount.host_path.clone(),
                    )
                    .with_context(|| {
                        format!("failed to create host fs for {}", mount.host_path.display())
                    })?,
                );
                TmpFileSystem::mount(&root_fs, guest_path.clone(), &host_fs, "/".into())
                    .with_context(|| {
                        format!(
                            "failed to mount {} at {}",
                            mount.host_path.display(),
                            guest_path.display()
                        )
                    })?;
            }
            builder = builder.sandbox_fs(root_fs);
            builder.add_preopen_dir("/")?;
        }

        let mut wasi_env = builder
            .finalize(&mut store)
            .context("failed to finalize WASIX environment")?;
        let mut import_object = wasi_env
            .import_object_for_all_wasi_versions(&mut store, &module)
            .context("failed to generate WASIX imports")?;

        let imported_memory_type = module
            .imports()
            .find_map(|import| {
                if import.module() == "env" && import.name() == "memory" {
                    if let wasmer::ExternType::Memory(ty) = import.ty() {
                        return Some(*ty);
                    }
                }
                None
            })
            .context("WASIX module does not import env.memory")?;
        let memory = Memory::new(&mut store, imported_memory_type)
            .context("failed to create imported WASIX memory")?;
        import_object.define("env", "memory", memory.clone());
        register_env_imports(&mut store, &mut import_object);

        let func_env = FunctionEnv::new(
            &mut store,
            RuntimeEnv {
                memory: Some(memory.clone()),
                malloc_fn: None,
                table: None,
                guest_data_ptrs: std::collections::HashMap::new(),
            },
        );
        register_napi_imports(&mut store, &func_env, &mut import_object);

        let instance = Instance::new(&mut store, &module, &import_object)
            .context("failed to instantiate WASIX wasm module")?;

        // Store guest allocator for guest-memory-backed ArrayBuffers.
        for export_name in ["ubi_guest_malloc", "malloc"] {
            if let Ok(malloc) = instance
                .exports
                .get_typed_function::<i32, i32>(&store, export_name)
            {
                func_env.as_mut(&mut store).malloc_fn = Some(malloc);
                break;
            }
        }

        func_env.as_mut(&mut store).table = instance
            .exports
            .get_table("__indirect_function_table")
            .ok()
            .cloned();

        let wasi_handles = wasmer_wasix::WasiModuleTreeHandles::Static(
            wasmer_wasix::WasiModuleInstanceHandles::new(
                memory.clone(),
                &store,
                instance.clone(),
                func_env.as_ref(&store).table.clone(),
            ),
        );
        wasi_env
            .initialize_handles_and_layout(&mut store, instance.clone(), wasi_handles, None, true)
            .context("failed to initialize WASIX environment")?;

        let callback_table = func_env.as_ref(&store).table.clone();
        let exit = if let Ok(main) = instance
            .exports
            .get_typed_function::<(i32, i32), i32>(&mut store, "main")
        {
            with_top_level_callback_state(&mut store, callback_table.clone(), |store| {
                if let Ok(ctors) = instance
                    .exports
                    .get_typed_function::<(), ()>(store, "__wasm_call_ctors")
                {
                    let _ = ctors.call(store);
                }
                main.call(store, 0, 0)
            })
            .map_err(|err| anyhow::anyhow!("WASIX `main` call failed: {err:?}"))?
        } else {
            let start: TypedFunction<(), ()> = instance
                .exports
                .get_typed_function(&mut store, "_start")
                .context("failed to find export `_start`")?;
            let exit = match with_top_level_callback_state(&mut store, callback_table, |store| {
                start.call(store)
            }) {
                Ok(()) => 0,
                Err(err) => {
                    if let Some(WasiError::Exit(code)) = err.downcast_ref::<WasiError>() {
                        i32::from(*code)
                    } else {
                        return Err(anyhow::anyhow!("WASIX `_start` failed: {err}"));
                    }
                }
            };
            drop(start);
            exit
        };

        drop(instance);
        drop(import_object);
        drop(wasi_env);
        drop(store);
        exit
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
