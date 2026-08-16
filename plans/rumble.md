# Rumble / haptics for Leaf (MLP1)

Status: **Phase 1 and Phase 2a built, tuned, audited and in review** (2026-07-25). Design
settled with Eric via an on-device exploration of the motor plus a full design grill; the
timings and floors were then measured on the device (section 6); a multi-agent audit then
found and fixed a further round of defects (section 8). Open PRs: Jawaka #11, retroarch-builds
#2, Leaf #17 - merge retroarch-builds #2 before Leaf #17. Phase 2b (standalone emulators) and
the variable-magnitude check are deliberately held as separate work (section 9).

The MLP1 has a rumble motor. Stock LoongOS drives it; Leaf never has. This wires it up.

---

## 1. Hardware findings (verified on Puff)

The rumble motor is a **single PWM-driven motor**, not a Linux force-feedback input
device. Confirmed on device:

- The `Loong Gamepad` input nodes (`event4`/`event5`) report `EV=b` (SYN+KEY+ABS) with
  **no `EV_FF` bit** — so there is no `/dev/input` FF interface. Emulator/SDL rumble
  will not reach the motor without help (this drives the whole Phase 2 approach).
- The motor is **`/sys/class/pwm/pwmchip0/pwm0`** (`npwm=1`, a dedicated single channel).
- Control: `duty_cycle` = strength, `enable` = 1/0, `period` = frequency.
- **Period is `1,000,000 ns` (1 kHz)** — set by the stock startup `/etc/init.d/S50loong`
  (`echo 1000000 > .../pwm0/period`). libloong itself only writes `duty_cycle` + `enable`;
  it assumes the period is already set. On a fresh export the period reads back `0`, so
  **Leaf must set the period itself.**
- Vendor plumbing exists but Leaf will not use it: `libloong_gui.so` exposes
  `lgui::LSound::vibrateOnce(int)`, `setVibration(bool)`, `setVibrationLevel(int)`, and the
  stock `SOUND_PARAM` config carries `vibrateFb` / `vibrateLevel`. We drive the PWM
  directly instead (no C++ vendor-lib coupling).

### The stuck-motor lesson (do not repeat)

During exploration the motor got **stranded on** twice. Root causes, and the rule that
prevents them:

- `echo 0 > enable` does **not** reliably stop this motor — the output pin latches its
  last state.
- **Unexporting** the channel while it is driven latches it on.
- Under the stock default `polarity=inversed`, `duty_cycle=0` is **full on**, not off.

**Rule:** the reliable "off" is to *actively drive 0% output*, never to disable/unexport.
The drive model below is built around this.

---

## 2. Drive model (the technical core)

Configure the channel **once** at daemon init and then only ever modulate `duty_cycle`:

```
export → enable=0 → polarity=normal → period=1000000 → duty_cycle=0 → enable=1
```

From then on:

- **buzz** = `duty_cycle = <strength>`  (higher = stronger; 1,000,000 = full)
- **stop** = `duty_cycle = 0`  (channel stays enabled — 0% output = motor off)
- **never** toggle `enable`, **never** unexport.

Decisions and rationale:

- **Polarity is read back, never assumed** - and the channel now comes up **`normal`**.
  For most of this project we believed the driver *rejected* `polarity=normal` and forced us
  to live with `inversed`, where `duty_cycle = 0` is FULL ON. That was a misdiagnosis, found
  2026-07-26: the driver rejects the polarity write only while `period` is still `0`, which
  is the state of a freshly exported channel. The old init wrote polarity **before** period,
  so the write always failed with EINVAL, the read-back said `inversed`, and we concluded the
  hardware refused. **Write `period` first and `normal` takes.** On a fresh export this
  driver also rejects `duty_cycle` and `enable` for the same reason, so period is simply the
  write that makes every other one legal.

  This is why the ordering is load-bearing rather than stylistic, and it defuses the whole
  feature's marquee hazard: under `normal`, a failed or zeroed duty write means **off**, not
  full power. The daemon still re-reads and adapts (`off = 0` when normal, `period` when
  inversed), because a channel someone else left inversed must still be handled.
