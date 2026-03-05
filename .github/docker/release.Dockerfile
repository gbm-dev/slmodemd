FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
      build-essential \
      gcc-multilib \
      g++-multilib \
      ca-certificates \
      curl \
      patch \
      pkg-config \
      file && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
