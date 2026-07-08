# AGENTS.md

Guidance for OpenCode agents working in this LLVM monorepo. **This checkout is
used for `llvm-bolt` development on an `AArch64` (arm64) host.**

## Build system

- This is the LLVM **monorepo**, but we only build **bolt** (with its required
  clang + lld dependencies). There is **no top-level `CMakeLists.txt`** —
  configure with `llvm/` as the source root. Generator is **Ninja**.
  `llvm/configure` is a stub that only errors out; ignore it.
- Configure (matches this checkout's existing `build/`):
  ```
  cmake -S llvm -B build -G Ninja \
        -DLLVM_ENABLE_PROJECTS="clang;bolt;lld" \
        -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
        -DLLVM_ENABLE_ASSERTIONS=ON \
        -DLLVM_USE_LINKER=lld \
        -DCMAKE_BUILD_TYPE=Release
  ```
  `clang` and `lld` are **required** for bolt tests — without them CMake prints
  "Not including BOLT tests" and `check-bolt` is not created.
- Build just bolt: `ninja -C build bolt`. The driver is `build/bin/llvm-bolt`
  (plus `llvm-bolt-heatmap`, `llvm-bolt-binary-analysis`, `llvm-boltdiff`,
  `perf2bolt`, `merge-fdata`).
- **Targets**: bolt supports `AArch64;X86;RISCV` (`bolt/CMakeLists.txt`).
  `BOLT_TARGETS_TO_BUILD` is auto-derived as the intersection of that list with
  `LLVM_TARGETS_TO_BUILD` (here `AArch64;X86`). To change it, re-run CMake.
- **Runtime libs**: `BOLT_ENABLE_RUNTIME` defaults ON on this native `aarch64`
  Linux host, so `libbolt_rt_instr` / `libbolt_rt_hugify` are built (needed by
  instrumentation/hugify). It is forced OFF when cross-compiling.
- Presets live in `llvm/CMakePresets.json`.

## Tests

- Tests are run with **lit**. Use the built runner: `build/bin/llvm-lit`.
- Whole bolt suite: `ninja -C build check-bolt`.
- **AArch64-focused** (this host runs them natively — no QEMU):
  - Single test / dir: `build/bin/llvm-lit -v -j1 bolt/test/AArch64/<file>.s`
  - Whole AArch64 dir: `build/bin/llvm-lit bolt/test/AArch64`
  - Bolt also has `bolt/test/X86`, `bolt/test/RISCV`, and arch-independent tests
    in `bolt/test/` root.
- `bolt/test/CMakeLists.txt` lists all tool deps for `check-bolt`
  (`llc`, `llvm-mc`, `llvm-objdump`, `llvm-dwarfdump`, etc.); build them with
  `ninja -C build bolt-test-depends` if running lit manually.
- This is a **native aarch64** machine, so executable tests run natively.
  `bolt/test/lit.local.cfg` forces the host triple and `-fuse-ld=lld -pie`.
- Regression tests use **FileCheck** (`build/bin/FileCheck`) with
  `CHECK`/`CHECK-LABEL` lines; preserve existing prefixes when editing tests.
- Out-of-tree large suites (optional): `rafaelauler/bolt-tests` and
  `arm/large-bolt-tests` (extra AArch64 binaries) — see `bolt/README.md`.

## Formatting & lint

- C/C++: `.clang-format` is `BasedOnStyle: LLVM`, `LineEnding: <LF>`. Format a
  commit's changes with `git clang-format HEAD~1` (documented LLVM workflow in
  `llvm/utils/git/github-automation.py`).
- Python: formatted with **black** (`pyproject.toml` excludes `third-party/`).
- clang-tidy config is `.clang-tidy`.
- Optional pre-push hook: `ln -sf ../../llvm/utils/git/pre-push.py .git/hooks/pre-push`.

## Code conventions

- **Every source file** must start with the LLVM license header
  (Apache-2.0 WITH LLVM-exception):
  ```
  //===----------------------------------------------------------------------===//
  //
  // Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
  // See https://llvm.org/LICENSE.txt for license information.
  // SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
  //
  //===----------------------------------------------------------------------===//
  ```
  (Python uses the `# =...` variant — see `llvm/utils/git/pre-push.py`.)
- C++17; **no RTTI, no exceptions**, prefer C++ casts, prefer `raw_ostream` over
  `<iostream>`. See the LLVM Coding Standards.
- Bolt-specific design notes: `bolt/docs/PointerAuthDesign.md` (arm64 PAC),
  `bolt/docs/RuntimeLibrary.md`. Maintainers: `bolt/Maintainers.md` (has an
  "Aarch64 Backend" section). Code-owner auto-assignment: `.github/CODEOWNERS`.

## Workflow

- PRs target `main` (or `users/**`). CI uses `LLVM_ENABLE_WERROR=ON`.
- `.git-blame-ignore-revs` is present; prefer rebase/atomic commits —
  formatting-only commits are listed there to keep blame clean.
- Do not modify generated files (TableGen outputs, etc.) directly — regenerate
  via the corresponding `ninja` target.