- **Always-enabled / modulate-duty-only** is what makes "off" bulletproof — it is impossible
  to strand the motor because the channel is always actively driving a defined level.
- **Energy:** keeping the channel enabled at 0% costs nothing meaningful. The motor draws
  zero at 0% duty (the only real consumer); the PWM block's idle clock/leakage is
  microamp-scale, dwarfed by SoC/panel/Wi-Fi. Not worth optimizing.
- **Forced-off on state transitions:** the daemon forces `duty_cycle=0` on **game launch,
  sleep/suspend, and shutdown**, so a pulse can never be left stranded across a transition.

### Strength & the "amplitude vs pattern" finding

On-device A/B testing showed **duty (amplitude) mostly changes the audible buzz, not the
perceived force** — a light vs strong duty is hard to tell apart by feel, easy by ear.
**Short sharp burst *patterns* differentiate far better** than amplitude. So:

- **Patterns** (burst count) carry *meaning* (see the vocabulary).
- **Strength** (duty) is a single user intensity control on top; it is a scale/ceiling, not
  the differentiator.

Tick shape and floors were later **measured** on Puff rather than guessed - see section 6,
which supersedes the ~40 ms / ~40% floor figures this section originally carried.

---

## 3. Architecture

- **jawakad owns the motor.** It is the root process already driving LED/brightness/PWM
  sysfs. It configures the channel at init, owns the event→pattern table and the tick
  timings, scales amplitude by the user's strength setting, and owns the safe-stop.
- **UI processes trigger by naming a semantic event.** The launcher / menu send a
  fire-and-forget `rumble` IPC action (mirroring the existing `led` / `brightness` /
  `hdmi_output` actions) carrying an *event name*, not raw duty/duration. The daemon maps the
  event → pattern → duty. This keeps all feel-tuning in one place and lets us retune patterns
  later without touching launcher/menu code.
  - IPC shape: `{ "action": "rumble", "event": "select" | "commit" | "blocked" | "nav" }`
  - Fire-and-forget (no wait for a reply) so per-press haptics add no perceptible latency.
- **Non-blocking pulses.** A small rumble worker thread in the daemon (like the screenshot
  worker) takes a pattern request and pulses `duty` with sleeps, so the IPC loop never
  blocks. A new request coalesces (restarts) rather than queueing, so rapid input does not
  back up a buzz train.

---

## 4. Phase 1 — UI haptics (build first)

### Vocabulary — event → pattern (fixed, not user-configurable)

Patterns are keyed to the **semantic weight of the action**, so users learn the language
(double = "into a game", triple = "nope") without configuring anything:

| Pattern       | Meaning              | Fires on |
|---------------|----------------------|----------|
| **Single tick** | routine / registered | cursor select, back/cancel, toggling a setting, a slider notch; and cursor **move** *only if* nav-tick is on |
| **Double tick** | commit / significant  | launching a game (leaving into it), confirming a modal/important dialog, applying a change |
| **Triple tick** | blocked / negative    | hitting a list boundary, a denied/locked action (e.g. a 5-Game-Mode lock), an operation error |

The UI emits named events (`RUMBLE_NAV`, `_SELECT`, `_COMMIT`, `_BLOCKED`); the daemon maps
`nav/select → single`, `commit → double`, `blocked → triple`.

### Settings — new "Controls & Feedback" section

A **new top-level Settings section, "Controls & Feedback"** (the current top level is
Appearance / Display & Sound / Lighting / Network / Bluetooth / … / General; internal page
name for "General" is `JW_SETTINGS_BEHAVIOR`). Rumble does not belong in Display & Sound.

Seed the section with:

- **Rumble** — UI/menu haptics on/off. **Default ON.** Game rumble is controlled
  independently by the Phase-2 toggle below.
- **Strength** — a **percent slider** reusing the existing brightness/volume track+fill
  widget (`settings.c` ~1866–1871, L/R to adjust), default ~65%, with a **live preview
  buzz** as you drag so you feel the level while setting it. (Slider only — no named
  presets; the slider supersedes Light/Med/Strong.)
- **Navigation tick** — on/off, **default OFF** (per-move buzz is the most polarizing; opt-in).
  Greyed unless Rumble is on.
