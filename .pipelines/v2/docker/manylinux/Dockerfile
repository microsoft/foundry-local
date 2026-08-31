ARG BASE_IMAGE
FROM ${BASE_IMAGE}

RUN dnf install -y \
        kernel-headers \
        make \
        perl-core \
        perl-IPC-Cmd \
        zip \
    && dnf clean all \
    && rm -rf /var/cache/dnf
