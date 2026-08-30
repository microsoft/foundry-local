ARG BASE_IMAGE
FROM ${BASE_IMAGE}

RUN dnf install -y \
        bzip2 \
        bzip2-devel \
        expat-devel \
        glibc-langpack-en \
        glibc-locale-source \
        kernel-headers \
        make \
        openssl-devel \
        perl-core \
        perl-IPC-Cmd \
        readline-devel \
        redhat-lsb-core \
        tar \
        unzip \
        wget \
        which \
        zip \
        zlib-devel \
    && dnf clean all \
    && rm -rf /var/cache/dnf