- **Screenshots (Menu+L1)** — **migrated here** out of General (it is a hotkey/input feature,
  not general behavior), so the new section launches with substance and General loses a
  grab-bag item.

DB keys: `rumble_enabled` (bool, default true), `rumble_strength` (int %, default ~65),
`rumble_nav` (bool, default false). The daemon caches these (like it caches
`screenshots_enabled`) so an incoming `rumble` action knows whether/how hard to buzz.

Room to grow in this section later: controller button mapping, menu A/B swap, stick-calibration
link, and the Phase-2 game-rumble toggle.

### Init & lifecycle

- At daemon start: configure the channel per §2 (set the period — Leaf must, since libloong
  does not).
- Force `duty=0` on game launch, sleep/suspend, shutdown.
- Respect `rumble_enabled` (drop all pulses when off) and `rumble_nav` (drop `nav` events
  when off) daemon-side.

---

## 5. Phase 2 — game rumble

**2a (RetroArch) is built.** 2b (standalone emulators) remains deferred.

Because the motor is not an FF/SDL device, emulator rumble needs an explicit path to the PWM.

### RetroArch cores (the bulk of systems) — Phase 2a  ✅ built

`retroarch-builds/patches/common/0003-sysfs-rumble-fallback.patch` is enabled in the MLP1
build as the patch-set entry **`sysfs-rumble`** (the shipped set is now
`portrait-rotation,command-menu,jawaka-load-content,sysfs-rumble`). It writes sysfs directly
when the joypad driver's native rumble returns false — which it always does here, since
there is no FF device.

:::note[Superseded — kept for the record]
Everything in this subsection describes the **first** Phase 2a design, which no longer
exists. Force feedback on the virtual pad (§ Phase 2b) made every consumer of the
`RUMBLE_PWM_*` contract redundant, and the contract was deleted in `c18ccbf` so the PWM
would have exactly one writer. RetroArch now rumbles through the same route as everything
else. It is preserved because the endpoint maths below still describes how magnitude maps
onto duty, which the force-feedback path kept.
:::

The patch gained a **PWM duty mode** for this device. jawakad keeps owning the channel
(exported, enabled, resting off), so RetroArch only ever writes `duty_cycle` and never
touches export/polarity/period/enable. Rather than teach RA about polarity, the period or
the stiction floor, the daemon handed it ready-made duty endpoints:

| Env var | Meaning |
| --- | --- |
| `RUMBLE_PWM_PATH` | the `duty_cycle` node |
| `RUMBLE_PWM_OFF`  | duty that means off (polarity-dependent: `0` or `period`) |
| `RUMBLE_PWM_MIN`  | duty at the weakest magnitude that still moves the motor (the floor) |
| `RUMBLE_PWM_MAX`  | duty at full magnitude, already capped to the user's strength setting |

Core magnitude (0-65535) maps linearly onto `[MIN,MAX]`; MIN/MAX may run in either
direction, so inversed polarity needs no special case in RA. The patch skips the sysfs
write when the computed duty is unchanged, since cores re-assert rumble every frame.

jawakad resolves the endpoints in the parent (it reads the DB) and applies them with
`setenv` in the forked child, so the daemon's own environment never carries them. It forces
`duty=off` immediately before the launch fork and again when any child exits, so a game that
dies mid-buzz cannot strand the motor.

**Permission: resolved.** Everything on this device runs as root — jawakad, its launcher
child, and RetroArch (verified `uid=0` on Puff) — so `duty_cycle` being root-owned is a
non-issue. No `chmod` dance is needed.

### Standalone emulators (Flycast / PPSSPP / DraStic / mupen64) — Phase 2b

The first answer here was wrong, and it is worth recording why, because the better one was
sitting underneath the whole time.

The premise was that an emulator has "no path to the motor", so each would need its own patch
writing `duty_cycle`. That is how Flycast got done. But the premise only held because of a
detail we had put there ourselves: the joystick these emulators open is **not** the physical
pad, it is jawakad's own calibrated virtual gamepad, and `input_proxy_mlp1.c` was building it
by copying `EV_KEY` and `EV_ABS` from the hardware and stopping. The pad reported no force
feedback because we never gave it any.

