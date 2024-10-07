FROM alpine:3.20.3 AS build

WORKDIR /app

RUN apk add --no-cache make=4.4.1-r2
RUN apk add --no-cache gcc=13.2.1_git20240309-r0
RUN apk add --no-cache musl-dev=1.2.5-r0
COPY Makefile dum.c .

RUN make dum


FROM scratch
COPY --from=build /app/dum /dum
ENTRYPOINT ["/dum"]
