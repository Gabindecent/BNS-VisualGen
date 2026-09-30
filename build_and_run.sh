#!/usr/bin/env bash
# =====================================================================================
#  BNS VisualGen — installation, compilation et lancement (Linux x86_64)
#
#  Usage :  ./build_and_run.sh [options] [-- options de l'application]
#
#    --no-deps        ne pas vérifier/installer les dépendances système
#    --yes, -y        répondre oui aux questions d'installation (apt, téléchargements)
#    --debug          compilation Debug (sinon Release)
#    --clean          nettoie les fichiers de compilation avant de compiler
#    --build-only     compile sans lancer
#    --of-root PATH   emplacement d'openFrameworks (sinon $OF_ROOT, ../openFrameworks,
#                     ou ../../.. si le projet est dans openFrameworks/apps/myApps/)
#
#  Exemples :
#    ./build_and_run.sh                         # tout-en-un
#    ./build_and_run.sh -- --monitor 1          # sortie sur l'écran 1
#    ./build_and_run.sh --debug -- --windowed   # répétition sur un seul écran
#
#  Ce que fait le script, dans l'ordre (chaque étape est sautée si déjà faite) :
#    1. outils de base (git, curl, make, pkg-config, g++)
#    2. openFrameworks 0.12.1 (téléchargé à côté du projet s'il est introuvable)
#    3. dépendances système d'openFrameworks (paquets -dev via son script officiel)
#    4. addons listés dans addons.make (ofxImGui, épinglé sur un commit précis)
#    5. compilation via le Makefile du projet
#    6. lancement de l'application depuis bin/
# =====================================================================================
set -euo pipefail

# --- Versions épinglées : un build de soirée doit être reproductible ------------------
readonly OF_VERSION="0.12.1"
readonly OF_ARCHIVE="of_v${OF_VERSION}_linux64_gcc6_release"
readonly OF_URL="https://github.com/openframeworks/openFrameworks/releases/download/${OF_VERSION}/${OF_ARCHIVE}.tar.gz"
readonly OFXIMGUI_REPO="https://github.com/jvcleave/ofxImGui.git"
readonly OFXIMGUI_COMMIT="4b5d04a3e73bbae34d564c9b78143e021f67377a" # branche develop, avril 2026

# Paquets pkg-config exigés par openFrameworks 0.12.1 sous Linux (config.linux.common.mk)
readonly OF_PKG_CONFIG=(cairo zlib gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
    gstreamer-base-1.0 gstreamer-gl-1.0 libudev freetype2 fontconfig sndfile openal
    libpulse-simple alsa gl glu glew glfw3 gtk+-3.0 rtaudio libmpg123)

# --- Affichage -------------------------------------------------------------------------
if [ -t 1 ]; then B=$'\033[1m'; R=$'\033[1;31m'; G=$'\033[1;32m'; Y=$'\033[1;33m'; N=$'\033[0m'
else B=""; R=""; G=""; Y=""; N=""; fi
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
ok()   { printf '%s  ✓ %s%s\n' "$G" "$*" "$N"; }
warn() { printf '%s  ! %s%s\n' "$Y" "$*" "$N" >&2; }
die()  { printf '%s  ✗ %s%s\n' "$R" "$*" "$N" >&2; exit 1; }

# --- Arguments -------------------------------------------------------------------------
DO_DEPS=1; ASSUME_YES=0; CONFIG="Release"; DO_CLEAN=0; DO_RUN=1; OF_ROOT_ARG=""
APP_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-deps)    DO_DEPS=0 ;;
        --yes|-y)     ASSUME_YES=1 ;;
        --debug)      CONFIG="Debug" ;;
        --clean)      DO_CLEAN=1 ;;
        --build-only) DO_RUN=0 ;;
        --of-root)    shift; OF_ROOT_ARG="${1:-}"; [ -n "$OF_ROOT_ARG" ] || die "--of-root attend un chemin" ;;
        --help|-h)    sed -n '2,28p' "$0"; exit 0 ;;
        --)           shift; APP_ARGS=("$@"); break ;;
        *)            die "option inconnue : $1 (voir --help)" ;;
    esac
    shift
done