So the pad advertises `FF_RUMBLE` now. The daemon created the device, which means the kernel
routes every `EVIOCSFF` upload and every `EV_FF` play back to the daemon — where the motor
already lives. An emulator rumbling through ordinary SDL reaches the PWM with **no patch of
its own**: `SDL_JoystickRumble` → `EVIOCSFF` → jawakad → `duty_cycle`. Verified on Puff with
Ocarina of Time, which needed only a config change (below) and not one line of C.

Two consequences worth stating plainly:

- **`main.c:6549` decides who benefits.** Mupen64Plus, Flycast, PPSSPP and PortMaster ports all
  run on the calibrated virtual pad, so all four are covered. An emulator that reads the
  physical pad directly is not, and would still need a sink.
- **RetroArch changes route.** Its udev joypad driver will now find real force feedback and use
  it, which means the sysfs fallback stands down on its own. That is why the daemon publishes
  the FF endpoints for the RetroArch launch too — the env contract alone would leave it silent.
  The two cannot both drive the motor: the fallback only engages when native rumble declines.

**Latency is the reason this has its own thread.** `EVIOCSFF` blocks the calling emulator until
the daemon answers it, and SDL re-uploads on every magnitude change. Served from the 50 ms
housekeeping loop that would be a three-frame stall each time rumble starts or stops, so the
proxy runs a thread blocked in `poll()` instead and answers in well under a millisecond.

**Magnitude:** one motor, two channels, so the louder of strong/weak wins. Averaging would
dilute a strong-only effect and could drop a weak-only one under the stiction floor — the two
most common single-channel cases both come out wrong.

**N64 needed a config change, not code.** The N64 had one expansion slot and so does
mupen64plus: `plugin = 5` (raw / Rumble Pak) reaches the motor, `plugin = 2` (Mem pak) gives a
Controller Pak, and the runtime pak-switch code is compiled out on SDL ≥ 2.0.18. The default
moved to Rumble Pak with a one-time version-stamped merge for existing configs, and the
in-game Options menu (Controls → Expansion Pak) switches back for anyone who wants Controller
Pak saves. mupen64plus also self-heals — it reverts `PLUGIN_RAW` to `PLUGIN_MEMPAK` when the
pad reports no rumble — so the config change is inert rather than harmful if FF is missing.

### Settings

**Game Rumble** in Controls & Feedback (DB key `rumble_game`, default on) is independent
from the Rumble UI-haptics toggle and uses the strength slider as the intensity ceiling
(the game supplies the variable magnitude). This lets a user keep UI haptics without game
rumble, or vice versa. Read at game launch, so a change takes effect on the next launch.

### Ownership / arbitration

No new arbitration: during a game there is no UI, so the daemon hands duty-writing to RA for
the session and reclaims it (forces off) on exit — still a single-owner PWM.

### Open Phase-2 details (flagged, not solved here)

- ~~**sysfs permission**~~ — resolved: everything runs as root (see 2a).
- ~~**Standalone scope:** decide per-emulator whether the payoff justifies the patch~~ —
  resolved by force feedback on the virtual pad: there is no per-emulator patch to justify.
  The open question shrank to whether Flycast's own sink is still worth keeping now that the
  generic route covers it.

---

## 6. Tuning — MEASURED on Puff (2026-07-24)

Done with Eric holding the device, driving `duty_cycle` from a shell script rather than
rebuilding per candidate. Every ladder anchored each test pulse behind a full-strength
marker buzz, so a step was identified by counting always-felt markers instead of trying to
count things that might not be felt at all.

### The motor's response curve

Perceptibility is **duty x duration**, not either alone — this motor spins up slowly:

| Pulse length | Duty needed to be clearly felt |
| --- | --- |
| 40 ms  | ~75% |
| 70 ms  | ~60% |
| 90 ms  | ~60% |
| 350 ms | ~20-23% |

Two independent ladders (descending duty at fixed length, ascending length at fixed duty)
converged on the same **(60%, 90 ms)** corner, which is the cross-check that makes the rest
trustworthy. The curve is flat from ~70 ms to ~90 ms and only climbs below that.

