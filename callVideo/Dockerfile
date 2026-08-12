# --- build stage -----------------------------------------------------------
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base
WORKDIR /src
COPY Makefile .
COPY src ./src
RUN make LDFLAGS="-lpthread -static"

# --- runtime stage -----------------------------------------------------------
# Fully static musl binary -> no libc needed at runtime, so scratch works.
FROM scratch
WORKDIR /app
COPY --from=build /src/bin/server ./bin/server
COPY public ./public

EXPOSE 8080
ENTRYPOINT ["./bin/server", "8080", "public"]
