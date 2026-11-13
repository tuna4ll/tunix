use std::env;
use std::path::PathBuf;
use std::process::Command;

fn run(command: &mut Command) {
    let status = command.status().unwrap_or_else(|error| panic!("{command:?}: {error}"));
    assert!(status.success(), "{command:?} failed");
}

fn main() {
    println!("cargo:rerun-if-changed=reference.c");
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let compiler = env::var("CC").unwrap_or_else(|_| "cc".into());
    let object = out.join("reference.o");
    run(Command::new(compiler)
        .args([
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fPIC",
            "-c",
            "reference.c",
            "-o",
        ])
        .arg(&object));
    run(Command::new("ar").arg("crs").arg(out.join("libreference.a")).arg(&object));
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=reference");
}
