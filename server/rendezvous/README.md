# Infested rendezvous server

Lets players join each other's matches with no port forwarding, UPnP or VPN.
The server only **introduces** players: the host registers and gets a
six-digit game code, and a survivor picks the game from the public list or
types its code. The server then tells each side the other's address, both
punch through their own routers, and the match runs **peer to peer**. For
the rare pair whose routers won't let a direct link through, the server
**relays** that one link, within hard caps on sessions, bandwidth and data a
month. Every other link stays direct.

- `rendezvous.c`: the server. C11, no dependencies, one UDP port (27961 by
  default), one thread, fixed tables (16384 games, 1024 relay sessions).
  Handling a packet takes 15–170 ns (heartbeat to a full list, with 5000
  games) and forwarding a relayed one 13 ns, so the socket's own system
  calls are most of the cost.
- `Dockerfile`: a static build in an empty image, about 1 MB. Build it from
  the repository root, because the server includes the game's wire header.
- Wire format: `src/game/mods/ZombieRendezvousWire.h`, plain C, compiled
  into both the game and the server.
- Tests: `python3 tests/test_zombie_rendezvous.py` covers the server's rules,
  400,000 fuzzed packets under AddressSanitizer / UBSan, the binary over a
  real socket, and the real game-side client against it across simulated
  NAT routers.

## Running it locally

```
cc -O2 -o rendezvous server/rendezvous/rendezvous.c
./rendezvous --port 27961        # -v adds a stats line a minute, -q silences it
```

Relay caps (defaults in brackets):

| Flag | Meaning |
|---|---|
| `--max-relays N` | relay sessions at once [64]; `0` turns the relay off |
| `--relay-kbps K` | KB/s per session, both directions together [384] |
| `--relay-total-mbps M` | Mbit/s for all sessions together [64] |
| `--relay-gb-month G` | GB relayed per calendar month (UTC) [150] |

A relayed survivor uses about 50–90 KB/s each way (about 250 MB an hour),
so the per-session cap leaves room for a busy room and stops anything
bigger. Once the month's budget is used, new sessions are refused and open
ones carry on. The count is kept in memory, so a restart starts it again;
the per-second caps still bound the worst case. 150 GB is 100 GB inside
Azure's free monthly egress plus about $4.50 more.

Then in each player's `config.ini`:

```
[Net]
Rendezvous=127.0.0.1:27961
```

(`host` or `host:port`; `Rendezvous=off` turns it off.) Once a public server
exists, put its name in `ZM_RDV_DEFAULT_SERVER` in `ZombieRendezvous.h`, so
players don't need the key at all.

## Azure Container Instances (no VM)

ACI runs a single container with a public IP and accepts **UDP**, which
Functions, App Service and Container Apps don't. 0.25 vCPU / 0.5 GB running
all month costs roughly $8–10; the server needs a fraction of that.

```bash
az login
az group create -n infested-rdv -l eastus

# A registry that builds the image in the cloud (no local Docker needed).
# Basic is about $5 a month; a public GHCR or Docker Hub image is free instead.
az acr create -g infested-rdv -n infestedrdv --sku Basic --admin-enabled true
# From the repository root: the build uses the root .dockerignore, which
# uploads only rendezvous.c and the wire header.
az acr build -r infestedrdv -t rendezvous:1 -f server/rendezvous/Dockerfile .

az container create -g infested-rdv -n rendezvous \
  --image infestedrdv.azurecr.io/rendezvous:1 \
  --registry-login-server infestedrdv.azurecr.io \
  --registry-username infestedrdv \
  --registry-password "$(az acr credential show -n infestedrdv --query 'passwords[0].value' -o tsv)" \
  --os-type Linux --cpu 0.25 --memory 0.5 \
  --ports 27961 --protocol UDP \
  --ip-address Public --dns-name-label infested-rdv \
  --restart-policy Always
```

To change the relay caps, add for example
`--command-line "/rendezvous --port 27961 --relay-gb-month 100"` to
`az container create`.

The server is then `infested-rdv.eastus.azurecontainer.io:27961`. The IP can
change when the container is recreated, but the DNS name doesn't, so give
players the name, not the IP.

Logs: `az container logs -g infested-rdv -n rendezvous`.
Update: run `az acr build ... -t rendezvous:2 -f server/rendezvous/Dockerfile .`, then `az container create` again with the new tag.

### Check before relying on it

Hole punching needs the server's replies to come **from the same IP:port the
players sent to**; many home routers drop anything else. Run from home:

```
python3 tools/rendezvous_probe.py infested-rdv.eastus.azurecontainer.io
```

`OK` means the server is usable. If it fails on the address check, the
smallest Azure VM (B1ls) is the fallback: copy the binary over, or build it there with
`cc -O2`, and run it under systemd.

## What gets through and what doesn't

| Players' routers | Result |
|---|---|
| Normal home routers (cone NAT) | Direct link |
| One side with a public IP | Direct link |
| Both on the same LAN | Direct link over the LAN address |
| One side symmetric (some mobile / CGNAT), the other lenient | Usually a direct link |
| Both symmetric | Relayed through this server (the survivor's lobby says so) |
| Both symmetric, relay full or off | Fails: "COULD NOT REACH THE HOST" - use a VPN (Tailscale) or join by address |

The survivor tries the direct link for 10 s first and keeps trying while it
asks for the relay, so a late direct link still wins. The relay adds the
trip through the server's region to that survivor's ping.

## Limits

- Codes are random among 100000–999999 and dropped 30 s after the host's last
  heartbeat (every 5 s for the whole match, so a crashed survivor's REJOIN
  still finds it).
- 60 JOINs and 30 new codes per source IP per minute (token buckets that
  refill continuously), generous enough for players sharing one public IP.
- The list shows at most 64 public games, matching the game's protocol
  version, not started and not full.
- Relay sessions: at most 4 per survivor IP and 6 per game (3 survivors
  plus stale ones from rejoins). A session only carries packets from its
  two endpoints, closes after 60 s of silence from either side, and lasts
  at most 3 hours.
