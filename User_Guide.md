# NETrex User Guide

> **Audience.** Neuroscientists and electrophysiologists using NETrex for
> simulations or dynamic-clamp experiments. No C++ required.
>
> For installation and build instructions, see `README.md`.  
> For architecture and internals, see `Developer_Guide.md`.

---

## Table of contents

1. [Quick start](#1-quick-start)
2. [The main window](#2-the-main-window)
3. [Building a network](#3-building-a-network)
4. [Running a pure simulation](#4-running-a-pure-simulation)
5. [Setting up NI-DAQmx hardware](#5-setting-up-ni-daqmx-hardware)
6. [Configuring a biological cell](#6-configuring-a-biological-cell)
7. [Running a dynamic-clamp experiment](#7-running-a-dynamic-clamp-experiment)
8. [Viewing and exporting data](#8-viewing-and-exporting-data)
9. [Keyboard shortcuts](#9-keyboard-shortcuts)
10. [Troubleshooting](#10-troubleshooting)
11. [Rig profiles (portable networks across labs)](#11-rig-profiles-portable-networks-across-labs)

---

## 1. Quick start

1. Launch **NETrex** (see `README.md` for platform-specific install steps).
2. Choose **File → New Network**. A sample 3-cell network appears in the
   network view.
3. Click **▶ Run** on the toolbar. The traces update in real time on the
   right.
4. Click **⏹ Stop** when you've seen enough.

That's the whole app in four steps. The rest of this guide walks through
the parts in more depth.

---

## 2. The main window

```
 ┌────────────────────────────┬──────────────────────────────────────┐
 │                            │                                      │
 │     Network view           │                                      │
 │     (graphical diagram)    │                                      │
 │                            │          Trace panel                 │
 ├────────────────────────────┤      (Vm / current traces)           │
 │                            │                                      │
 │     Network editor         │                                      │
 │     (tree: cells, currents,│                                      │
 │      synapses, electrodes) │                                      │
 │                            │                                      │
 └────────────────────────────┴──────────────────────────────────────┘
```

- **Network view** (top left): graphical picture of cells and synapses.
  Right-click anywhere to add a cell, or right-click between two cells to
  add a synapse.
- **Network editor** (bottom left): tree view. Right-click an item for
  add/remove/edit actions. Double-click to open a parameter editor.
- **Trace panel** (right): one plot per displayed signal. Buttons along
  the left edge switch between *Zoom XY* (⊞), *Zoom X* (↔), *Zoom Y* (↕),
  *Pan* (✋), and *Reset* (⊙).

**Top toolbar:** ▶ Run, ⏸ Pause, ⏹ Stop, ⏭ Step, plus a **duration**
box. These drive the simpler "run the network for N seconds" workflow.
For dynamic clamp, use **Simulate → Run Dynamic Clamp…** instead.

---

## 3. Building a network

### 3.1 The pieces

| Object | What it is |
|---|---|
| **Model Cell** | Integrate-and-fire neuron. You give it a capacitance and a set of ionic currents; NETrex integrates *V<sub>m</sub>* for you. |
| **Vm Playback Cell** | A "virtual" cell whose *V<sub>m</sub>* is read from a file. Useful for reproducibly driving network inputs. |
| **Biological Cell** | A real neuron on your rig, connected via NI-DAQmx. *V<sub>m</sub>* is sampled from the amplifier; currents NETrex computes are injected. **Requires NI-DAQmx hardware.** |
| **HH Current** | Hodgkin–Huxley conductance `I = G_max · m^p · h^q · n^r · (V − E)`. Fully configurable gating. |
| **HH2 Current** | Two-step HH with a second, slower recovery state (for e.g. persistent Na, slow K). |
| **Gap Junction** | Ohmic coupling between two cells. |
| **Generic Bidirectional Synapse** | Simple graded chemical synapse. |
| **Voltage-Clamp PID** | Software voltage clamp implemented as a PID controller on injected current. |
| **Injection Electrode** | Current-clamp pulse generator (amplitude / delay / duration / repeats). |
| **Playback Current** / **Playback Electrode** | Injects a pre-recorded current waveform. |

### 3.2 Adding objects

1. **Right-click** in the network view or editor tree.
2. Choose **Add Cell** (or **Add Electrode**, **Add Current**, etc.).
3. Pick a type and give it a unique name.
4. Double-click the new item to edit its parameters.

### 3.3 Saving and loading

- **File → Save** writes a human-readable `.json` file. Commit these to
  your lab notebook / git repo — they fully describe the network.
- **File → Open** reads a `.json`. **File → Open Recent** is the
  quickest way to resume yesterday's work.

### 3.4 Units (always)

NETrex uses one unit convention everywhere:

| Quantity | Unit |
|---|---|
| Conductance | µS |
| Voltage | mV |
| Current | nA |
| Time | ms |
| Capacitance | nF |

Because µS × mV = nA, Ohm's law stays clean: `I (nA) = G (µS) · (V − E) (mV)`.

---

## 4. Running a pure simulation

When your network contains **no biological cells**, NETrex runs purely
numerically — no hardware needed. This is how you prototype a model
before taking it to the rig.

1. Build or load the network.
2. Set the **duration** in the toolbar (e.g. `5.0` seconds).
3. Click **▶ Run**. The traces animate in real time.
4. **⏹ Stop** resets, **⏸ Pause** freezes without resetting, **⏭ Step**
   advances by one time step.
5. **File → Export Data (CSV)…** saves whatever is currently in the
   plot window as a CSV. **File → Export Plot Image…** saves a PNG or
   SVG of the plots.

Simulation uses the same 4th-order Runge–Kutta integrator as dynamic
clamp (see `Developer_Guide.md` §2.3). The RK4 sub-step cap is set
per-run in the dynamic-clamp dialog, or defaults to 0.01 ms.

---

## 5. Setting up NI-DAQmx hardware

> **macOS note.** NI-DAQmx is not available on macOS. NETrex on Mac is
> simulation-only, with a built-in mock DAQ for testing network
> behavior. Skip to §6 if that's your setup.

### 5.1 Install the driver

**Windows (primary platform for dynamic clamp):**

1. Download the **full NI-DAQmx driver** (not just the Runtime) from
   [ni.com/downloads/drivers/ni-daqmx](https://www.ni.com/en/support/downloads/drivers/download.ni-daq-mx.html).
2. Run the installer. Accept the defaults; this also installs
   **NI Measurement & Automation Explorer (NI MAX)**, which is the
   tool you'll use to configure devices.
3. Reboot if prompted.

**Linux:**

1. Same download page — pick the `.rpm` or `.deb` bundle for your
   distro (RHEL/CentOS, Ubuntu, openSUSE are supported).
2. Install per NI's instructions. The shared library ends up at
   `/usr/lib/x86_64-linux-gnu/libnidaqmx.so`.
3. There is no NI MAX on Linux; instead the command-line tools
   `lsdaq` and `nipkiface` list and configure devices.

### 5.2 Connect your hardware

1. Plug the NI device into the host (USB for USB-series, PCI/PXI
   otherwise).
2. Windows/Linux should auto-detect it. In NI MAX you should see it
   under **My System → Devices and Interfaces**.

### 5.3 Device naming

NI gives each card a default name like `Dev1`, `Dev2`, `PXI1Slot3` and
so on. NETrex **automatically enumerates** every device the driver
reports and shows the real channel names in the biological-cell
dialog, so there's no special name you need to pick. You can rename a
device if you want to — right-click it in NI MAX and choose **Rename**
— but the only reason to do so is human readability (e.g. `PatchRig`
instead of `Dev1`). Multi-card rigs work out of the box: every card's
channels appear in the dropdowns.

If you load a `.json` network that was recorded on a different rig and
some of its channels belong to devices that aren't plugged in, the
dialog shows those channels greyed out with a *"(not present)"* note.
The names are preserved so nothing silently rewrites your file; just
pick a valid channel before running.

### 5.4 Verify the device with NI MAX

Before pointing NETrex at real neurons, confirm the card works:

1. In NI MAX, select your device in the tree.
2. Click **Self-Test**. You should get a green check.
3. Click **Test Panels** → **Analog Input**.
   - Pick `ai0`.
   - Short the `ai0+` and `ai0-` inputs (BNC short, or just touch the
     leads).
   - Click **Start**. The trace should sit near 0 V.
4. **Test Panels → Analog Output**:
   - Pick `ao0`.
   - Set a 1 V DC value and click **Update**. Measure with a DMM —
     should read 1.0 V.

If any of that fails, fix it before moving on. NETrex cannot work
around a miswired or misconfigured card.

### 5.5 Connect the amplifier

The standard dynamic-clamp wiring:

```
                 BNC
  Amplifier   ──────→   NI Dev1/ai0       (reads scaled V_m)
  (V_m OUT)

                 BNC
  Amplifier   ←──────   NI Dev1/ao0       (injects I_inject)
  (EXT CMD IN)
```

- **V<sub>m</sub> output of the amplifier → AI0 of the NI card.** This
  is what NETrex reads as "voltage of the biological cell."
- **AO0 of the NI card → External Command input of the amplifier.**
  This is what NETrex drives to inject current.

Always use **shielded BNC cables** and keep grounds consistent across
the amp, the NI card, and the rig. A bad ground loop will show up as
60 Hz hum on every trace.

---

## 6. Configuring a biological cell

Once the card is wired, add a **Biological Cell** to your network and
double-click it to open the editor.

> **Tip — making this configuration portable.** If you and your
> collaborators want to share the same network across different rigs,
> use a **Rig Profile** (§11) instead of typing channels and gains into
> each biological cell. A role like `"Cell_A"` in your `.json` file
> resolves to different hardware on each rig automatically.

### 6.1 Pick AI and AO channels

Two lists on the left of the dialog: *Analog Input Channels* (where the
amp's V<sub>m</sub> comes in) and *Analog Output Channels* (where
NETrex's computed current goes out). Select one in each list.

### 6.2 Set the gains (the tricky part)

The dialog has a **Switch to Amplifier Gains** button that toggles
between two equivalent ways of entering the same information:

**Network gains** (default view):

| Field | Meaning |
|---|---|
| Gain (cell mV / ADC V) | How many mV of real membrane potential each volt on the ADC represents. Called `AIGain` internally. |
| Gain (DAC V / cell nA) | How many volts NETrex should put out per nA of intended injection current. Called `AOGain` internally. |

**Amplifier gains** (alternate view):

| Field | Meaning |
|---|---|
| Amp Gain (cell mV / Amp mV) | The front-panel gain setting on your amp. |
| Ext. Command Sens. (nA/V) | The external-command sensitivity, usually printed on the amp's front panel. |

The dialog converts between them with `AIGain = 1000 / amp_gain` and
`AOGain = 1 / ext_cmd_sens`. Whichever form is more natural for you is
fine; just be consistent.

### 6.3 Worked example: MultiClamp 700B

The default scaling on a MultiClamp 700B in current-clamp is:

- **α = 100** (primary output): 100 mV at the amp output per 1 mV of
  real V<sub>m</sub>. Equivalently, 10 mV/V on the ADC side.
- **External command input sensitivity**: 2 nA/V (some modes are
  400 pA/V — check your specific configuration).

Entered in the dialog as:

| | Network-gain mode | Amp-gain mode |
|---|---|---|
| AI | **10** (mV/V) | **100** (cell mV / amp mV) |
| AO | **0.5** (V/nA) | **2** (nA/V) |

### 6.4 Worked example: Axopatch 200B

With α = 1 (current-clamp, β=1×):

| | Network-gain mode | Amp-gain mode |
|---|---|---|
| AI | **100** (mV/V) | **10** (cell mV / amp mV) |
| AO | **1** (V/nA) (if CMD SENS = 1 nA/V) | **1** (nA/V) |

At α = 10, AI becomes `1000 / 100 = 10` mV/V.

### 6.5 Current injection limits

Below the gain group:

- **Positive limit (nA)** — NETrex will clamp the injected current at
  this value going positive. Typical: `+5` for most preparations.
- **Negative limit (nA)** — same, going negative. Typical: `-5`.

These are a safety net. If your network model develops a runaway and
asks for 80 nA, the limit will cap it at 5 nA before it reaches the amp.

> **Tip.** Start with tight limits (±2 nA) when testing a new network,
> then widen once you trust it.

---

## 7. Running a dynamic-clamp experiment

> **A note on USB DAQ devices.** NETrex picks an AO timing mode at run
> start, in this order:
> 1. **HW-timed single-point** — sub-microsecond determinism, used on
>    PCIe / PXI X-series cards.
> 2. **Continuous-buffer** — used on most USB devices (USB-6001,
>    USB-6008, USB-6009, USB-6210 family, etc.). Output is still
>    hardware-clocked and jitter-free; loop latency is ~50 ms (the
>    AO buffer depth) by default.
> 3. **On-demand** — last-resort fallback. Each AO write goes over USB
>    one sample at a time. Functional but jittery (~1 ms).
>
> Practical implication: USB rigs work for adding/subtracting
> conductances whose time constants are slower than ~5–10 ms — slow K,
> NMDA (τ ~ 50 ms), GABA-B, h-current, AHP currents. They cannot
> faithfully clamp fast HH sodium (τ_m ~ 0.1 ms): the cell's spike is
> over before the closed loop's correction reaches the amplifier. For
> fast Na/K experiments, use a PCIe rig.
>
> **Low-cost USB cards (USB-6008 / USB-6009).** These devices have
> two limitations worth flagging up front:
>   - **No AO sample clock at all.** Tier 2 (continuous buffer) won't
>     succeed; you'll always land in tier 3 (on-demand), with
>     ~1 ms loop latency. Fine for slow conductances; not for fast Na.
>   - **Unipolar AO (0–5 V), not bipolar (±10 V).** NETrex queries
>     and uses the device's actual range automatically (so you no
>     longer get an "Maximum Value 5.0" error from the driver), but
>     this means you can only output one polarity directly. To inject
>     both depolarizing and hyperpolarizing currents through the same
>     channel, configure your amplifier so its external command input
>     interprets the 2.5 V midpoint of the DAC as zero current — for
>     example, use the amp's holding-current dial to bias the
>     injection by an amount equal to whatever 2.5 V × AOGain
>     produces. Then a 0–5 V DAC swing = ± (2.5 V / AOGain) of
>     bidirectional current.

Menu: **Simulate → Run Dynamic Clamp…** (or press **Ctrl+R**).

The run dialog has three columns.

### 7.1 Left column — parameters

| Group | Controls |
|---|---|
| **Acquisition** | *Sample Rate* (requested; Coerced Rate is shown after the card snaps to its nearest supported rate). *Max RK4 step* caps integrator sub-steps; 0.01 ms is safe for most HH models. |
| **Timing (seconds)** | *Before*: DAQ is reading but no current is injected (baseline). *Duration*: active dynamic clamp. *After*: DAQ reading, current injection off (recovery). |
| **Repeats** | How many times to run the Before/Duration/After sequence. `0` = unlimited. *Reset between runs* re-initializes the network model each repeat. |
| **Interpolation** | When enabled, integrates the network at a finer rate than the DAQ sample rate. Useful when your card can't acquire fast enough to keep HH kinetics stable. |
| **Data** | *Save data to file* triggers a file-save dialog. Output is binary `.ntrx` — see §8.3. |

### 7.2 Middle column — plot selection

- **Cells (mV)** and **Electrodes (nA)** list everything in your
  network that can be plotted. Select items and click **Add to Display
  ▶** to move them into the right-hand *Displayed Plots* list.
- **▲ Up / ▼ Down** reorder. You can display up to **8 traces** at once.

### 7.3 Right column — live traces

Same zoom/pan controls as the main window's trace panel. Plots update
at ~30 Hz regardless of the DAQ sample rate.

### 7.4 Starting a run

Click **Start**. You should see:

- The **Coerced Rate** label fill in (this is what the DAQ actually runs
  at).
- The progress bar advance.
- The status label cycle through `DAQ [Before]` → `DAQ [During]` →
  `DAQ [After]`.
- The stats block update continuously with **Scans**, **Chunks**,
  **UI overflows**, **Disk rejects**, wall-clock **Wall** time, and the
  **RT** priority status (`Ok` / `NotSupported` / `PermissionDenied`).

**Interpreting the stats:**

- **UI overflows > 0** — the UI was stalled briefly; the plot skipped
  some samples to catch up. The **disk file is still complete**. Happens
  occasionally on macOS during Mission Control or Spotlight; nothing to
  worry about.
- **Disk rejects > 0** — the disk couldn't keep up and the run was
  **aborted** to prevent data loss. Check disk free space, anti-virus
  scanning, or move to a faster drive. This should essentially never
  happen on a local SSD.
- **RT: NotSupported** (macOS) — the kernel won't honor real-time
  priority. Fine for development and simulation; don't publish
  dynamic-clamp results from a Mac.
- **RT: PermissionDenied** (Linux) — you need `CAP_SYS_NICE`. See the
  Linux real-time section of `Developer_Guide.md` §4.6.

### 7.5 Stopping

- **Stop** button — cleanly ends the run, zeroes the DAC, and closes
  the save file.
- **Close** dismisses the dialog (only while not running).

NETrex always drives the DAC to zero on stop, so the amp won't keep
injecting a stale command after your run ends.

---

## 8. Viewing and exporting data

### 8.1 Interacting with plots

- **Zoom XY (⊞)** — drag a rectangle.
- **Zoom X (↔)** — drag horizontally; the time axis reshapes, Y stays.
- **Zoom Y (↕)** — drag vertically; the value axis reshapes.
- **Pan (✋)** — drag to scroll.
- **Reset (⊙)** or **double-click** a plot — fit all data.

### 8.2 Exporting what's on screen

- **File → Export Data (CSV)…** writes the currently-visible plot
  contents as CSV.
- **File → Export Plot Image…** writes a high-resolution PNG or a
  vector SVG suitable for figures.

### 8.3 The `.ntrx` file format

A dynamic-clamp run with *Save data* enabled produces a binary `.ntrx`
file. This file contains **every AI sample at the DAQ sample rate**,
plus the network's end-of-chunk V<sub>m</sub> and I<sub>nA</sub> values
at the same resolution. It's complete enough to reproduce the run offline.

You have three ways to read an `.ntrx` file:

**1. From within NETrex (easiest):**

**File → Convert .ntrx to CSV…**

- Pick the `.ntrx` you recorded.
- Pick an output `.csv` path.
- A progress bar runs while it transcodes.

The output CSV has a `Time (s)` column plus one column per channel,
with exact channel names pulled from the file.

**2. Command line:**

```
ntrx2csv /path/to/run.ntrx /path/to/run.csv
```

If you omit the second argument, it writes `run.ntrx.csv`. Useful in
batch scripts:

```bash
for f in *.ntrx; do ntrx2csv "$f"; done
```

**3. Any programming language:**

The binary format is documented in `core/io/scan_writer.h`. Short
version: 256-byte little-endian header, then `num_channels × 64` bytes
of NUL-padded channel names, then row-major `float64` scans. Python:

```python
import numpy as np, struct
with open("run.ntrx", "rb") as f:
    hdr = f.read(256)
    magic, ver = struct.unpack_from("<II", hdr, 0)
    rate = struct.unpack_from("<d", hdr, 8)[0]
    scan_size, n_ch = struct.unpack_from("<II", hdr, 16)
    n_scans = struct.unpack_from("<Q", hdr, 32)[0]
    data_off = struct.unpack_from("<Q", hdr, 40)[0]
    names = [f.read(64).rstrip(b"\0").decode() for _ in range(n_ch)]
    f.seek(data_off)
    data = np.fromfile(f, dtype="<f8", count=n_scans * scan_size)
data = data.reshape(n_scans, scan_size)
t = np.arange(n_scans) / rate
```

### 8.4 If your computer crashed mid-run

The `.ntrx` file is still usable. NETrex patches the final scan count
into the header only on a clean stop; if it didn't get a chance to, the
count is computed from file size instead. Nothing that was flushed to
disk is lost. This recovery happens automatically when you open the
file; no action required.

---

## 9. Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+N | New Network |
| Ctrl+O | Open… |
| Ctrl+S | Save |
| Ctrl+D | Export Data (CSV) |
| Ctrl+Z / Ctrl+Shift+Z | Undo / Redo |
| Ctrl+R | Run Dynamic Clamp… |
| Ctrl+Q / Cmd+Q | Exit |
| Double-click a plot | Reset its zoom |

---

## 10. Troubleshooting

### "NI-DAQmx not detected" at startup

- The runtime isn't installed, or it's installed but the system
  hasn't been rebooted since.
- On Windows, open **NI MAX → My System → Software** and confirm
  `NI-DAQmx Runtime` appears. On Linux, run `ls /usr/lib/x86_64-linux-gnu/libnidaqmx.so`.
- If NI MAX sees the card but NETrex doesn't, the biological-cell
  dialog still lists `Dev1/aiN` channels — you can still configure
  and run. The actual DAQ calls will fail at run-time; the error
  dialog will tell you which one.

### "Coerced rate" is different from what I asked for

NI cards round the requested rate to the nearest supported value
(they have specific master-clock divisors). This is normal. NETrex
shows you the coerced rate prominently so you know what's actually
being used. Typical: ask for 50 000 Hz, get 50 000.000 Hz; ask for
10 007 Hz, get 10 000 Hz.

### Traces are full of 60 Hz (50 Hz) hum

Ground-loop issue, not a NETrex problem. Common fixes:

- Use a single ground point (usually the amplifier).
- Don't plug the rig's AC adapter into a different outlet than the
  host PC.
- Shield the BNC cables.
- Increase the amp's notch filter.

### My `.ntrx` file is only 256 + N×64 bytes

That's header-plus-channel-names and nothing else — the run started
but no chunks were submitted. Either *Save data* was checked but the
DAQ thread failed before writing (look at `Disk rejects` in the
stats), or the recording was only a few milliseconds long.

### `Disk rejects > 0` every run

The disk queue is overflowing. Order of likelihood:

1. Antivirus / Windows Defender real-time scanning is touching
   the file as it's being written — add an exclusion for the save
   folder.
2. You're saving to a network drive or an SD card that can't
   sustain the write rate. Save locally to an SSD and copy off
   afterwards.
3. Disk is almost full — `fwrite` falls back to slow paths.
4. Some other process (backup software, cloud sync) is fighting you
   for the disk.

### `UI overflows > 0` but `Disk rejects = 0`

The experiment worked and the file is complete. The UI plot skipped
some samples to catch up after a display-side pause. Ignore unless
it's happening constantly; then check for CPU-hog background
processes.

### My amp output looks like it's saturating

NETrex clamps AO output at the hardware range (**±10 V** on NI
cards). If your gain math says you need >10 V to inject a computed
current, the amp will see the clamped value instead. Either increase
the amp's external command sensitivity (fewer nA/V) or reduce the
model conductances.

### Running on macOS: how accurate is the "dynamic clamp" mock?

The mock backend internally simulates a single integrate-and-fire
cell with an HH sodium current. It lets you test that your network
integrates correctly, that the Before/During/After phases fire, that
plot/save pipelines work, and that the amp-gain math is right — all
without hardware. But **it will not give you physiologically real
results** because both sides of the dynamic-clamp loop are in the
same software. Take the model to a Windows or Linux rig for actual
experiments.

---

If you hit something not covered here, check `Developer_Guide.md` for
the internals or open an issue on the GitHub repo.

---

## 11. Rig profiles (portable networks across labs)

A network `.json` describes **the science** — cells, currents, synapses,
gating parameters — everything that is true of the model regardless of
where it runs. A **rig profile** (`.rigprofile.json`) describes **one
bench's hardware** — which DAQ channels are wired to which amp, what
gains those channels use, what current limits are safe.

Keeping the two separate means you can share your `.json` file with a
colleague and they can load it into their rig without rewiring
anything. They just pick their own rig profile; the biological cells
resolve automatically.

### 11.1 The role-based workflow, in short

1. Define a rig profile once per bench, with role names like
   `Cell_A`, `Cell_B`, `Presynaptic`, etc.
2. In any network file, each biological cell carries a **Role** instead
   of a device/channel.
3. On load, NETrex looks up the active rig profile and binds
   AI channel, AO channel, AI gain, AO gain, and current limits from
   the profile's entry for that role.
4. Move the `.json` to a different rig → activate that rig's profile →
   the same network runs with the new hardware bindings.

### 11.2 Creating a rig profile

**Settings → Rig Profiles…** opens the manager.

- **New** — prompts for a profile name (e.g. `"Room207_Patch"`).
- **Name / Description** — free-form; description is for your notes,
  like "Axopatch 200B, NI USB-6343, whole-cell clamp rig".
- **Role bindings** table — each row is one role on this rig:

  | Column | Meaning |
  |---|---|
  | Role | Symbolic name your network files reference (e.g. `Cell_A`). |
  | AI Channel | The NI-DAQmx channel your amp's V<sub>m</sub> output is plugged into (e.g. `Dev1/ai0`). |
  | AO Channel | The NI-DAQmx channel that drives your amp's External Command input (e.g. `Dev1/ao0`). |
  | AI Gain (mV/V) | Same as the `AIGain` field in the per-cell dialog — see §6.2. |
  | AO Gain (V/nA) | Same as the `AOGain` field. |
  | I+ / I− limit (nA) | Clamp bounds on injected current for safety. |

- **+ Add binding** adds a row. Role names must be unique within one
  profile.
- **Save** writes to disk.
- **Set as Active Profile** makes this profile the one NETrex uses
  when resolving roles. The active profile is remembered across app
  restarts.

Profile files live under your user config directory:

- **macOS:** `~/Library/Preferences/NETrex/rig_profiles/*.rigprofile.json`
- **Linux:** `~/.config/NETrex/rig_profiles/*.rigprofile.json`
- **Windows:** `%APPDATA%\NETrex\rig_profiles\*.rigprofile.json`

### 11.3 Assigning a role to a biological cell

In the **Biological Cell** editor, the **Role** dropdown at the top
lists every role defined in the active profile. Picking one means "at
run time, use this rig profile's binding for this role." You don't
need to manually enter channels or gains once a role is assigned —
those fields in the dialog are still visible for reference but the
profile wins.

If no profile is active, the dropdown shows *"— none —"* and you fill
in channels and gains the old-fashioned way; the values are saved in
the `.json`.

### 11.4 What happens on load

The resolution rules, in order:

1. If the cell has a **Role** and that role exists in the **active**
   rig profile → the profile's channels, gains, and limits are used,
   overriding anything in the file.
2. If the cell has a **Role** but it isn't in the active profile (or
   no profile is active) → the values stored in the `.json` are used
   and a warning is shown. The role name is preserved so you can fix
   the profile and reload.
3. If the cell has **no role** → the stored values are used directly,
   exactly like the pre-rig-profile behavior. Existing files still
   work.

### 11.5 Sharing a network across labs — example

Lab A ("Trexler lab"):

- Builds network `cpg_3cell.json` with three biological cells:
  `Cell_A`, `Cell_B`, `Cell_C`.
- Has a rig profile `Room207_Patch` mapping those roles to
  `Dev1/ai0..2`, `Dev1/ao0..2`, amp-specific gains.

Lab B ("Stanford lab"):

- Receives `cpg_3cell.json`.
- Creates their own rig profile `Stanford_Rig` with the same three
  role names but mapping them to `PXI1Slot3/ai4..6`, different gains.
- **Settings → Rig Profiles → Set as Active.**
- Opens `cpg_3cell.json` — biological cells bind to Stanford's
  hardware automatically. No hand-editing.

The science is portable. The wiring is local.

### 11.6 Troubleshooting

**"No active rig profile" in the biological cell dialog.**
You haven't set an active profile. Either create one and click *Set
as Active*, or enter channels/gains directly in the dialog. Both
work.

**Cells keep reverting to my old channels.**
That means your cells don't have a role assigned, so the loader uses
the stored values. Open each biological cell dialog, pick a role,
and save the network.

**A role in my file isn't in the active profile.**
The loader keeps the role on the cell and falls back to the stored
channel/gain values so the network still runs. Go to **Settings →
Rig Profiles**, add the missing role, *Set as Active*, and reload
the network.

**I want to rename my rig.**
Open the profile in **Settings → Rig Profiles**, edit the Name
field, and *Save*. The on-disk filename updates automatically. If
it was the active profile, NETrex follows the rename.
