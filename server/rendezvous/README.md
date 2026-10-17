# Infested rendezvous server

Lets players join each other's matches with no port forwarding, UPnP or VPN.
The server only **introduces** players: the host registers and gets a
six-digit game code, and a survivor picks the game from the public list or
types its code. The server then tells each side the other's address, both
punch through their own routers, and the match runs **peer to peer**. No
game traffic passes through this server; it handles a few small packets per
match.

- `rendezvous.py`: the server. Python 3.8+, standard library only, one UDP
  port (27961 by default).
- `Dockerfile`: a ~50 MB Alpine image that runs it.
- Wire format: `src/game/mods/ZombieRendezvous.h` (the two must agree).
- Tests: `python3 tests/test_zombie_rendezvous.py` runs the real game-side
  client against this server across simulated NAT routers.

## Running it locally

```
python3 server/rendezvous/rendezvous.py --port 27961 -v
```

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
all month costs roughly $8–10.

```bash
az login
az group create -n infested-rdv -l eastus

# A registry that builds the image in the cloud (no local Docker needed).
# Basic is about $5 a month; a public GHCR or Docker Hub image is free instead.
az acr create -g infested-rdv -n infestedrdv --sku Basic --admin-enabled true
az acr build -r infestedrdv -t rendezvous:1 server/rendezvous

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

The server is then `infested-rdv.eastus.azurecontainer.io:27961`. The IP can
change when the container is recreated, but the DNS name doesn't, so give
players the name, not the IP.

Logs: `az container logs -g infested-rdv -n rendezvous`.
Update: `az acr build ... -t rendezvous:2`, then `az container create` again with the new tag.

### Check before relying on it

Hole punching needs the server's replies to come **from the same IP:port the
players sent to**; many home routers drop anything else. Run from home:

```
python3 tools/rendezvous_probe.py infested-rdv.eastus.azurecontainer.io
```

`OK` means the server is usable. If it fails on the address check, the
smallest Azure VM (B1ls) running `rendezvous.py` under systemd is the
fallback.

## What gets through and what doesn't

| Players' routers | Result |
|---|---|
| Normal home routers (cone NAT) | Direct link |
| One side with a public IP | Direct link |
| Both on the same LAN | Direct link over the LAN address |
| One side symmetric (some mobile / CGNAT), the other lenient | Usually a direct link |
| Both symmetric | Fails: "COULD NOT REACH THE HOST". Use a VPN (Tailscale) or join by address |

There's no relay: a pair that can't punch through gets an error rather than
having its traffic carried by the server.

## Limits

- Codes are random among 100000–999999 and dropped 30 s after the host's last
  heartbeat (every 5 s for the whole match, so a crashed survivor's REJOIN
  still finds it).
- 20 JOINs and 8 new codes per source IP per minute.
- The list shows at most 64 public games, matching the game's protocol
  version, not started and not full.
