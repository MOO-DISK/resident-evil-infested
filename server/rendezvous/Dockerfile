# Build from the repository root - the server includes the game's wire header:
#   docker build -f server/rendezvous/Dockerfile -t infested-rdv .
# (The root .dockerignore sends only the two files below as the context.)
FROM alpine:3.20 AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY src/game/mods/ZombieRendezvousWire.h src/game/mods/
COPY server/rendezvous/rendezvous.c server/rendezvous/
RUN gcc -std=c11 -O2 -Wall -Wextra -Werror -static -o /rendezvous server/rendezvous/rendezvous.c

# A static binary needs nothing else: an empty image, about 1 MB.
FROM scratch
COPY --from=build /rendezvous /rendezvous
USER 65534:65534
EXPOSE 27961/udp
ENTRYPOINT ["/rendezvous", "--port", "27961"]
