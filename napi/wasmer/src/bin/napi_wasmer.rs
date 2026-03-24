use anyhow::{Context, Result, anyhow, bail};
use napi_wasmer::{
    NapiCtx,
    cli::{GuestMount, run_wasix_main_capture_stdio_with_ctx},
};
use std::path::{Path, PathBuf};

const BUILTIN_JS_GUEST_PATH: &str = "/edgejs-builtins";
const BUILTIN_JS_ENV_VAR: &str = "WASMER_NAPI_BUILTIN_JS_DIR";

fn maybe_add_builtin_mounts(
    extra_mounts: &mut Vec<GuestMount>,
    explicit_builtin_dir: Option<String>,
) -> Result<()> {
    let explicit_builtin_dir =
        explicit_builtin_dir.or_else(|| std::env::var(BUILTIN_JS_ENV_VAR).ok());
    let builtin_dir = if let Some(dir) = explicit_builtin_dir {
        let path = std::fs::canonicalize(&dir)
            .with_context(|| format!("failed to resolve builtin js dir {}", dir))?;
        if !path.is_dir() {
            bail!("builtin js dir must be a directory: {}", path.display());
        }
        path
    } else {
        let repo_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
        let lib = repo_root.join("lib");
        if lib.is_dir() {
            std::fs::canonicalize(&lib).ok().unwrap_or(lib)
        } else {
            let node_lib = repo_root.join("node-lib");
            if node_lib.is_dir() {
                std::fs::canonicalize(&node_lib).ok().unwrap_or(node_lib)
            } else {
                bail!(
                    "builtin js dir is not configured: neither {} nor {} exists under {}",
                    repo_root.join("lib").display(),
                    repo_root.join("node-lib").display(),
                    repo_root.display()
                );
            }
        }
    };

    if !extra_mounts
        .iter()
        .any(|mount| mount.guest_path == Path::new(BUILTIN_JS_GUEST_PATH))
    {
        extra_mounts.push(GuestMount {
            host_path: builtin_dir.clone(),
            guest_path: PathBuf::from(BUILTIN_JS_GUEST_PATH),
        });
    }

    if !extra_mounts
        .iter()
        .any(|mount| mount.guest_path == Path::new("/lib"))
    {
        extra_mounts.push(GuestMount {
            host_path: builtin_dir.clone(),
            guest_path: PathBuf::from("/lib"),
        });
    }

    if !extra_mounts
        .iter()
        .any(|mount| mount.guest_path == Path::new("/node-lib"))
    {
        extra_mounts.push(GuestMount {
            host_path: builtin_dir.clone(),
            guest_path: PathBuf::from("/node-lib"),
        });
    }

    if let Some(parent) = builtin_dir.parent() {
        let node_deps_dir = parent.join("node/deps");
        if node_deps_dir.is_dir()
            && !extra_mounts
                .iter()
                .any(|mount| mount.guest_path == Path::new("/node/deps"))
        {
            extra_mounts.push(GuestMount {
                host_path: node_deps_dir,
                guest_path: PathBuf::from("/node/deps"),
            });
        }
    }

    Ok(())
}

fn parse_mount(spec: &str) -> Result<GuestMount> {
    let (host, guest) = spec
        .split_once(':')
        .ok_or_else(|| anyhow!("invalid mount {spec:?}, expected <host-dir>:<guest-dir>"))?;
    let host_path = std::fs::canonicalize(host)
        .with_context(|| format!("failed to resolve host mount path {}", host))?;
    if !host_path.is_dir() {
        bail!("mount source must be a directory: {}", host_path.display());
    }
    let guest_path = PathBuf::from(guest);
    if !guest_path.is_absolute() {
        bail!(
            "mount target must be an absolute guest path: {}",
            guest_path.display()
        );
    }
    Ok(GuestMount {
        host_path,
        guest_path,
    })
}

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let wasm_path = match args.next() {
        Some(path) => PathBuf::from(path),
        None => {
            bail!(
                "usage: napi_wasmer <wasm-file> [<script.js>] [--builtin-js-dir <host-dir>] [--app-dir <host-dir>] [--mount <host-dir>:<guest-dir>]"
            );
        }
    };

    let mut script_arg: Option<String> = None;
    let mut builtin_js_dir: Option<String> = None;
    let mut extra_mounts = Vec::new();

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--app-dir" => {
                let host_dir = args
                    .next()
                    .ok_or_else(|| anyhow!("--app-dir requires a host directory"))?;
                let host_path = std::fs::canonicalize(&host_dir)
                    .with_context(|| format!("failed to resolve app dir {}", host_dir))?;
                if !host_path.is_dir() {
                    bail!("app dir must be a directory: {}", host_path.display());
                }
                extra_mounts.push(GuestMount {
                    host_path,
                    guest_path: PathBuf::from("/app"),
                });
            }
            "--mount" => {
                let spec = args
                    .next()
                    .ok_or_else(|| anyhow!("--mount requires <host-dir>:<guest-dir>"))?;
                extra_mounts.push(parse_mount(&spec)?);
            }
            "--builtin-js-dir" => {
                builtin_js_dir = Some(
                    args.next()
                        .ok_or_else(|| anyhow!("--builtin-js-dir requires a host directory"))?,
                );
            }
            _ if arg.starts_with("--mount=") => {
                extra_mounts.push(parse_mount(arg.trim_start_matches("--mount="))?);
            }
            _ if arg.starts_with("--builtin-js-dir=") => {
                builtin_js_dir = Some(arg.trim_start_matches("--builtin-js-dir=").to_string());
            }
            _ if arg.starts_with("--app-dir=") => {
                let host_dir = arg.trim_start_matches("--app-dir=");
                let host_path = std::fs::canonicalize(host_dir)
                    .with_context(|| format!("failed to resolve app dir {}", host_dir))?;
                if !host_path.is_dir() {
                    bail!("app dir must be a directory: {}", host_path.display());
                }
                extra_mounts.push(GuestMount {
                    host_path,
                    guest_path: PathBuf::from("/app"),
                });
            }
            _ if script_arg.is_none() => script_arg = Some(arg),
            _ => bail!("unexpected argument: {arg}"),
        }
    }

    let mut guest_args = Vec::new();
    if let Some(script) = script_arg {
        let host_script = PathBuf::from(&script);
        let host_script = if host_script.is_absolute() {
            host_script
        } else {
            std::env::current_dir()
                .context("failed to resolve current dir")?
                .join(host_script)
        };
        let host_script = std::fs::canonicalize(&host_script)
            .with_context(|| format!("failed to resolve script {}", script))?;
        let script_parent = host_script
            .parent()
            .ok_or_else(|| anyhow!("script has no parent dir: {}", host_script.display()))?;
        if !extra_mounts
            .iter()
            .any(|mount| mount.guest_path == Path::new("/app"))
        {
            extra_mounts.push(GuestMount {
                host_path: script_parent.to_path_buf(),
                guest_path: PathBuf::from("/app"),
            });
        }
        let script_name = host_script
            .file_name()
            .ok_or_else(|| anyhow!("script has no file name: {}", host_script.display()))?;
        guest_args.push(format!("/app/{}", script_name.to_string_lossy()));
        maybe_add_builtin_mounts(&mut extra_mounts, builtin_js_dir)?;
    }

    let ctx = NapiCtx::default();
    let (exit_code, _stdout, _stderr) =
        run_wasix_main_capture_stdio_with_ctx(&ctx, &wasm_path, &guest_args, &extra_mounts)?;

    if exit_code != 0 {
        std::process::exit(exit_code);
    }

    Ok(())
}
