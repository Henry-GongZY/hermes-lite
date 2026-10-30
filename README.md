# hermes-lite

A minimal, portable **agent runtime** in C — the embedded counterpart of the
[Hermes Agent](https://github.com/NousResearch/hermes-agent) ecosystem. It runs
the classic agent loop (context assembly → cloud LLM → tool calls → repeat)
on microcontrollers, and speaks the **agentskills.io skill format**, so
markdown skills written for Hermes-style agents work here unchanged.

```
cloud LLM (any OpenAI-compatible API)  ←  the "brain"
        ▲ HTTPS / JSON
hermes-lite agent loop                 ←  ~3k lines of C99
        ▼ local tool calls
GPIO / ADC / shell / your C functions  ←  the "hands"
```

## Layout

```
components/hermes_core/   portable core: JSON, agent loop, skills, history
  ├── include/hermes_lite.h   public API
  └── src/                    + platform_posix.c / platform_esp32.c backends
examples/linux/           terminal demo (exec + time tools)
examples/esp32/           ESP-IDF firmware (GPIO/ADC/sysinfo tools, serial REPL)
test/                     JSON parser unit tests
```

The core is strict C99 with one platform interface (`hl_platform_t`):
`http_post`, four filesystem ops, `time_ms`, `log`. A new port (STM32, RP,
TI, Zephyr…) is exactly that vtable — the agent loop never changes.

## Features

- OpenAI-compatible `/v1/chat/completions`, parallel tool calls, configurable
  iteration limit
- **SSE streaming**: reply fragments arrive via callback (live output on the
  device serial console); automatic fallback to buffered mode
- Skills: `SKILL.md` frontmatter (`name`/`description`) indexed into the
  system prompt; full body loaded on demand via the built-in `read_skill`
  tool (progressive disclosure — a big library costs ~nothing per request)
- JSONL conversation history with char-budget context trimming and
  **automatic compaction** (file size stays bounded; flash-wear friendly)
- Arena-based JSON parser (no malloc inside the parser; bounded memory)
- **Test suite**: 5 JSON parser cases + 13 agent-loop cases (tool calls,
  parallel calls, unknown tools, iteration limits, skills, context trimming,
  history reload/compaction, streaming) driven by a scripted mock backend

## Host (Linux / macOS)

```bash
make && make test
HL_API_KEY=sk-... ./build/hermes-lite          # exec/time tools, ./skills dir
HL_SKILLS_DIR=path/to/skills HL_API_KEY=... ./build/hermes-lite
```

## ESP32 (AI-Thinker ESP32-S or any ESP32)

```bash
cd examples/esp32
. ~/esp-idf/export.sh
idf.py set-target esp32
# set your endpoint/key in menuconfig: Hermes Lite
idf.py build && idf.py -p /dev/cu.usbserial-0001 flash
# skills partition (1.5MB SPIFFS at 0x10000):
./scripts/flash_skills.sh /dev/cu.usbserial-0001
```

Serial REPL (115200): `wifi <ssid> <pass>` (persisted to NVS), `skills`,
`reset` (wipe history), `free`, anything else → the agent. Built-in tools:
`gpio_write`, `gpio_read`, `adc_read` (ADC1 ch0-7 = GPIO32-39), `sys_info`.
Replies stream token-by-token over the serial console.

## Zephyr (Tier 2)

The repository root is a Zephyr module (`zephyr/module.yml`). The core builds
and runs on any Zephyr target; `components/hermes_core/src/platform_zephyr.c`
implements the backend with zsock + mbedTLS + littlefs.

```bash
# verify the agent loop under the Zephyr kernel, no hardware needed
# (any QEMU board works; mps2/an385 has roomy RAM for the TLS-enabled config):
west build -b mps2/an385 hermes-lite/zephyr/samples/agent_demo \
    -DZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
    -DGNUARMEMB_TOOLCHAIN_PATH=$HOME/arm-gnu-toolchain \
    -DEXTRA_ZEPHYR_MODULES=$(pwd)/hermes-lite
west build -t run    # runs QEMU; prints ALL ZEPHYR TESTS PASSED
```

Notes: Zephyr's POSIX arch (native_sim) is Linux-only, hence the QEMU board.
The sample uses newlib (`CONFIG_PICOLIBC=n`) — picolibc misbehaves under the
brew ARM toolchain. TLS is compiled out of the sample (see prj.conf); enable
`CONFIG_NET_SOCKETS_SOCKOPT_TLS=y` on real boards, then call
`hl_zephyr_set_ca(pem)` + `hl_zephyr_fs_init()` at boot and pass
`hl_platform_zephyr()` to `hl_agent_new`.

## Memory budget (ESP32)

Agent buffers: 24KB request + 24KB response + 24KB JSON arena + 8KB history +
8KB skill scratch ≈ 88KB heap, ~109KB free heap reported at runtime alongside
WiFi. TLS uses the built-in certificate bundle (`CONFIG_HL_TLS_INSECURE=y`
skips verification — dev only).

## Roadmap

- [x] SSE streaming responses (live fragments; buffered fallback)
- [x] History compaction (bounded file size)
- [x] Zephyr module + native_sim self-test
- [ ] Zephyr on real hardware (STM32 / RP2350 / TI CC32xx) end-to-end
- [ ] TLS session resumption to cut handshake RAM/time
- [ ] Over-the-air skill sync from a Skills Hub

## License

MIT
