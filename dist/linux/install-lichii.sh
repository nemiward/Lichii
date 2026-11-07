#!/usr/bin/env bash
# Compiles this checkout and installs Lichii for the current user (no root): the program, a
# `lichii` command and an entry with its icon in the applications menu.
#
# Usage, from a clone of this repository:
#   dist/linux/install-lichii.sh               compile, then install
#   dist/linux/install-lichii.sh --no-build    install the existing build (build/bin)
#   dist/linux/install-lichii.sh --uninstall   remove what this script installed
#
# Build dependencies: see docs/Deps.md (Fedora, Ubuntu/Debian, Arch...).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$REPO/build"
APP_ID="io.github.zortos293.lichii"

PREFIX="$HOME/.local/opt/lichii"
BIN_LINK="$HOME/.local/bin/lichii"
APPS_DIR="$HOME/.local/share/applications"
ICON_DIR="$HOME/.local/share/icons/hicolor/256x256/apps"

die() { echo "Error: $*" >&2; exit 1; }

refresh_menu() {
  update-desktop-database "$APPS_DIR" 2>/dev/null || true
  gtk-update-icon-cache -q -t "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
  kbuildsycoca6 >/dev/null 2>&1 || kbuildsycoca5 >/dev/null 2>&1 || true
}

build() {
  command -v cmake >/dev/null || die "cmake is missing; install the dependencies in docs/Deps.md"
  command -v ninja >/dev/null || die "ninja is missing; install the dependencies in docs/Deps.md"
  # A build directory configured from another path keeps invalid absolute paths: start over.
  if [[ -f "$BUILD/CMakeCache.txt" ]] &&
     ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$REPO" "$BUILD/CMakeCache.txt"; then
    echo "==> The repository moved; rebuilding from scratch"
    rm -rf "$BUILD"
  fi
  echo "==> Configuring"
  cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF \
    -DENABLE_WERROR=OFF -DVulkanHeaders_FORCE_BUNDLED=ON
  echo "==> Compiling (this takes a while)"
  ninja -C "$BUILD" yuzu
}

install() {
  [[ -x "$BUILD/bin/lichii" ]] || die "$BUILD/bin/lichii does not exist; compile it first"
  echo "==> Installing to $PREFIX"
  mkdir -p "$PREFIX" "$(dirname "$BIN_LINK")" "$APPS_DIR" "$ICON_DIR"
  cp -a "$BUILD/bin/." "$PREFIX/" 2>/dev/null || true
  # Copy then rename, so it also works while Lichii is running.
  cp -f "$BUILD/bin/lichii" "$PREFIX/lichii.new"
  mv -f "$PREFIX/lichii.new" "$PREFIX/lichii"
  ln -sf "$PREFIX/lichii" "$BIN_LINK"
  cp -f "$REPO/dist/lichii.png" "$ICON_DIR/$APP_ID.png"
  cat > "$APPS_DIR/$APP_ID.desktop" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Lichii
GenericName=Switch Emulator
Comment=Experimental Nintendo Switch and Switch 2 emulator
Icon=$APP_ID
TryExec=$PREFIX/lichii
Exec=$PREFIX/lichii %f
Categories=Game;Emulator;Qt;
MimeType=application/x-nx-nro;application/x-nx-nso;application/x-nx-nsp;application/x-nx-xci;
Keywords=Nintendo;Switch;Lichii;
StartupWMClass=lichii
Terminal=false
EOF
  refresh_menu
  echo "Done. Start it from the applications menu or with: lichii"
  case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo "Note: ~/.local/bin is not in your PATH." ;; esac
}

uninstall() {
  rm -rf "$PREFIX"
  rm -f "$BIN_LINK" "$APPS_DIR/$APP_ID.desktop" "$ICON_DIR/$APP_ID.png"
  refresh_menu
  echo "Lichii removed. Saves and settings in ~/.local/share/eden and ~/.config/eden were kept."
}

case "${1:-}" in
  "") build; install ;;
  --no-build) install ;;
  --uninstall) uninstall ;;
  -h|--help) sed -n '2,10p' "$0" ;;
  *) die "unknown option $1 (see --help)" ;;
esac