**Coast-down is fast even though spin-up is slow.** A 60 ms gap already separates a double
burst cleanly at full duty (the worst case), so the 80 ms gap ships unchanged, with margin.

### What that forced

- **The original 40 ms tick was unusable.** At 40 ms nothing under ~75% duty registers, which
  would have pinned the floor at 75% and collapsed the strength slider to a 75-100 range.
  The tick had to grow to **100 ms** for the slider to mean anything.
- **Short and sustained pulses need different floors.** One 40% floor was wrong in both
  directions: too low for a UI tick, too high for game rumble. Hence `JW_RUMBLE_FLOOR` 60%
  for ticks and `JW_RUMBLE_GAME_FLOOR` 25% for sustained game rumble — holding a core's
  weakest effect to the tick floor would have thrown away most of its magnitude range.
- **Nav needed its own tick length.** Catastrophe repeats a held direction every 100 ms
  (`CAT_INPUT_REPEAT_RATE`), so a 100 ms nav tick left zero gap and a held scroll read as one
  unbroken buzz — confirmed by feel and by the constant. Note a rate limit *cannot* fix this
  finely: events only arrive every 100 ms, so dropping them yields 200/300 ms and nothing in
  between. The fix is a shorter nav tick (**70 ms**, tick-per-move preserved, 30 ms gap),
  chosen by A/B against 200 ms cadence and against "first move only, silence while held".
  60% is still the floor at 70 ms, so nav needs no separate floor.

### Shipped values

| Constant | Value |
| --- | --- |
| `JW_RUMBLE_FLOOR` | 60% |
| `JW_RUMBLE_GAME_FLOOR` | 25% |
| `JW_RUMBLE_TICK_MS` | 100 |
| `JW_RUMBLE_NAV_TICK_MS` | 70 |
| `JW_RUMBLE_GAP_MS` | 80 |

Verified after deploy: previews at strength 1 / 50 / 100 wrote duty `400000` / `200000` / `0`
(inversed, so lower is stronger) — exactly 60% / 80% / 100%. Confirmed by feel in the UI, and
still perceptible at the 5% strength setting.

- ~~Phase 2: sysfs write permission for the game process~~ — resolved, everything runs as root.
- Still open: whether the strength slider reads as a sensible ceiling inside an actual game
  (the game floor of 25% has not been feel-checked in a rumbling game, only the math).

## 7. Files

- `cmd/jawakad/main.c` — rumble module: channel init, `jw__rumble_*` worker + event→pattern
  table + duty scaling + safe-stop, force-off on launch/sleep/shutdown, `rumble` IPC action,
  cache the three DB settings, Phase-2 env injection at RA launch.
- `internal/settings/settings.{c,h}` — new **Controls & Feedback** page + rows (Rumble,
  Strength slider, Navigation tick, Game Rumble, migrated Screenshots); new DB keys; page enum
  + top-level list entry.
- `cmd/jawaka-launcher/main.c` (+ menu) — emit named rumble events at select / commit /
  boundary / (optional) nav sites; strength-slider live preview.
- `retroarch-builds/patches/common/0003-sysfs-rumble-fallback.patch` — the `SR_PWM` duty mode,
  wired into `build-mlp1.sh` as the `sysfs-rumble` patch-set entry (branch
  `agent/sysfs-rumble-pwm`, `c09a7e6`).
- `internal/platform/input_proxy_mlp1.c` + `input_proxy.h` — `FF_RUMBLE` on the virtual pad and
  the uinput force-feedback service (upload / erase / play) on its own poll thread, plus the
  `jw_input_rumble_cb` hook the daemon hangs the motor off.
- `N64-standalone` (`config/shared/default.cfg`, `config/shared/overlay_settings.json`,
  `config/mlp1/launch.sh`) — Rumble Pak by default, one-time migration, Options-menu switch
  back to the Controller Pak. No emulator source change.
- Docs (leaf-docs) — still to write, at release time.

