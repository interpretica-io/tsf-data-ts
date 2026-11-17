# Build environment
FROM debian:bookworm

ARG user
ARG uid
ARG gid
ARG docker_gid
ENV USER=${user}
ENV UID=${uid}
ENV GID=${gid}
ENV DOCKER_GID=${docker_gid}

RUN if [ "$DOCKER_GID" != "${GID}" ] ; then groupadd --gid ${DOCKER_GID} docker ; fi

RUN if [ ! -d /home/${USER} ] ; then mkdir -p /home/${USER} && chmod -R 777 /home/${USER} ; fi

# Install basic dependencies & useful tools. ta_data links jansson (JSON
# and JSON Lines), SQLite (the built-in SQL source) and, through its one
# C++ unit, Apache Arrow/Parquet C++ - the last from Apache's own apt
# repository below, Debian has none. g++ builds that unit. python3 is the
# independent oracle: the suite recomputes reference statistics with the
# standard library, and pyarrow writes Parquet files with a different
# implementation than the one under test.
RUN apt-get update && \
    apt-get install -y \
        autoconf \
        automake \
        bison \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        curl \
        docker.io \
        file \
        flex \
        gawk \
        git \
        libbsd-dev \
        zlib1g-dev \
        libjansson-dev \
        libsqlite3-dev \
        sqlite3 \
        lsb-release \
        libexpat1 \
        libexpat1-dev \
        libcurl4-openssl-dev \
        libglib2.0-dev \
        libnetconf2-dev \
        libncurses5 \
        libncurses5-dev \
        libpam0g \
        libpam0g-dev \
        libpcap0.8-dev \
        libpopt-dev \
        libreadline-dev \
        libssh-dev \
        libssh2-1-dev \
        libssl-dev \
        libusb-1.0-0 \
        libusb-1.0-0-dev \
        libxml2 \
        libxml2-dev \
        libxslt1-dev \
        libxslt1.1 \
        libpcre3-dev \
        libyaml-dev \
        m4 \
        meson \
        openssh-server \
        pkg-config \
        perl \
        python3 \
        python3-pip \
        rsync \
        ssh \
        sudo \
        tshark \
        tcl-dev \
        unzip \
        wget

# Apache Arrow and Parquet C++ (libarrow-dev, libparquet-dev) from the
# Apache Arrow apt repository for this Debian release. They ship
# arrow.pc and parquet.pc, which ta_data's meson and build.sh
# --extra-deps resolve.
RUN cd /tmp && \
    wget -q "https://packages.apache.org/artifactory/arrow/debian/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb" && \
    apt-get install -y ./apache-arrow-apt-source-latest-*.deb && \
    apt-get update && \
    apt-get install -y libarrow-dev libparquet-dev && \
    rm -f /tmp/apache-arrow-apt-source-latest-*.deb

# The second Parquet implementation the interoperability test reads
# files from (and the oracle that reads ours back).
RUN pip3 install --no-cache-dir --break-system-packages 'pyarrow==21.0.0'

# The Test Agent application (ta_rpcprovider) is linked with the C
# compiler driver, so it does not pull in the C++ runtime that ta_data's
# Arrow/Parquet unit needs. TE injects link libraries into that app only through
# pkg-config names given to build.sh --extra-deps, and libstdc++ ships no
# .pc, so provide a tiny one and name "stdcxx" in conf/builder.conf.
RUN mkdir -p /usr/lib/pkgconfig && \
    printf 'Name: stdcxx\nDescription: C++ runtime for the TE agent link\nVersion: 1\nLibs: -lstdc++\n' \
        > /usr/lib/pkgconfig/stdcxx.pc

# The build tree lives in /tmp/aibuild; a named volume mounted there
# (scripts/run.sh, TE_DOCKER_CACHE) starts as a copy of this directory,
# so it has to exist and be writable by the container user.
RUN mkdir -p /tmp/aibuild && chmod 1777 /tmp/aibuild

RUN echo "${USER} ALL=NOPASSWD: ALL" >> /etc/sudoers

RUN if [ "$DOCKER_GID" != "${GID}" ] ; then groupadd --gid ${GID} ${USER} ; fi

RUN useradd --system \
            --gid ${GID} --gid ${DOCKER_GID} \
            --uid ${UID} \
            --shell /bin/bash \
            ${USER}
RUN usermod -a -G ${DOCKER_GID} ${USER}
RUN usermod -a -G docker ${USER}
