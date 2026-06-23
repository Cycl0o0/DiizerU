# Multi-stage build for the DiizerU relay.
FROM rust:1-bookworm AS build
WORKDIR /src
# cache deps
COPY relay/Cargo.toml relay/Cargo.toml
RUN mkdir -p relay/src && echo "fn main(){}" > relay/src/main.rs \
    && cd relay && cargo build --release || true
# real sources
COPY relay/ relay/
RUN cd relay && touch src/main.rs && cargo build --release

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3 && rm -rf /var/lib/apt/lists/*
RUN useradd -r -u 10001 diizeru && mkdir -p /data && chown diizeru /data
COPY --from=build /src/relay/target/release/diizeru-relay /usr/local/bin/
USER diizeru
ENV BIND_ADDR=0.0.0.0:8080 STORE_PATH=/data/store.json
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/diizeru-relay"]
