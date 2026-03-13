{
  description = "Development shell for building Ubi for WASIX";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    rust-overlay.url = "github:oxalica/rust-overlay";
  };

  outputs = { self, nixpkgs, rust-overlay }:
    let
      systems = [ "x86_64-linux" ];
      wasinixSource = builtins.fetchTree {
        type = "github";
        owner = "wasix-org";
        repo = "wasinix";
        rev = "723839b919d7c3644b1f2e5c60b256e65bbed365";
        narHash = "sha256-uMZkQJo+kjiZGMZPtVmhEQUe6xlZ6C8uiUmzrQADS/w=";
      };
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system:
          f {
            inherit system;
            pkgs = import nixpkgs {
              inherit system;
              overlays = [ (import rust-overlay) ];
            };
          });
    in {
      devShells = forAllSystems ({ system, pkgs }: {
        default =
          let
            wasinixPkgs = import (wasinixSource + "/pkgs") {
              inherit system nixpkgs;
            };
            rustToolchain =
              pkgs.rust-bin.fromRustupToolchainFile ./napi/wasmer/rust-toolchain.toml;
            v8Prebuilt = pkgs.stdenvNoCC.mkDerivation {
              pname = "ubi-v8-prebuilt";
              version = "11.9.2";
              src = pkgs.fetchurl {
                url = "https://github.com/wasmerio/v8-custom-builds/releases/download/11.9.2/v8-linux-amd64.tar.xz";
                hash = "sha256-nTCVdBKtyVMb7lE+Db4RDsShKkLbG/0r980ejd+EAvo=";
              };
              nativeBuildInputs = [ pkgs.clang pkgs.xz pkgs.gnutar ];
              dontUnpack = true;
              dontConfigure = true;
              dontBuild = true;
              installPhase = ''
                mkdir -p "$out"
                tar -xJf "$src" -C "$out"

                ${pkgs.clang}/bin/clang++ -shared \
                  -Wl,--whole-archive "$out/lib/libv8.a" -Wl,--no-whole-archive \
                  -o "$out/lib/libv8.so" \
                  -ldl -pthread -lm -lrt || true

                cat > "$out/lib/v8_libplatform_stub.cc" <<'EOF'
                extern "C" void __dummy_v8_libplatform() {}
                EOF
                ${pkgs.clang}/bin/clang++ -shared \
                  "$out/lib/v8_libplatform_stub.cc" \
                  -o "$out/lib/libv8_libplatform.so"

                cat > "$out/lib/v8_libbase_stub.cc" <<'EOF'
                extern "C" void __dummy_v8_libbase() {}
                EOF
                ${pkgs.clang}/bin/clang++ -shared \
                  "$out/lib/v8_libbase_stub.cc" \
                  -o "$out/lib/libv8_libbase.so"
              '';
            };
          in
          pkgs.mkShell {
            packages = [
              wasinixPkgs.toolchains.${wasinixPkgs.defaultProfileName}.wasixcc
              pkgs.bash
              pkgs.binaryen
              pkgs.cmake
              pkgs.coreutils
              pkgs.curl
              pkgs.findutils
              pkgs.gawk
              pkgs.git
              pkgs.gnumake
              pkgs.gnugrep
              pkgs.gnused
              pkgs.libffi
              pkgs.libxml2
              pkgs.llvmPackages_21.clang
              pkgs.llvmPackages_21.libclang.dev
              pkgs.llvmPackages_21.libllvm
              pkgs.llvmPackages_21.llvm
              pkgs.llvmPackages_21.llvm.dev
              pkgs.ninja
              pkgs.openssl
              pkgs.perl
              pkgs.pkg-config
              pkgs.python3
              (rustToolchain.override {
                targets = [ "wasm32-unknown-unknown" ];
                extensions = [ "rust-src" ];
              })
              v8Prebuilt
              pkgs.wabt
              pkgs.xz
            ];

            shellHook = ''
              export UBI_REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
              export V8_VERSION="11.9.2"
              export V8_ROOT_DEFAULT="${v8Prebuilt}"

              export LLVM_SYS_211_PREFIX="${pkgs.llvmPackages_21.llvm.dev}"
              export LIBCLANG_PATH="${pkgs.llvmPackages_21.libclang.lib}/lib"
              export LIBRARY_PATH="${pkgs.llvmPackages_21.compiler-rt-libc}/lib/linux:$LIBRARY_PATH"
              export LD_LIBRARY_PATH="${pkgs.llvmPackages_21.compiler-rt-libc}/lib/linux:$LD_LIBRARY_PATH"
              export BINDGEN_EXTRA_CLANG_ARGS="$(
                    < ${pkgs.llvmPackages_21.stdenv.cc}/nix-support/libc-crt1-cflags
                  ) $(
                    < ${pkgs.llvmPackages_21.stdenv.cc}/nix-support/libc-cflags
                  ) $(
                    < ${pkgs.llvmPackages_21.stdenv.cc}/nix-support/cc-cflags
                  ) $(
                    < ${pkgs.llvmPackages_21.stdenv.cc}/nix-support/libcxx-cxxflags
                  ) \
                  -isystem ${pkgs.glibc.dev}/include \
                  -idirafter ${pkgs.llvmPackages_21.clang}/lib/clang/${pkgs.lib.getVersion pkgs.llvmPackages_21.clang}/include"
              export V8_INCLUDE_DIR="''${V8_INCLUDE_DIR:-$V8_ROOT_DEFAULT/include}"
              export V8_LIB_DIR="''${V8_LIB_DIR:-$V8_ROOT_DEFAULT/lib}"
              export V8_DEFINES="''${V8_DEFINES:-V8_COMPRESS_POINTERS}"
              export LD_LIBRARY_PATH="$V8_LIB_DIR:$LD_LIBRARY_PATH"
              echo "WASIX build shell ready."
              echo "Try: ubi/wasix/build-wasix.sh"
              echo "Try: cargo build --manifest-path napi/wasmer/Cargo.toml"
            '';
          };
      });
    };
}
