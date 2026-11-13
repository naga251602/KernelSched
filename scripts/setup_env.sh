#!/usr/bin/env bash
# KernelSched environment setup (Ubuntu 24.04 LTS / Debian or Fedora).
# Installs clang/llvm, libbpf, bpftool, cmake and the Python plotting stack.
set -euo pipefail

if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y \
    build-essential cmake pkg-config clang llvm libbpf-dev \
    linux-tools-common "linux-tools-$(uname -r)" \
    python3 python3-pip python3-pandas python3-matplotlib
  # bpftool lives in the kernel-versioned linux-tools package; fall back to
  # linux-tools-generic if the exact one is not published for your kernel.
  if ! command -v bpftool >/dev/null 2>&1; then
    sudo apt-get install -y linux-tools-generic || true
  fi
elif command -v dnf >/dev/null 2>&1; then
  sudo dnf install -y \
    gcc-c++ cmake pkgconf-pkg-config clang llvm libbpf-devel bpftool \
    kernel-devel python3 python3-pip
  pip3 install --user pandas matplotlib || true
else
  echo "Unsupported package manager; install: clang llvm libbpf-dev bpftool cmake" >&2
  exit 1
fi

echo
echo "Checking prerequisites..."
command -v clang   >/dev/null || { echo "missing: clang";   exit 1; }
command -v bpftool >/dev/null || { echo "missing: bpftool"; exit 1; }
command -v cmake   >/dev/null || { echo "missing: cmake";   exit 1; }
[ -e /sys/kernel/btf/vmlinux ] || { echo "missing: /sys/kernel/btf/vmlinux (need kernel 5.5+ with CONFIG_DEBUG_INFO_BTF)"; exit 1; }
[ -d /sys/kernel/tracing/events/sched ] || [ -d /sys/kernel/debug/tracing/events/sched ] \
  || { echo "warning: sched tracepoints not visible (check CONFIG_FTRACE / mount debugfs tracefs)"; }

echo "All good. Build with:"
echo "  cmake -S . -B build && cmake --build build -j"
