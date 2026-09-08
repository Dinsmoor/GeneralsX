# Building on Linux with Wine (without Docker)

This guide builds the Windows game executables on Linux using Wine directly,
without Docker. It is an alternative to [`scripts/docker-build.sh`](docker-build.sh)
for systems where Docker is unavailable or undesirable — for example when the
user cannot be granted Docker daemon access, which is root-equivalent.

The approach is the same one the Docker image uses: the Windows build tools
(CMake, Ninja, Visual C++ 6.0 and MinGit) are run under Wine to cross-compile
Windows binaries. The only difference is that they run under the host's Wine
instead of Wine inside a container.

**Everything runs as a normal user. No root, and nothing is installed system-wide.**

## Requirements

- **Wine** with 32-bit support. The toolchain binaries are i386, so a Wine
  build that can only run 64-bit executables will not work. On Debian/Ubuntu
  this usually means enabling the i386 architecture:
  ```bash
  sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine
  ```
  Wine 9.0 or newer is recommended; the build has been verified on Wine 11.16.
- `wget` and `unzip`
- About 3 GB of free disk space (toolchain plus build tree)

No CMake, Ninja or compiler is needed on the host — the Windows versions are
downloaded and run under Wine.

## Quick start

```bash
./scripts/wine-build.sh --setup     # download the toolchain (once, ~2 GB)
./scripts/wine-build.sh             # build Zero Hour
```

The build takes roughly 10-20 minutes depending on the machine.

> **A `wine-preloader has encountered a fatal error` dialog during configuration is
> expected and harmless — the build completes normally.** See
> [Notes and gotchas](#notes-and-gotchas).

Outputs land in `build/vc6/GeneralsMD/`:

```
generalszh.exe        Zero Hour
WorldBuilderZH.exe    Map editor
W3DViewZH.exe         Model viewer
```

Then install into an existing game directory as usual:

```bash
./scripts/docker-install.sh --detect     # despite the name, does not use Docker
```

## Usage

```bash
./scripts/wine-build.sh --setup                # download/verify toolchain only
./scripts/wine-build.sh                        # build Zero Hour (default)
./scripts/wine-build.sh --game generals        # build Generals
./scripts/wine-build.sh --game all             # build everything
./scripts/wine-build.sh --target z_worldbuilder
./scripts/wine-build.sh --cmake                # force CMake reconfiguration
./scripts/wine-build.sh --clean                # wipe build/vc6 first
```

Environment overrides:

| Variable          | Default                  | Purpose                                     |
|-------------------|--------------------------|---------------------------------------------|
| `TOOLS_DIR`       | `build/wine-tools`       | Where the Windows toolchain is installed     |
| `WINE`            | `wine` from `PATH`       | Use a specific Wine build                    |
| `WINEPREFIX_BUILD`| `$TOOLS_DIR/prefix`      | Wine prefix used for building                |

The script uses its **own** Wine prefix and never touches `~/.wine` or a prefix
holding an installed game.

## What the script does

1. Downloads the same toolchain versions the Docker image pins — CMake 3.31.6,
   Ninja 1.13.1, MinGit 2.49.0 and [MSVC600](https://github.com/itsmattkc/MSVC600).
2. Creates a dedicated Wine prefix and verifies MSVC 6 actually runs.
3. Configures with the `vc6` CMake preset, pointing CMake at the Windows tools
   via `Z:` paths.
4. Builds with Ninja under Wine.

## Notes and gotchas

These are the differences from the container environment that the script handles;
they are worth knowing if you adapt it or debug a failure.

**`CL.EXE` needs its DLLs on `WINEPATH`.** The compiler driver is a ~50 KB stub
that loads `C1.DLL`, `C1XX.DLL` and `C2.DLL` from its own directory. If
`WINEPATH` does not include `VC98\Bin`, it exits with status **53** and prints
nothing at all. This is the single most confusing failure mode.

**MinGit must stay intact.** `git.exe` under `cmd/` is a launcher that expects
`mingw64/` and `usr/` as sibling directories. Moving `cmd/` out of the extracted
archive breaks it with `error launching git: Path not found`, which surfaces during
configuration as:

```
git version 1.6.5 or later required ... GIT_VERSION_STRING=''
```

The script therefore keeps the archive layout and points `GIT_EXECUTABLE` at
`git/mingw64/bin/git.exe`. Git is genuinely required — CMake uses it to fetch the
Miles, Bink and DirectX 8 dependencies.

**Case sensitivity.** Linux filesystems are case-sensitive and the MSVC600 archive
ships uppercase `VC98/Bin/CL.EXE`. Paths must match the archive's actual casing;
the lowercase spellings used inside the Docker image work there but not
necessarily here.

**Never trust an incremental build across a source-tree switch.** Ninja decides
what to rebuild from file mtimes, so a `git stash`, `git stash pop`, `git checkout`
or `git rebase` can restore sources with *older* timestamps than the objects built
from them. Ninja then skips work it should redo and links a mixture of old and new
objects. The build succeeds, and the resulting binary misbehaves at runtime — in
one case hanging at the splash screen with a `.text` section 64 bytes smaller than
a correct build. `make`-based builds have the same flaw; it is not specific to Ninja.

`wine-build.sh` records the built revision and wipes the build directory when it
changes, but the safe habit after any git operation is:

```bash
./scripts/wine-build.sh --clean
```

A build that reports far fewer steps than expected (`[5/5]` where a full build is
`[1080/1080]`) is a symptom, not a fast build.

**Expect a `wine-preloader` crash dialog — the build still succeeds.** During
configuration you will most likely see:

```
<wine>/lib/wine/x86_64-unix/wine-preloader has encountered a fatal error and was closed.
```

This is normal and can be ignored. It comes from one of the short-lived helper
processes CMake spawns while probing the toolchain and fetching dependencies, not
from the compiler itself. The build continues and completes with all targets. If
the script does stop, simply run it again — CMake and Ninja both resume from
where they left off.

## Troubleshooting

**`MSVC 6 failed to run under Wine`**
Wine cannot execute 32-bit binaries. Install 32-bit Wine support (see Requirements).
Verify manually:
```bash
WINEPATH="Z:$PWD/build/wine-tools/vs6/VC98/Bin" \
  wine build/wine-tools/vs6/VC98/Bin/CL.EXE
```
This should print the compiler banner.

**Configuration fails fetching dependencies**
Check network access and that `git/mingw64/bin/git.exe` exists. Re-run with
`--setup` to repair a partial toolchain download.

**Build errors after updating the repository**
Force a reconfiguration: `./scripts/wine-build.sh --cmake`

## Verified configuration

| Component | Version |
|-----------|---------|
| Host      | Ubuntu (kernel 7.0), x86_64 |
| Wine      | 11.16 |
| CMake     | 3.31.6 (Windows) |
| Ninja     | 1.13.1 (Windows) |
| MinGit    | 2.49.0 |
| Compiler  | MSVC 12.00.8804 (Visual C++ 6.0) |

Result: `generalszh.exe` (1079/1079 targets), plus `WorldBuilderZH.exe` and
`W3DViewZH.exe`. Verified by playing a skirmish game against retail Zero Hour
game data.
