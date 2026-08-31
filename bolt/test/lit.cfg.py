# -*- Python -*-

import os
import platform
import re
import subprocess
import tempfile

import lit.formats
import lit.util

from lit.llvm import llvm_config
from lit.llvm.subst import ToolSubst
from lit.llvm.subst import FindTool

# Configuration file for the 'lit' test runner.

# name: The name of this test suite.
config.name = "BOLT"

# testFormat: The test format to use to interpret tests.
#
# For now we require '&&' between commands, until they get globally killed and
# the test runner updated.
config.test_format = lit.formats.ShTest(not llvm_config.use_lit_shell)

# suffixes: A list of file extensions to treat as test files.
config.suffixes = [
    ".c",
    ".cpp",
    ".cppm",
    ".m",
    ".mm",
    ".cu",
    ".ll",
    ".cl",
    ".s",
    ".S",
    ".modulemap",
    ".test",
    ".rs",
]

# excludes: A list of directories to exclude from the testsuite. The 'Inputs'
# subdirectories contain auxiliary inputs for various tests in their parent
# directories.
config.excludes = ["Inputs", "CMakeLists.txt", "README.txt", "LICENSE.txt"]

# test_source_root: The root path where tests are located.
config.test_source_root = os.path.dirname(__file__)

# test_exec_root: The root path where tests should be run.
config.test_exec_root = os.path.join(config.bolt_obj_root, "test")

# checking if maxIndividualTestTime is available on the platform and sets
# it to 60sec if so, declares lit-max-individual-test-time feature for
# further checking by tests.
supported, errormsg = lit_config.maxIndividualTestTimeIsSupported
if supported:
    config.available_features.add("lit-max-individual-test-time")
    lit_config.maxIndividualTestTime = 60
else:
    lit_config.warning(
        "Setting a timeout per test not supported. "
        + errormsg
        + " Some tests will be skipped."
    )

if config.bolt_enable_runtime:
    config.available_features.add("bolt-runtime")

if config.gnu_ld:
    config.available_features.add("gnu_ld")

if lit.util.which("fuser"):
    config.available_features.add("fuser")

# Feature for tests that execute an x86_64 binary: cross-compilation alone
# is not enough, the host must be able to run the result.
if config.host_arch in ["x86", "X86", "x86_64"]:
    config.available_features.add("x86_64-host")

llvm_config.use_default_substitutions()

llvm_config.config.environment["CLANG"] = config.bolt_clang
llvm_config.use_clang()

def check_mappingsymbol_support(gc_path):
    # Build a real one-file module with the flag: a successful compilation
    # proves both that the toolchain accepts -mappingsymbol and that it
    # compiles with it. (Probing via stdin "-" requires module mode and
    # fails for unrelated reasons; matching on the error text enabled the
    # feature falsely on toolchains without the flag.)
    try:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            with open(td + '/go.mod', 'w') as f:
                f.write("module probe\n\ngo 1.24\n")
            with open(td + '/main.go', 'w') as f:
                f.write("package main\n\nfunc main() {}\n")
            result = subprocess.run(
                [gc_path, "build", "-mappingsymbol", "-o=/dev/null", "."],
                cwd=td, capture_output=True, text=True, timeout=30)
            return result.returncode == 0
    except:
        pass
    return False

# Go compiler configuration for BOLT golang tests
gc_path = None
gc_version = None

# Priority 1: CMake-configured GO_EXECUTABLE
if hasattr(config, 'go_executable') and config.go_executable:
    gc_path = config.go_executable
    lit_config.note(f'BOLT Go: Using GO_EXECUTABLE: {gc_path}')

# Priority 2: BOLT-specific detection
if not gc_path and hasattr(config, 'bolt_gc_path') and config.bolt_gc_path:
    gc_path = config.bolt_gc_path
    lit_config.note(f'BOLT Go: Using bolt_gc_path: {gc_path}')

# Priority 3: System PATH
if not gc_path:
    try:
        result = subprocess.run(["which", "go"], capture_output=True, text=True, check=True, timeout=30)
        if result.stdout.strip():
            gc_path = result.stdout.strip()
            lit_config.note(f'BOLT Go: Using system Go: {gc_path}')
    except:
        pass

# Detect Go version if found
if gc_path and os.path.exists(gc_path):
    try:
        result = subprocess.run([gc_path, "version"], capture_output=True, text=True, check=True, timeout=30)
        match = re.search(r'go([0-9]+\.[0-9]+)', result.stdout)
        if match:
            gc_version = match.group(1)
    except:
        pass

