# Cross-compiler for Ableton Move (aarch64 Linux).
#
# Debian bullseye for GCC 10 / glibc 2.31, so dsp.so never asks the Move for a
# newer symbol version than it has -- the same image chaolue/schwung-arranger
# builds with. Bullseye is EOL: deb.debian.org no longer serves it and the
# security repo's signed Release file has expired, so a plain apt-get update
# fails outright. archive.debian.org keeps EOL releases available, but main
# only -- it 404s the security repo, which apt treats as a hard error, so that
# line is dropped rather than repointed. Not needed here: this image only ever
# installs the toolchain and is rebuilt fresh on every CI run.
FROM debian:bullseye-slim

RUN sed -i -e '/security/d' -e 's/deb.debian.org/archive.debian.org/g' /etc/apt/sources.list \
    && apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    make \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-

WORKDIR /build
