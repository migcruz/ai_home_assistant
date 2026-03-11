# embedded — Zephyr RTOS Dev Environment

Containerized Zephyr 4.3.0 workspace for the home AI assistant embedded nodes.
All builds happen inside a Dev Container; flashing runs in the dev container as well.

---

## Prerequisites

- Docker
- VS Code + [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)

---

## First-time setup

1. Open the `embedded/` folder in VS Code (**not** the repo root):
   ```
   File → Open Folder → .../home_ai_assistant/embedded/
   ```

2. When prompted, click **Reopen in Container** (or run `Dev Containers: Reopen in Container` from the command palette).

3. VS Code builds the Docker image and runs `postCreateCommand`, which:
   - Initialises the west workspace at `/zephyr-ws/`
   - Fetches Zephyr 4.3.0 + all modules (`west update`)
   - Downloads any vendor binary blobs required by hardware drivers (`west blobs fetch`)

   This takes a while on first run. Subsequent opens are fast — the named volume `home-ai-zephyr-workspace` persists the Zephyr sources across container rebuilds.

4. The file explorer opens at `/zephyr-ws/`, giving you visibility into:
   - `embedded/` — your app code (bind-mounted from host, edits are live)
   - `zephyr/` — Zephyr RTOS source
   - `modules/` — HAL, mbedtls, crypto, etc.

---

## Workspace layout

```
/zephyr-ws/                  ← west workspace root (named Docker volume)
├── .west/                   ← west config
├── zephyr/                  ← Zephyr 4.3.0 source
├── modules/                 ← HAL modules, mbedtls, …
└── embedded/                ← bind-mounted from host
    ├── west.yml             ← west manifest (pins Zephyr v4.3.0)
    ├── .devcontainer/
    │   ├── Dockerfile       ← zephyr-build + SDK 0.17.4 (all toolchains)
    │   └── devcontainer.json
    └── voice_node/          ← voice firmware (AMP: procpu + appcpu)
        ├── Makefile
        ├── overlays/        ← explicit DTS overlays for both cores
        ├── procpu/          ← Core 0 image (WiFi, WebSocket, button, shell)
        └── appcpu/          ← Core 1 image (PDM mic capture)
```

---

## Building a Zephyr image

Open a terminal inside the Dev Container and `cd` into an app directory:

```bash
cd /zephyr-ws/embedded/voice_node

make          # incremental build
make clean    # wipe build dir
make menuconfig  # interactive Kconfig browser
```

Build output lands in `voice_node/build/` (for example), which is bind-mounted and visible in your regular VS Code window.

To target a different board:
```bash
make BOARD=nrf52840dk/nrf52840
```

### Manual west build

```bash
# from /zephyr-ws
west build -d embedded/voice_node/build \
           -b xiao_esp32s3/esp32s3/procpu/sense \
           embedded/voice_node

# pristine (full clean rebuild)
west build --pristine -d embedded/voice_node/build \
           -b xiao_esp32s3/esp32s3/procpu/sense \
           embedded/voice_node
```

---

## Flashing a Zephyr Image

Flashing requires USB access to the board, so it runs **on the host** (not inside the container). The build artifact is at `voice_node/build/zephyr/zephyr.bin` (for example) on the host since the build dir is bind-mounted.

Some MCUs require manually entering bootloader mode before flashing (e.g. hold BOOT, tap RESET, release BOOT). Check your board's datasheet if the flash command fails or times out.

```bash
# on the host, from the embedded/ directory
make flash
```

---

## Serial monitor

Use minicom, or picocom, our puTTY to get serial output from the device after connecting it to the host.

```bash
picocom /dev/ttyACM0          # 8N1, baud rate ignored by hardware
```

Exit: `Ctrl+A` then `Ctrl+X`.

If Zephyr shell is running (assuming you enabled it in the build) you'll get a prompt:
```
uart:~$
```

---

## Adding a new app

1. Create a new directory under `embedded/`:
   ```
   embedded/
   └── my_node/
       ├── CMakeLists.txt
       ├── prj.conf
       ├── Makefile          ← copy from voice_node, set BOARD
       └── src/
           └── main.c
   ```

2. Build: `cd /zephyr-ws/embedded/my_node && make`

All apps share the same Zephyr + modules in the named volume. No per-app container needed.

---

## Rebuilding the container

If `Dockerfile` changes (e.g. adding packages):
```
Dev Containers: Rebuild Container
```

To also wipe the Zephyr sources volume and re-fetch everything from scratch:
```bash
docker volume rm home-ai-zephyr-workspace
```
Then reopen in container.

---

## Shelling into the container from an external terminal

If you prefer your own terminal (e.g. Ghostty) over the VS Code integrated terminal, you can drop into the running container with `docker exec` while the Dev Container is open in VS Code.

**Step 1 — find the container name:**
```bash
docker ps
```
Sample output:
```
CONTAINER ID   IMAGE                        COMMAND   CREATED        STATUS        NAMES
a3f2c1d80b44   vsc-embedded-abc123-uid      "/bin/sh" 2 hours ago    Up 2 hours    crazy_ganguly
```
The container name is in the last column (`crazy_ganguly` in this example). VS Code assigns a random name — use `docker ps` each session to find it.

Alternatively, filter by image name (the image is always prefixed with `vsc-embedded`):
```bash
docker ps --filter "ancestor=$(docker images --format '{{.Repository}}' | grep vsc-embedded | head -1)"
```

**Step 2 — attach a shell:**
```bash
docker exec -it crazy_ganguly /bin/bash
```

You land inside the container as the `user` account with the full environment — `ZEPHYR_BASE`, `PATH`, west, and all toolchains available. The `/zephyr-ws/embedded/` bind mount is live, so edits from either terminal are immediately visible everywhere.

---

## Troubleshooting

**Container won't start / stuck rebuilding**

Force-remove the container and let VS Code recreate it:
```bash
# find the container name
docker ps -a | grep embedded

# force remove it (safe — all persistent state is in the named volume, not the container)
docker rm -f <container-name> # can also use the hash instead of the name
```
Then run `Dev Containers: Reopen in Container` in VS Code.

**`west update` or blob fetch failed mid-run**

The named volume may be in a partial state. Wipe it and start fresh:
```bash
# volume can't be removed while the container is running — stop it first
docker rm -f <container-name>
docker volume rm home-ai-zephyr-workspace
```
Then reopen in container. `postCreateCommand` will re-fetch everything.

**Build directory is stale after a failed configure step**

```bash
# inside the container
make clean
# or equivalently
west build --pristine -d build -b xiao_esp32s3/esp32s3/procpu/sense .
```

**`docker volume rm` says "volume is in use"**

The container is still running. Stop it first:
```bash
docker rm -f <container-name>
docker volume rm home-ai-zephyr-workspace
```
