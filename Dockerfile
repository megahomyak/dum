FROM alpine:3.20.3 AS build

WORKDIR /app

RUN apk add --no-cache make=4.4.1-r2 --repository=https://dl-cdn.alpinelinux.org/alpine/v3.20/main
RUN apk add --no-cache gcc=13.2.1_git20240309-r1 --repository=https://dl-cdn.alpinelinux.org/alpine/v3.20/main
RUN apk add --no-cache musl-dev=1.2.5-r1 --repository=https://dl-cdn.alpinelinux.org/alpine/v3.20/main
COPY Makefile dum.c .

RUN make dum


FROM scratch
COPY --from=build /app/dum /dum
ENTRYPOINT ["/dum"]
