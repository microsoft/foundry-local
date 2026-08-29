ARG BASE_IMAGE
FROM ${BASE_IMAGE}

RUN dnf install -y zip \
    && dnf clean all \
    && rm -rf /var/cache/dnf
