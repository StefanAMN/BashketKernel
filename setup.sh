#!/bin/bash
#
# BashketKernel build dependency installer.
# Supports Ubuntu/Debian (apt), Fedora/RHEL (dnf), and Arch/Manjaro (pacman).
# Run with -h/--help for usage.

set -Eeuo pipefail
IFS=$'\n\t'

# ---------------------------------------------------------------------------
# Package lists
# ---------------------------------------------------------------------------

UBUNTU_PACKAGES=(build-essential xorriso qemu-system-x86 gdb)
FEDORA_PACKAGES=(gcc gcc-c++ make binutils xorriso qemu-system-x86 gdb)
ARCH_PACKAGES=(base-devel xorriso qemu-system-x86 gdb)

# Limine is fetched separately from a pinned tag (rather than a floating
# branch) so the exact bootloader source/binaries in use are fixed and
# auditable instead of silently changing on a future `./setup.sh` run.
LIMINE_REPO="https://github.com/limine-bootloader/limine.git"
LIMINE_TAG="v11.4.1-binary"

declare -A DISTRO_LABEL=(
    [ubuntu]="Ubuntu/Debian (apt-get)"
    [fedora]="Fedora/RHEL (dnf)"
    [arch]="Arch/Manjaro (pacman)"
)

ASSUME_YES="false"
DISTRO_FLAG=""

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    C_RED=$'\033[31m'; C_YEL=$'\033[33m'; C_GRN=$'\033[32m'; C_BLD=$'\033[1m'; C_RST=$'\033[0m'
else
    C_RED=""; C_YEL=""; C_GRN=""; C_BLD=""; C_RST=""
fi

log_info()  { printf '%s\n' "${C_GRN}[*]${C_RST} $*"; }
log_warn()  { printf '%s\n' "${C_YEL}[!]${C_RST} $*" >&2; }
log_error() { printf '%s\n' "${C_RED}[x]${C_RST} $*" >&2; }

# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

on_error() {
    local line="$1" code="$2"
    echo "" >&2
    log_error "setup.sh failed at line ${line} (exit code ${code})."
    log_error "No further changes were made. Review the error above and re-run."
}
trap 'on_error "${LINENO}" "$?"' ERR
trap 'echo; log_warn "Interrupted."; exit 130' INT

usage() {
    cat <<EOF
Usage: ./setup.sh [--distro=NAME] [-y|--yes] [-h|--help]

Installs the build dependencies needed for BashketKernel (a C toolchain,
xorriso, a QEMU x86 system emulator, and gdb), and clones the Limine
bootloader (pinned to ${LIMINE_TAG}) into ./limine.

Options:
  --distro=NAME   Skip the interactive menu and install for NAME directly.
                   Accepted: ubuntu, debian, fedora, rhel, centos, arch, manjaro
  -y, --yes        Skip the confirmation prompt (assume "yes").
  -h, --help       Show this help text and exit.

With no options, the script tries to auto-detect your distribution and asks
you to confirm (or pick a different one) from a short menu before installing
anything.
EOF
}

# ---------------------------------------------------------------------------
# Distro detection
# ---------------------------------------------------------------------------

os_release_value() {
    local key="$1" file="/etc/os-release" line
    [[ -r "${file}" ]] || return 1
    line="$(grep -m1 -E "^${key}=" "${file}" || true)"
    [[ -n "${line}" ]] || return 1
    line="${line#"${key}"=}"
    line="${line%\"}"
    line="${line#\"}"
    printf '%s\n' "${line}"
}

detect_distro() {
    local id id_like combined
    id="$(os_release_value ID || true)"
    id_like="$(os_release_value ID_LIKE || true)"
    combined="${id} ${id_like}"
    case "${combined}" in
        *ubuntu*|*debian*)        echo "ubuntu"  ;;
        *fedora*|*rhel*|*centos*) echo "fedora"  ;;
        *arch*|*manjaro*)         echo "arch"    ;;
        *)                        echo "unknown" ;;
    esac
}

# Normalizes an alias (from --distro) to one of: ubuntu, fedora, arch.
# Prints nothing and returns 1 if the value isn't recognized.
normalize_distro_flag() {
    case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
        ubuntu|debian)        echo "ubuntu" ;;
        fedora|rhel|centos|rocky|almalinux) echo "fedora" ;;
        arch|manjaro)         echo "arch"   ;;
        *)                    return 1      ;;
    esac
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --distro=*)
                DISTRO_FLAG="${1#--distro=}"
                shift
                ;;
            --distro)
                [[ $# -ge 2 ]] || { log_error "--distro requires a value."; usage; exit 2; }
                DISTRO_FLAG="$2"
                shift 2
                ;;
            -y|--yes)
                ASSUME_YES="true"
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                log_error "Unknown argument: $1"
                usage
                exit 2
                ;;
        esac
    done
}

# ---------------------------------------------------------------------------
# Interactive menu
# ---------------------------------------------------------------------------

