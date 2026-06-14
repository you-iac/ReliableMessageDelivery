FROM docker.m.daocloud.io/library/debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive
ARG APT_MIRROR=http://mirrors.ustc.edu.cn/debian
ARG APT_SECURITY_MIRROR=http://mirrors.ustc.edu.cn/debian-security

RUN sed -i \
        -e "s|http://deb.debian.org/debian|${APT_MIRROR}|g" \
        -e "s|http://deb.debian.org/debian-security|${APT_SECURITY_MIRROR}|g" \
        /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get install -y
     --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        libhiredis-dev \
        libmuduo-dev \
        libprotobuf-dev \
        protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target server client --parallel

FROM docker.m.daocloud.io/library/debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV RMD_REDIS_HOST=redis
ENV RMD_REDIS_PORT=6379
ARG APT_MIRROR=http://mirrors.ustc.edu.cn/debian
ARG APT_SECURITY_MIRROR=http://mirrors.ustc.edu.cn/debian-security

RUN sed -i \
        -e "s|http://deb.debian.org/debian|${APT_MIRROR}|g" \
        -e "s|http://deb.debian.org/debian-security|${APT_SECURITY_MIRROR}|g" \
        /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libhiredis-dev \
        libmuduo-dev \
        libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /app --shell /usr/sbin/nologin rmd

WORKDIR /app
COPY --from=build /src/bin/server /app/bin/server
COPY --from=build /src/bin/client /app/bin/client

USER rmd
EXPOSE 8080

CMD ["/app/bin/server"]
