# Builds the MDMI ROM and runs its tests: the image the ROM's author builds v1.34 with
# (third_party/mdmi/Dockerfile), pinned by digest. E2 built the release byte for byte in it (D28).
FROM ghcr.io/rhargreaves/docker-sgdk:v2.00-mw-bs@sha256:82d2928c4754e95cc6fc395cb451af1229637ed881fc04c1d5d84aa978ded629
USER root
# Debian 11 has left deb.debian.org; its packages stay on archive.debian.org.
RUN printf '%s\n' \
    'deb http://archive.debian.org/debian bullseye main' \
    'deb http://archive.debian.org/debian-security bullseye-security main' \
    'deb http://archive.debian.org/debian bullseye-updates main' \
    > /etc/apt/sources.list && \
    apt-get -y update && \
    apt-get -y install build-essential git cmake wget unzip && \
    git config --global --add safe.directory "*"
WORKDIR /app
ENTRYPOINT []
