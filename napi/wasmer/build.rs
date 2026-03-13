fn main() {
    let project_root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap();

    let edge_src = project_root.join("src");
    let libuv_include = project_root.join("deps/uv/include");

    // napi/v8 paths
    let napi_v8_dir = project_root.join("napi/v8");
    let napi_include = project_root.join("napi/include");
    let napi_v8_src = napi_v8_dir.join("src");

    // V8 paths
    let default_v8_include = "/opt/homebrew/Cellar/v8/14.5.201.9/include".to_string();
    let default_v8_lib = "/opt/homebrew/Cellar/v8/14.5.201.9/lib".to_string();
    let v8_include = std::env::var("V8_INCLUDE_DIR")
        .or_else(|_| std::env::var("NAPI_V8_INCLUDE_DIR"))
        .unwrap_or(default_v8_include);
    let v8_lib = std::env::var("V8_LIB_DIR")
        .or_else(|_| {
            std::env::var("NAPI_V8_LIBRARY").map(|path| {
                std::path::Path::new(&path)
                    .parent()
                    .map(|dir| dir.to_string_lossy().into_owned())
                    .unwrap_or(path)
            })
        })
        .unwrap_or(default_v8_lib);

    let v8_defines = std::env::var("V8_DEFINES")
        .or_else(|_| std::env::var("NAPI_V8_DEFINES"))
        .unwrap_or_else(|_| "V8_COMPRESS_POINTERS".to_string());

    // Compile the napi/v8 sources + bridge into a single static library.
    // Keep V8 feature defines aligned with the selected V8 binary.
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .flag_if_supported("-std=c++20")
        .flag_if_supported("-w")
        .define("NAPI_EXTERN", Some(""))
        .include(&v8_include)
        .include(edge_src.to_str().unwrap())
        .include(libuv_include.to_str().unwrap())
        .include(napi_include.to_str().unwrap())
        .include(napi_v8_src.to_str().unwrap())
        .file("src/napi_bridge_init.cc")
        .file(edge_src.join("edge_environment.cc").to_str().unwrap())
        .file(napi_v8_src.join("js_native_api_v8.cc").to_str().unwrap())
        .file(napi_v8_src.join("unofficial_napi.cc").to_str().unwrap())
        .file(napi_v8_src.join("unofficial_napi_error_utils.cc").to_str().unwrap())
        .file(napi_v8_src.join("unofficial_napi_contextify.cc").to_str().unwrap())
        .file(napi_v8_src.join("edge_v8_platform.cc").to_str().unwrap());

    for raw in v8_defines.split(&[';', ',', ' '][..]) {
        let entry = raw.trim();
        if entry.is_empty() {
            continue;
        }
        if let Some((name, value)) = entry.split_once('=') {
            build.define(name.trim(), Some(value.trim()));
        } else {
            build.define(entry, Some("1"));
        }
    }

    build.compile("napi_bridge");

    println!("cargo:rerun-if-changed=src/napi_bridge_init.cc");
    println!("cargo:rustc-link-search=native={v8_lib}");
    println!("cargo:rustc-link-lib=dylib=v8");
    println!("cargo:rustc-link-lib=dylib=v8_libplatform");
    println!("cargo:rustc-link-lib=dylib=v8_libbase");
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "macos" || target_os == "ios" {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
}