cd "$(dirname "$(readlink -f "$0")")"
readonly PROJECT_DIR="$(pwd)"
readonly APP_NAME="$(basename "$PROJECT_DIR")" # le Makefile OF nomme le binaire d'après le dossier
[ "$(uname -s)" = "Linux" ] || die "ce script vise Linux uniquement"
[ "$(uname -m)" = "x86_64" ] || die "architecture $(uname -m) non prise en charge (x86_64 requis)"

confirm() { # confirm "question" -> 0 si oui
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    local answer; read -r -p "  $1 [o/N] " answer
    [[ "$answer" =~ ^[oOyY] ]]
}

SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

detect_distro() { # ubuntu | debian | fedora | archlinux
    local id="" like=""
    if [ -r /etc/os-release ]; then
        id="$(. /etc/os-release; echo "${ID:-}")"
        like="$(. /etc/os-release; echo "${ID_LIKE:-}")"
    fi
    case "$id $like" in
        *arch*)           echo archlinux ;;
        *fedora*|*rhel*)  echo fedora ;;
        debian*)          echo debian ;;
        *ubuntu*|*debian*) echo ubuntu ;;
        *)                echo unknown ;;
    esac
}
readonly DISTRO="$(detect_distro)"

# =====================================================================================
step "1/6 Outils de base"
# =====================================================================================
missing_tools=()
for tool in git curl tar make g++ pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing_tools+=("$tool")
done
if [ ${#missing_tools[@]} -gt 0 ]; then
    warn "manquants : ${missing_tools[*]}"
    [ "$DO_DEPS" = 1 ] || die "installer ces outils, ou relancer sans --no-deps"
    case "$DISTRO" in
        ubuntu|debian) $SUDO apt-get update && $SUDO apt-get install -y git curl tar make g++ pkg-config ;;
        fedora)        $SUDO dnf install -y git curl tar make gcc-c++ pkgconf-pkg-config ;;
        archlinux)     $SUDO pacman -S --needed --noconfirm git curl tar make gcc pkgconf ;;
        *)             die "distribution non reconnue : installer à la main ${missing_tools[*]}" ;;
    esac
fi
ok "git, curl, make, g++, pkg-config"

# =====================================================================================
step "2/6 openFrameworks ${OF_VERSION}"
# =====================================================================================
is_of_root() { [ -f "$1/libs/openFrameworksCompiled/project/makefileCommon/compile.project.mk" ]; }

OF_ROOT_FOUND=""
for candidate in "$OF_ROOT_ARG" "${OF_ROOT:-}" "$PROJECT_DIR/../openFrameworks" "$PROJECT_DIR/../../.."; do
    if [ -n "$candidate" ] && is_of_root "$candidate"; then
        OF_ROOT_FOUND="$(cd "$candidate" && pwd)"; break
    fi
done

if [ -z "$OF_ROOT_FOUND" ]; then
    target="$(cd "$PROJECT_DIR/.." && pwd)/openFrameworks"
    [ -n "$OF_ROOT_ARG" ] && target="$OF_ROOT_ARG"
    warn "openFrameworks introuvable"
    confirm "Télécharger openFrameworks ${OF_VERSION} (~1 Go) dans $target ?" \
        || die "installer openFrameworks puis relancer avec --of-root CHEMIN"
    [ -e "$target" ] && die "$target existe déjà mais n'est pas un openFrameworks valide"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    curl -fL --retry 3 --progress-bar -o "$tmp/of.tar.gz" "$OF_URL" || die "échec du téléchargement : $OF_URL"
    tar -xzf "$tmp/of.tar.gz" -C "$tmp"
    mv "$tmp/$OF_ARCHIVE" "$target"
    OF_ROOT_FOUND="$(cd "$target" && pwd)"
fi
export OF_ROOT="$OF_ROOT_FOUND"

# Clone git d'OF (au lieu de l'archive) : ses librairies tierces ne sont pas encore là
if [ ! -d "$OF_ROOT/libs/glm/include" ] || [ ! -d "$OF_ROOT/libs/kiss/include" ]; then
    warn "librairies d'openFrameworks absentes (clone git ?) : téléchargement"
    "$OF_ROOT/scripts/linux/download_libs.sh"
