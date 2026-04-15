FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Seoul
ENV LANG=ko_KR.UTF-8
ENV LANGUAGE=ko_KR.UTF-8

RUN apt-get update && apt-get install -y \
    locales \
    tzdata \
    build-essential \
    sudo \
    gdb \
    vim \
    curl \
    git \
    wget \
    python3 \
    cmake \
    make \
    gcc \
    valgrind \
    telnet \
    net-tools \
    iproute2 \
    clang-format \
    && locale-gen ko_KR.UTF-8 \
    && update-locale LANG=ko_KR.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash jungle \
    && usermod -aG sudo jungle \
    && echo "jungle ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/jungle

USER jungle
WORKDIR /home/jungle
