#!/usr/bin/env python3
"""Build wrapper for deadlyprem: rexglue codegen -> cmake configure -> cmake build.

Adapted from EternalSonataReprise. By default uses the locally-installed
ReXGlue SDK at ../rexglue-sdk/out/install/win-amd64; override with --sdk-dir
to point at a downloaded nightly archive.
"""
import os
import sys
import glob
import platform
import shutil
import subprocess
import tomllib


def detect_preset(build_type="release"):
    os_name = platform.system()
    arch = platform.machine().lower()
    os_id = {"Linux": "linux", "Windows": "win"}.get(os_name)
    if os_id is None:
        raise RuntimeError(f"Unsupported OS: {os_name}")
    arch_id = {"x86_64": "amd64", "amd64": "amd64",
               "aarch64": "arm64", "arm64": "arm64"}.get(arch)
    if arch_id is None:
        raise RuntimeError(f"Unsupported architecture: {arch}")
    return f"{os_id}-{arch_id}-{build_type}"


def find_clangxx():
    if platform.system() != "Windows":
        for v in range(30, 17, -1):
            if shutil.which(f"clang++-{v}"):
                return f"clang++-{v}"
    if shutil.which("clang++"):
        return "clang++"
    sys.exit("error: no clang++ compiler found in PATH")


def check_deps():
    missing = [d for d in ("cmake", "ninja") if shutil.which(d) is None]
    if missing:
        sys.exit(f"error: missing required tool(s): {', '.join(missing)}")


def run(args, **kw):
    print(f"+ {' '.join(str(a) for a in args)}")
    r = subprocess.run(args, **kw)
    if r.returncode != 0:
        sys.exit(r.returncode)


def copy_runtime_libs(is_windows, sdk_dir):
    src_dir = os.path.join(sdk_dir, "bin" if is_windows else "lib")
    suffix = ".dll" if is_windows else ".so"
    if not os.path.isdir(src_dir):
        return
    for name in os.listdir(src_dir):
        if name.endswith(suffix):
            src = os.path.join(src_dir, name)
            print(f"+ cp {src} {name}")
            shutil.copy2(src, name)


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--sdk-dir",
                   default=os.path.normpath(os.path.join(
                       os.path.dirname(os.path.abspath(__file__)),
                       "..", "..", "rexglue-sdk", "out", "install", "win-amd64")),
                   help="Path to the ReXGlue SDK install root")
    p.add_argument("--debug", action="store_true",
                   help="Build with debug symbols (RelWithDebInfo preset)")
    p.add_argument("--skip-codegen", action="store_true",
                   help="Skip 'rexglue codegen'; only configure + build")
    args = p.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(script_dir, ".."))
    os.chdir(root)

    is_windows = platform.system() == "Windows"

    manifests = glob.glob("*_manifest.toml")
    if len(manifests) != 1:
        sys.exit(f"error: expected exactly one *_manifest.toml in repo root, found: {manifests}")
    manifest_path = manifests[0]
    with open(manifest_path, "rb") as f:
        manifest = tomllib.load(f)
    project_name = manifest["project"]["name"]
    xex_path = manifest["entrypoint"]["file_path"]

    if not os.path.exists(xex_path):
        sys.exit(f"error: XEX not found at '{xex_path}' — drop default.xex there first")

    check_deps()

    sdk_dir = args.sdk_dir
    rexglue_bin = "rexglue.exe" if is_windows else "rexglue"
    rexglue = os.path.join(sdk_dir, "bin", rexglue_bin)
    if not os.path.exists(rexglue):
        sys.exit(f"error: rexglue binary not found at {rexglue} — pass --sdk-dir to override")

    preset = detect_preset("relwithdebinfo" if args.debug else "release")
    exe_name = f"{project_name}.exe" if is_windows else project_name

    if not args.skip_codegen:
        run([rexglue, "codegen", manifest_path])

    cxx = find_clangxx()
    cfg = [f"-DCMAKE_PREFIX_PATH={sdk_dir}", f"-DCMAKE_CXX_COMPILER={cxx}"]
    if shutil.which("sccache"):
        cfg += ["-DCMAKE_CXX_COMPILER_LAUNCHER=sccache"]

    run(["cmake", "--preset", preset, *cfg])
    run(["cmake", "--build", "--preset", preset,
         "--parallel", str(os.cpu_count() or 1)])

    build_out = os.path.join("out", "build", preset, exe_name)
    print(f"+ cp {build_out} {exe_name}")
    shutil.copy2(build_out, exe_name)
    copy_runtime_libs(is_windows, sdk_dir)


if __name__ == "__main__":
    main()
