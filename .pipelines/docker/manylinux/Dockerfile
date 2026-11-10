ARG BASE_IMAGE
FROM ${BASE_IMAGE}

# These build prerequisites come from AlmaLinux, not EPEL.
RUN dnf --disablerepo='epel*' install -y \
        kernel-headers \
        make \
        perl-core \
        perl-IPC-Cmd \
        zip \
    && dnf clean all \
    && rm -rf /var/cache/dnf
