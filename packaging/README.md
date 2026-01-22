# Packaging (starter)

This directory contains **starter** packaging metadata for distros.

- Debian/Ubuntu: `packaging/debian/` (copy/symlink to `debian/` at repo root, then build with `dpkg-buildpackage`).
- Arch: `packaging/arch/PKGBUILD` (update `source=...` / checksums as needed).

Notes:

- These files are intentionally minimal and may need distro-specific tweaks (dependency names, hardening flags, etc.).
- The project currently installs via CMake (`cmake --install build`), including the desktop file, AppStream metadata, and icon.
