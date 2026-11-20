FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc libc6-dev make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
RUN make all

FROM ubuntu:22.04

RUN mkdir -p /data
COPY --from=builder /src/build/pagestore  /usr/local/bin/pagestore
COPY --from=builder /src/build/pagestored /usr/local/bin/pagestored

EXPOSE 7654
VOLUME /data
WORKDIR /data

ENTRYPOINT ["pagestored"]
CMD ["/data", "7654"]
