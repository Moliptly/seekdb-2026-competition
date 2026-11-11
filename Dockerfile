# MiniVectorDB - the image the evaluation platform builds.
#
# The build stage compiles the database from the sources that live in this
# repository; the runtime stage carries nothing but the resulting binary, so
# the image has no runtime dependency besides a C library and the container
# needs no network at run time.
#
# The program speaks the JSON lines protocol on stdin/stdout (see
# STDIO_PROTOCOL.md) and writes every file it creates below /data.
FROM gcc:13-bookworm AS build

WORKDIR /build
COPY src/ ./src/
# The build id is the hash of the sources; it is printed on stderr when the
# database starts, so the log of a case shows which build produced it.
RUN BUILD_ID=$(cat src/*.c src/*.h | sha256sum | cut -c1-12) \
 && gcc -std=gnu11 -O3 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation \
        -DMVDB_BUILD_ID="\"$BUILD_ID\"" \
        -o /build/minivectordb src/*.c -lm \
 && strip /build/minivectordb

FROM debian:bookworm-slim

COPY --from=build /build/minivectordb /usr/local/bin/minivectordb
RUN mkdir -p /data
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/minivectordb", "--data-dir", "/data"]