fi
of_version_file="$OF_ROOT/libs/openFrameworks/utils/ofConstants.h"
if ! grep -q "OF_VERSION_MINOR 12" "$of_version_file" 2>/dev/null; then
    warn "cet openFrameworks n'est pas en 0.12.x : le build peut échouer"
fi
ok "openFrameworks : $OF_ROOT"

# =====================================================================================
step "3/6 Dépendances système"
# =====================================================================================
missing_pkgs=()
for pkg in "${OF_PKG_CONFIG[@]}"; do
    pkg-config --exists "$pkg" || missing_pkgs+=("$pkg")
done
if [ ${#missing_pkgs[@]} -gt 0 ]; then
    warn "manquantes : ${missing_pkgs[*]}"
    [ "$DO_DEPS" = 1 ] || die "relancer sans --no-deps, ou installer les dépendances d'openFrameworks"
    [ "$DISTRO" != unknown ] || die "distribution non reconnue : voir $OF_ROOT/scripts/linux/"
    installer="$OF_ROOT/scripts/linux/$DISTRO/install_dependencies.sh"
    [ -x "$installer" ] || die "script introuvable : $installer"
    echo "  Installation via le script officiel d'openFrameworks (mot de passe sudo demandé)"
    yes_flag=(); [ "$ASSUME_YES" = 1 ] && yes_flag=(-y)
    $SUDO "$installer" "${yes_flag[@]}"
    still=()
    for pkg in "${missing_pkgs[@]}"; do pkg-config --exists "$pkg" || still+=("$pkg"); done
    [ ${#still[@]} -eq 0 ] || die "toujours manquantes après installation : ${still[*]}"
fi
ok "${#OF_PKG_CONFIG[@]} paquets openFrameworks présents"

# Audio : l'app capte l'entrée par défaut du serveur son (PulseAudio ou PipeWire-Pulse)
if command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1; then
    ok "serveur audio : $(pactl info | sed -n 's/^Server Name: //p')"
else
    warn "aucun serveur PulseAudio/PipeWire détecté : l'audio passera par ALSA directement"
fi

# =====================================================================================
step "4/6 Addons"
# =====================================================================================
install_addon() { # install_addon NOM
    local name="$1" dir="$OF_ROOT/addons/$1"
    case "$name" in
        ofxImGui)
            if [ ! -d "$dir" ]; then
                git clone --quiet "$OFXIMGUI_REPO" "$dir"
                git -C "$dir" -c advice.detachedHead=false checkout --quiet "$OFXIMGUI_COMMIT"
                ok "ofxImGui installé (commit ${OFXIMGUI_COMMIT:0:7})"
            else
                local head; head="$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo "?")"
                [ "$head" = "$OFXIMGUI_COMMIT" ] || warn "ofxImGui présent mais sur ${head:0:7} (attendu ${OFXIMGUI_COMMIT:0:7})"
                ok "ofxImGui"
            fi ;;
        *)
            [ -d "$dir" ] || die "addon $name absent de $OF_ROOT/addons et installation automatique inconnue"
            ok "$name" ;;
    esac
}
if [ -s addons.make ]; then
    while IFS= read -r addon || [ -n "$addon" ]; do
        addon="${addon%%#*}"; addon="$(echo "$addon" | xargs)"
        [ -n "$addon" ] && install_addon "$addon"
    done < addons.make
else
    ok "aucun addon"
fi

# =====================================================================================
step "5/6 Compilation ($CONFIG)"
# =====================================================================================
if [ "$DO_CLEAN" = 1 ]; then
    make clean >/dev/null || true
    rm -rf obj
    ok "nettoyé"
fi
# La 1re compilation construit aussi le coeur d'openFrameworks : plusieurs minutes.
make -j"$(nproc)" "$CONFIG"
BIN="bin/$APP_NAME"; [ "$CONFIG" = "Debug" ] && BIN="bin/${APP_NAME}_debug"
[ -x "$BIN" ] || die "binaire introuvable après compilation : $BIN"
ok "$BIN"

# =====================================================================================
if [ "$DO_RUN" = 0 ]; then exit 0; fi
step "6/6 Lancement"
# =====================================================================================
cd bin
exec "./$(basename "$BIN")" "${APP_ARGS[@]}"