# Add %gc substitution and composite gc124 feature
if gc_path and os.path.exists(gc_path):
    config.available_features.add("gc")
    llvm_config.add_tool_substitutions([ToolSubst(r'%gc', gc_path)])

    # Set environment for Go. The go binary directory is prepended to
    # PATH so a toolchain found via GO_EXECUTABLE/BOLT_GC_PATH that is
    # not on PATH still works for tests invoking plain `go` and for the
    # external linker driver.
    config.environment['GO111MODULE'] = 'off'
    config.environment['GOCACHE'] = tempfile.gettempdir()
    go_bin_dir = os.path.dirname(gc_path)
    if go_bin_dir and go_bin_dir not in config.environment.get('PATH', ''):
        config.environment['PATH'] = go_bin_dir + os.pathsep + \
            config.environment.get('PATH', os.environ.get('PATH', ''))

    # Composite gc124 feature: platform-specific requirements
    if gc_version:
        gc_parts = gc_version.split('.')
        major, minor = int(gc_parts[0]), int(gc_parts[1])
        is_exactly_124 = (major == 1 and minor == 24)

        if platform.machine() == 'x86_64':
            if is_exactly_124:
                config.available_features.add("gc124")
                lit_config.note(f'BOLT Go: X86_64 requirements met (gc124: go {gc_version})')
            else:
                lit_config.note(f'BOLT Go: Go version {gc_version} != 1.24 for x86_64')

        elif platform.machine() == 'aarch64':
            mappingsymbol_ok = check_mappingsymbol_support(gc_path)

            if is_exactly_124 and mappingsymbol_ok:
                config.available_features.add("gc124")
                lit_config.note(f'BOLT Go: ARM64 requirements met (gc124: go {gc_version}, mappingsymbol)')
            else:
                reasons = []
                if not is_exactly_124:
                    reasons.append(f"go {gc_version} != 1.24")
                if not mappingsymbol_ok:
                    reasons.append("no mappingsymbol")
                lit_config.note(f'BOLT Go: ARM64 requirements NOT met: {", ".join(reasons)}')

config.substitutions.append(("%goldflags", "'-ldflags=-linkmode=external -extld=gcc -extldflags \"-fuse-ld=bfd -no-pie -Wl,--emit-relocs -Wl,--compress-debug-sections=none\"'"))
# Same as %goldflags but keep the external linker in full PIE mode (no
# -no-pie override). Requires BOLT support for PIE Go binaries.
config.substitutions.append(("%pieflags", "'-ldflags=-linkmode=external -extld=gcc -extldflags \"-fuse-ld=bfd -Wl,--emit-relocs -Wl,--compress-debug-sections=none\"'"))
if platform.machine() == 'aarch64':
    config.substitutions.append(("%goopt", "-mappingsymbol"))
    # BOLT does not support jump tables in Go binaries on AArch64.
    config.substitutions.append(
        ("%gcjt", "-gcflags=all=-d=go119usejumptables=0"))
else:
    config.substitutions.append(("%goopt", ""))
    # Jump tables in Go binaries are supported on x86-64 (incl. PIE and
    # -rewrite).
    config.substitutions.append(
        ("%gcjt", "-gcflags=all=-d=go119usejumptables=1"))

llvm_config.config.environment["LD_LLD"] = config.bolt_lld
ld_lld = llvm_config.use_llvm_tool("ld.lld", required=True, search_env="LD_LLD")
llvm_config.config.available_features.add("ld.lld")
llvm_config.add_tool_substitutions([ToolSubst(r"ld\.lld", command=ld_lld)])

config.substitutions.append(("%cflags", ""))
config.substitutions.append(("%cxxflags", ""))

link_fdata_cmd = os.path.join(config.test_source_root, "link_fdata.py")

tool_dirs = [config.llvm_tools_dir, config.test_source_root]

tools = [
    ToolSubst("llc", unresolved="fatal"),
    ToolSubst("llvm-dwarfdump", unresolved="fatal"),
    ToolSubst("llvm-bolt", unresolved="fatal"),
    ToolSubst("llvm-boltdiff", unresolved="fatal"),
    ToolSubst("llvm-bolt-heatmap", unresolved="fatal"),
    ToolSubst("llvm-bat-dump", unresolved="fatal"),
    ToolSubst("perf2bolt", unresolved="fatal"),
    ToolSubst("yaml2obj", unresolved="fatal"),
    ToolSubst("llvm-mc", unresolved="fatal"),
    ToolSubst("llvm-nm", unresolved="fatal"),
    ToolSubst("llvm-objdump", unresolved="fatal"),
    ToolSubst("llvm-objcopy", unresolved="fatal"),
    ToolSubst("llvm-strings", unresolved="fatal"),
    ToolSubst("llvm-strip", unresolved="fatal"),
    ToolSubst("llvm-readelf", unresolved="fatal"),
    ToolSubst(
        "link_fdata",
        command=sys.executable,
        unresolved="fatal",
        extra_args=[link_fdata_cmd],
    ),
    ToolSubst("merge-fdata", unresolved="fatal"),
    ToolSubst("llvm-readobj", unresolved="fatal"),
    ToolSubst("llvm-dwp", unresolved="fatal"),
    ToolSubst("split-file", unresolved="fatal"),
]
llvm_config.add_tool_substitutions(tools, tool_dirs)


def calculate_arch_features(arch_string):
    features = []
    for arch in arch_string.split():
        features.append(arch.lower() + "-registered-target")
    return features


llvm_config.feature_config(
    [
        ("--assertion-mode", {"ON": "asserts"}),
        ("--cxxflags", {r"-D_GLIBCXX_DEBUG\b": "libstdcxx-safe-mode"}),
        ("--targets-built", calculate_arch_features),
    ]
)

config.targets = frozenset(config.targets_to_build.split(";"))
