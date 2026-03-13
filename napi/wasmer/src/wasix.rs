use anyhow::{Context, Result};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use virtual_fs::{AsyncReadExt, FileSystem};
use wasmer_wasix::{
    runners::wasi::{RuntimeOrEngine, WasiRunner},
    runtime::task_manager::tokio::TokioTaskManager,
    Pipe, PluggableRuntime, WasiError,
};

use crate::{load_wasix_module, NapiCtx};

#[derive(Debug, Clone)]
pub struct GuestMount {
    pub host_path: PathBuf,
    pub guest_path: PathBuf,
}

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
        if (root.join("lib").is_dir() || root.join("node-lib").is_dir())
            && root.join("node").is_dir()
        {
            return std::fs::canonicalize(&root).ok().or(Some(root));
        }
    }
    None
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

pub fn configure_runner_mounts(
    runner: &mut WasiRunner,
    wasm_path: &Path,
    extra_mounts: &[GuestMount],
) -> Result<()> {
    let repo_root = resolve_repo_root(&[wasm_path]);
    if repo_root.is_none() && extra_mounts.is_empty() {
        return Ok(());
    }

    let host_handle = tokio::runtime::Handle::current();
    if let Some(repo_root) = repo_root {
        let lib_dir = if repo_root.join("lib").is_dir() {
            repo_root.join("lib")
        } else {
            repo_root.join("node-lib")
        };
        let lib_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
            virtual_fs::host_fs::FileSystem::new(host_handle.clone(), lib_dir.clone())
                .with_context(|| format!("failed to create host fs for {}", lib_dir.display()))?,
        );
        runner.with_mount("/lib".to_string(), lib_fs.clone());
        runner.with_mount("/node-lib".to_string(), lib_fs);

        let node_deps_dir = repo_root.join("node/deps");
        if node_deps_dir.is_dir() {
            let node_deps_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
                virtual_fs::host_fs::FileSystem::new(host_handle.clone(), node_deps_dir.clone())
                    .with_context(|| {
                        format!("failed to create host fs for {}", node_deps_dir.display())
                    })?,
            );
            runner.with_mount("/node/deps".to_string(), node_deps_fs);
        }
    }

    for mount in extra_mounts {
        let host_fs: Arc<dyn FileSystem + Send + Sync> = Arc::new(
            virtual_fs::host_fs::FileSystem::new(host_handle.clone(), mount.host_path.clone())
                .with_context(|| {
                    format!("failed to create host fs for {}", mount.host_path.display())
                })?,
        );
        runner.with_mount(mount.guest_path.display().to_string(), host_fs);
    }

    Ok(())
}

pub fn run_wasix_main_capture_stdio(
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String, String)> {
    let ctx = NapiCtx::default();
    run_wasix_main_capture_stdio_with_ctx(&ctx, wasm_path, args, extra_mounts)
}

pub fn run_wasix_main_capture_stdio_with_ctx(
    ctx: &NapiCtx,
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
        let loaded = load_wasix_module(wasm_path)?;
        let engine = loaded.store.engine().clone();
        let module = loaded.module;
        let module_hash = loaded.module_hash;

        let mut runner = WasiRunner::new();
        runner
            .with_stdout(Box::new(stdout_tx))
            .with_stderr(Box::new(stderr_tx))
            .with_args(args.iter().cloned());
        runner
            .capabilities_mut()
            .threading
            .enable_asynchronous_threading = false;
        configure_runner_mounts(&mut runner, wasm_path, extra_mounts)?;

        let task_manager = Arc::new(TokioTaskManager::new(tokio::runtime::Handle::current()));
        let mut runtime = PluggableRuntime::new(task_manager);
        runtime.set_engine(engine.clone());
        let _session = ctx.configure_runtime(&mut runtime, &module)?;

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
    let ctx = NapiCtx::default();
    run_wasix_main_capture_stdout_with_ctx(&ctx, wasm_path, args, extra_mounts)
}

pub fn run_wasix_main_capture_stdout_with_ctx(
    ctx: &NapiCtx,
    wasm_path: &Path,
    args: &[String],
    extra_mounts: &[GuestMount],
) -> Result<(i32, String)> {
    let (exit_code, stdout, _stderr) =
        run_wasix_main_capture_stdio_with_ctx(ctx, wasm_path, args, extra_mounts)?;
    Ok((exit_code, stdout))
}
