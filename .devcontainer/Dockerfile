# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
#
# The build environment: Debian Trixie arm64, the same OS and architecture as the CM4 (Pi OS
# Trixie). Used by deploy/dev.sh and by VS Code's Dev Containers (devcontainer.json).
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make cmake python3 git ca-certificates \
        device-tree-compiler openssl file \
        clangd gdb rsync openssh-client curl \
    && rm -rf /var/lib/apt/lists/*

# The source is a bind mount owned by the host user.
RUN git config --system --add safe.directory /src

WORKDIR /src
