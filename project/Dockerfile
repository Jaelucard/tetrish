FROM debian:bookworm-slim
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      pkg-config \
      libssl-dev \
      libncurses-dev \
      valgrind \
      gdb strace lsof \
      git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work/project
CMD ["bash"]