Branch `agent/rumble-haptics` (PR #11): `ee1d082` plan, `a8d9e62` motor core, `394b20b`
Controls & Feedback page, `db6e4d5` polarity fix, `8676468` launcher haptics, `47b992c`
settings-screen haptics, `b4c3d73` phase 2a, `7673dd5` measured timings, `8b7d921` +
`1a3489a` pre-existing input bugs, `473add8` gate + UI coverage, `69c3378` deep-suspend
release, `78edc5d` stoppable worker, `33afe3f` call sites + async IPC, `5afc86b` deferred
motor reclaim.

---

## 8. Audit (2026-07-25) - what a review pass found

Five parallel reviewers over the finished branch. Everything below was verified against the
code before fixing; all of it is fixed and on device.

**The motor could still be stranded.** The quiesce added for suspend did not stop the worker:
on timeout it forced the motor off and returned while the worker re-energised it on its next
burst, and suspend froze it that way for the whole sleep - the exact outcome its own comment
claimed to prevent. The worker is now cancellable via a generation counter. Both timed waits
were also on `CLOCK_REALTIME`, and with no battery-backed RTC the first time-sync steps the
clock hours forward and fires every deadline instantly, making that race a certainty rather
than a possibility. Now `CLOCK_MONOTONIC`.

**Four bare force-offs raced the worker** and were usually no-ops, including on daemon exit,
where the detached worker can die mid-tick leaving the motor on with no owner left to clear
it. All now quiesce.

**The in-game menu left the motor running.** Pausing RetroArch stops the core asking for
rumble but does not clear the duty it last set. Worse, `jw_ra_pause_direct` sends `PAUSE` as a
fire-and-forget datagram, so RetroArch can run another frame and re-assert the duty *after*
the reclaim. Fixed with an immediate reclaim plus a deferred one 250 ms later, driven from the
main loop so the latency-sensitive menu open is not blocked.

**The release build omitted the patch entirely** - see Leaf #17. Two separate stale patch-set
defaults, plus no check that an existing binary matches the requested set.

**RetroArch-side:** no validation on the `RUMBLE_PWM_*` values, where a malformed entry became
duty 0, which under inverted polarity is full on; and a write cache that went stale whenever
the daemon touched the node, silently dropping rumble a core had asked for.

**Haptic call sites** claimed outcomes they had not checked, and `jw_ipc_rumble` was never
fire-and-forget despite three comments saying so - every cursor move was a blocking round trip
on the UI thread.

### Two pre-existing bugs it surfaced

Neither is rumble's fault; haptics just made them perceptible.

- `jw_input_proxy_release_buttons` walked `EV_KEY` only, but the D-pad is an ABS hat and the
  stick two more axes, so "release everything" left directions pinned. That half-fixed
  `a9adfb6` (the in-game-menu resume leak).
- Entering standby swallows input, so the launcher never saw a held direction's release and
  auto-repeated it behind a dark screen, still scrolling on wake. Present since jawakad took
  the power key. The deep-suspend path needed the same fix separately, and is the path a
  power tap actually takes.

---

## 9. Next (held as separate work)

- **Phase 2b - standalone emulators.** Flycast / PPSSPP / DraStic / mupen64 each need their
  own patch to write `duty_cycle`. Now covers PSP, DS, N64 **and** Dreamcast, since standalone
  is the default for all four. Decide per emulator whether the payoff justifies it.
- **Variable-magnitude verification.** Only ever proven with a GB rumble cart, which is binary
  on/off, so the 25% game floor and the whole `[MIN,MAX]` lerp are untested against a core
  that sends intermediate magnitudes. Needs a PS1 title with `input_libretro_device_p1` set to
  DualShock.
- **Release-day docs.** leaf-docs has `guide/rumble.md` written and live behind a Soon banner
  plus a sidebar badge in `astro.config.mjs`; both must come off when this ships. Also
  `guide/screenshots.md` still says Settings > General, and that row moves to Controls &
  Feedback - its annotated screenshot needs retaking too.
- **Known residue.** A held direction may still advance the cursor one position across a
  sleep/wake. Suspected to be events already buffered in the launcher's own SDL queue, which
  the daemon cannot reach, so any fix is launcher-side.
