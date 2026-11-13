# Build and test acsh without a Linux machine:
#
#   docker build -t acsh .
#   docker run --rm -it acsh              start acsh
#   docker run --rm acsh make test        run every test
#   docker run --rm acsh make test-debug  the same under the sanitizers
#
# The base image is pinned by digest, so it's the same image every time.
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c

# libclang-rt-21-dev holds clang's AddressSanitizer and UBSan runtime,
# without which `make CC=clang test-debug` doesn't link. clang here is
# version 21. clang-tidy is for `make analyze`.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential clang clang-tidy libclang-rt-21-dev valgrind python3 python3-venv \
 && rm -rf /var/lib/apt/lists/*

# A normal user, not root: root can read any file, so tests that expect
# "Permission denied" would pass for the wrong reason or fail.
RUN useradd --create-home tester
USER tester
WORKDIR /home/tester/acsh

COPY --chown=tester . .

# Build, and install the test tools now, so `make test` needs no network later.
RUN make && make .venv/ready

CMD ["build/acsh"]