present_menu() {
    local detected="$1" choice
    echo "Select your distribution family:" >&2
    echo "  1) Ubuntu / Debian   (apt-get)" >&2
    echo "  2) Fedora / RHEL     (dnf)" >&2
    echo "  3) Arch / Manjaro    (pacman)" >&2
    if [[ "${detected}" == "unknown" ]]; then
        log_warn "Could not auto-detect your distribution. Pick the closest match above, or see README.md for manual install steps."
    else
        echo "Detected: ${DISTRO_LABEL[${detected}]} (press Enter to accept)" >&2
    fi

    while true; do
        read -r -p "Enter choice [1-3]: " choice
        if [[ -z "${choice}" && "${detected}" != "unknown" ]]; then
            printf '%s\n' "${detected}"
            return 0
        fi
        case "${choice}" in
            1) printf 'ubuntu\n'; return 0 ;;
            2) printf 'fedora\n'; return 0 ;;
            3) printf 'arch\n';   return 0 ;;
            *) log_error "Invalid choice: '${choice}'. Enter 1, 2, or 3." ;;
        esac
    done
}

confirm() {
    local prompt="$1" reply
    [[ "${ASSUME_YES}" == "true" ]] && return 0
    read -r -p "${prompt} [y/N]: " reply
    case "${reply}" in
        [yY]|[yY][eE][sS]) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# Privilege handling
# ---------------------------------------------------------------------------

resolve_sudo() {
    if [[ "${EUID}" -eq 0 ]]; then
        SUDO=()
        log_warn "Running as root: skipping sudo."
    else
        if ! command -v sudo >/dev/null 2>&1; then
            log_error "'sudo' is required to install packages as a non-root user, but it was not found."
            log_error "Install sudo, or re-run this script as root."
            exit 1
        fi
        SUDO=(sudo)
    fi
}

check_pm_binary() {
    command -v "$1" >/dev/null 2>&1 || {
        log_error "Package manager '$1' was not found on this system."
        log_error "This usually means the selected distro doesn't match your actual system."
        exit 1
    }
}

# ---------------------------------------------------------------------------
# Package plan + install
# ---------------------------------------------------------------------------

get_packages() {
    case "$1" in
        ubuntu) printf '%s\n' "${UBUNTU_PACKAGES[@]}" ;;
        fedora) printf '%s\n' "${FEDORA_PACKAGES[@]}" ;;
        arch)   printf '%s\n' "${ARCH_PACKAGES[@]}"   ;;
    esac
}

print_plan() {
    local distro="$1"
    shift
    local IFS=' '
    echo ""
    echo "${C_BLD}Target:${C_RST}   ${DISTRO_LABEL[${distro}]}"
    echo "${C_BLD}Packages:${C_RST} $*"
    echo "${C_BLD}Also:${C_RST}     clone Limine bootloader (${LIMINE_TAG}) into ./limine"
    if [[ "${distro}" == "arch" ]]; then
        log_warn "Arch will also sync and upgrade existing system packages (pacman -Syu)."
        log_warn "This avoids the well-known 'partial upgrade' breakage from installing"
        log_warn "new packages against a stale local package database."
    fi
    echo ""
}

install_ubuntu() {
    check_pm_binary apt-get
    "${SUDO[@]}" apt-get update
    "${SUDO[@]}" apt-get install -y "${UBUNTU_PACKAGES[@]}"
}

install_fedora() {
    check_pm_binary dnf
    "${SUDO[@]}" dnf install -y "${FEDORA_PACKAGES[@]}"
}

install_arch() {
    check_pm_binary pacman
    "${SUDO[@]}" pacman -Syu --needed --noconfirm "${ARCH_PACKAGES[@]}"
}

# ---------------------------------------------------------------------------
# Limine bootloader
# ---------------------------------------------------------------------------

fetch_limine() {
    if [[ -f "limine/limine.c" ]]; then
        log_info "Limine sources already present in ./limine (skipping)."
        return 0
    fi

    check_pm_binary git

    log_info "Fetching Limine bootloader (${LIMINE_TAG})..."
    rm -rf limine
    git clone --quiet --depth=1 --branch="${LIMINE_TAG}" "${LIMINE_REPO}" limine
    log_info "Limine bootloader fetched into ./limine"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

main() {
    parse_args "$@"

    local distro
    if [[ -n "${DISTRO_FLAG}" ]]; then
        if ! distro="$(normalize_distro_flag "${DISTRO_FLAG}")"; then
            log_error "Unrecognized --distro value: '${DISTRO_FLAG}'"
            log_error "Accepted: ubuntu, debian, fedora, rhel, centos, arch, manjaro"
            exit 2
        fi
    else
        local detected
        detected="$(detect_distro)"
        if [[ ! -t 0 ]]; then
            log_error "No terminal available to prompt for a distribution, and no --distro was given."
            log_error "Re-run with, e.g., --distro=ubuntu --yes"
            exit 2
        fi
        distro="$(present_menu "${detected}")"
    fi

    local -a packages
    mapfile -t packages < <(get_packages "${distro}")

    print_plan "${distro}" "${packages[@]}"

    if ! confirm "Proceed with installation?"; then
        log_info "Aborted by user. No changes were made."
        exit 0
    fi

    resolve_sudo

    case "${distro}" in
        ubuntu) install_ubuntu ;;
        fedora) install_fedora ;;
        arch)   install_arch   ;;
    esac

    fetch_limine

    echo ""
    log_info "Dependencies installed successfully!"
    echo "You can now run 'make iso' to build the kernel or 'make run' to boot it in QEMU."
}

main "$@"
