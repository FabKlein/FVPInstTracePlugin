# InstProfiler — FVP MTI Plugin

Generates a [Chrome Tracing / Perfetto](https://ui.perfetto.dev) JSON profile from an Arm Fast Models (FVP) simulation in real time, with no intermediate tarmac file.  Optionally also produces a per-function **code coverage** JSON and a per-function **performance statistics** CSV.

## How it works

```
FVP simulation
   └── MTI INST source  (fires once per retired instruction)
           │  PC + INST_COUNT
           ▼
    InstProfiler::TracePC()
           │
           ├─ [gating]  start/stop conditions evaluated first
           │
           ├─ SymbolTable::FindSymbol(pc)   ← last-hit cache → O(1) common case
           │       loaded from ELF or nm(1) output at startup    O(log N) fallback
           │
           ├─ call-stack heuristic
           │       PC enters new symbol  → push StackFrame
           │       PC returns to caller  → pop + emit "X" event(s)
           │
           ├─ [coverage]  record unique PC values per symbol
           │
           └─ [JSON]  JsonWriter::WriteCompleteEvent()
                   streaming write to output JSON (never buffered in RAM)
                   skipped entirely if output-file is empty
```

### Call-stack inference

The plugin does **not** instrument the binary.  Instead it watches the retired PC every instruction and infers call/return boundaries:

- When the PC moves into a symbol that is **not on the current stack**, a new `StackFrame` is pushed (treated as a call).
- When the PC moves into a symbol that **is already on the stack**, all frames above it are popped and emitted as Chrome Tracing `"ph":"X"` (complete) events (treated as a return chain).
- When the PC stays within the current top-of-stack symbol, nothing happens — this is the hot path.

This heuristic handles standard C/C++ calling conventions correctly.  It may miss tail calls and `longjmp`.

### capture-function and the silent pre-capture stack

When `capture-function` is set the plugin silently maintains the call stack **before** tracing starts, tracking every function entry and return with `emitting=false` frames.  This means that when the capture function eventually returns, the parent caller is already present in the stack and the return is detected correctly.  Without this, the first call to the capture function would appear to last until the end of the simulation because the return-to-caller event would never fire.

### Output options

| Output | Enabled by | Format |
|---|---|---|
| Chrome Tracing / Perfetto JSON | `output-file=<path>` (non-empty) | Chrome Tracing `"ph":"X"` events |
| Per-function coverage | `coverage-file=<path>` | JSON array, sorted by covered bytes |
| Per-function statistics | `stats-file=<path>` | CSV, sorted by `self_sum_us` |

All three are independent and can be used in any combination.  Setting none of `output-file`, `coverage-file`, or `stats-file` is valid — the plugin still attaches and runs but produces no output files.

The Chrome Tracing JSON is written incrementally to disk.  The file is valid even if the simulation is interrupted (Ctrl-C) because the SIGINT handler flushes all open call-stack frames and writes the closing `]` before the process exits.

Open the JSON result in:
- [Perfetto UI](https://ui.perfetto.dev) — drag & drop the `.json` file
- Chrome / Chromium — navigate to `chrome://tracing`, click **Load**

## Building

### Prerequisites

| Requirement | Notes |
|---|---|
| GCC ≥ 7 or Clang ≥ 6 | C++14 support required (`-std=c++14`) |
| `<cxxabi.h>` | Part of `libstdc++` / `libc++`; needed for optional C++ demangling |
| GNU `make` | Standard `make` or `gmake` |
| Fast Models Portfolio | Provides the MTI/ESLAPI headers under `include/fmruntime/` |

### Steps

```bash
# 1. Point PVLIB_HOME at the root of the Fast Models Portfolio installation.
export PVLIB_HOME=/path/to/FastModelsPortfolio_11.27

# 2. Build — produces InstProfiler.so in the same directory.
cd examples/MTI/InstProfiler
make

# 3. (Optional) clean intermediate object files.
make clean
```

### What the Makefile does

- Compiles `InstProfiler.cpp`, `JsonWriter.cpp`, and `SymbolTable.cpp` independently with `-std=c++14 -Wall -Wextra -O2 -fPIC`.
- Links them into a single shared library: `g++ -pthread -ldl -lrt -shared -o InstProfiler.so *.o`.
- Any `.h` change triggers a full recompile of all `.cpp` files.

### Choosing the right `nm` tool

The plugin calls `nm` at runtime to extract symbols from an ELF binary.  If your binary targets a bare-metal Arm core, the host `nm` will not understand it — pass the correct cross tool via the `nm-tool` plugin parameter:

```bash
-C TRACE.InstProfiler.nm-tool=arm-none-eabi-nm     # Cortex-M / bare-metal
-C TRACE.InstProfiler.nm-tool=aarch64-linux-gnu-nm  # AArch64 Linux
```

Alternatively, pre-generate the symbol file on the host and pass it directly:

```bash
arm-none-eabi-nm --print-size my_app.axf > my_app.sym
-C TRACE.InstProfiler.symbol-file=my_app.sym
```

> **Do not use `--size-sort`**: that flag silently omits functions whose `st_size` field is 0 in the ELF symbol table (common for assembly startup code). The plugin sorts symbols internally.

## Loading the plugin

Plugin parameters are passed via `-C TRACE.<PluginType>.<param>=<value>`, the same mechanism used for all FVP component configuration:

```bash
FVP_Corstone_SSE-300_Ethos-U55 \
    --application my_app.axf \
    --plugin /path/to/InstProfiler.so \
    -C TRACE.InstProfiler.symbol-file=my_app.axf \
    -C TRACE.InstProfiler.output-file=trace.json \
    -C TRACE.InstProfiler.demangle=1 \
    -C TRACE.InstProfiler.tid=1
```

> **Note:** The plugin cannot read the binary already loaded by the FVP — you must supply it explicitly via `symbol-file`.

## Parameters

All parameters are passed as strings via `-C TRACE.InstProfiler.<name>=<value>`.

### Core parameters

| Parameter | Default | Description |
|---|---|---|
| `symbol-file` | `debug.sym` | Path to an ELF binary or `nm(1)` text output.  If the file starts with the ELF magic bytes, the plugin runs `nm-tool` on it automatically. |
| `output-file` | `""` | Destination path for the Chrome Tracing JSON.  **Empty by default** — no JSON file is written unless this parameter is set.  Useful when you only need coverage or statistics output. |
| `demangle` | `0` | Set to `1` to demangle C++ symbol names using `abi::__cxa_demangle`.  Results are cached so there is no per-instruction overhead after the first call. |
| `max-name-len` | `128` | Maximum length (characters) of a demangled C++ name used in all output files.  If a demangled name exceeds this limit the original **mangled** name is used instead (no truncation).  Set to `0` for unlimited. |
| `tid` | `1` | Thread ID written into every event.  Use different values when attaching to multiple CPUs. |
| `pid` | `1` | Process ID written into every event. |
| `nm-tool` | `nm` | `nm` executable used when `symbol-file` is an ELF binary.  Set to e.g. `arm-none-eabi-nm` for bare-metal binaries. |
| `time-scale` | `1.0` | Divide the raw `INST_COUNT` value by this number to produce the timestamp in microseconds shown in the profiler UI.  Example: `1000` if one `INST_COUNT` unit represents approximately 1 ns. |

### Start/stop gating parameters

By default tracing begins at the very first instruction and runs until the simulation ends.  Use the parameters below to record only a window of interest.  **Any** start condition activates tracing; **any** stop condition ends it — whichever fires first wins.

| Parameter | Default | Description |
|---|---|---|
| `start-pc` | `""` | Hex address (e.g. `0x8000`) at which to begin recording.  Fires when the CPU retires an instruction at exactly this address. |
| `stop-pc` | `""` | Hex address at which to stop recording.  The trace is finalised when this PC is first retired. |
| `start-symbol` | `""` | **Mangled** symbol name (as printed by `nm`) at which to begin recording.  Fires when execution first enters this symbol. |
| `stop-symbol` | `""` | **Mangled** symbol name at which to stop recording.  Fires when execution enters this symbol. |
| `start-count` | `0` | `INST_COUNT` value at which to begin recording (`0` = disabled). |
| `stop-count` | `0` | `INST_COUNT` value at which to stop recording (`0` = disabled). |
| `capture-function` | `""` | **Mangled** symbol name of a single function to capture in isolation. Tracing starts on first entry, stops automatically when the function returns. All callees are included. Simpler alternative to `start-symbol` + `stop-symbol`. |

> **Mangled vs demangled names:** `start-symbol`, `stop-symbol`, and `capture-function` always take the **mangled** name as output by `nm` without `--demangle`.  Use `nm --print-size my_app.axf | grep my_function` to find it.

### Analysis output parameters

| Parameter | Default | Description |
|---|---|---|
| `coverage-file` | `""` | If set, write a per-function code coverage JSON to this path at the end of the run.  Each entry records unique retired PC values and an estimated byte count.  Sorted by `covered_bytes` descending.  Empty = disabled. |
| `stats-file` | `""` | If set, write a per-function performance statistics CSV to this path.  Columns: `name`, `count`, `wall_sum_us`, `self_sum_us`, `wall_avg_us`, `self_avg_us`.  `self_sum_us` excludes time in callees.  Sorted by `self_sum_us` descending.  Empty = disabled. |

Both outputs are independent of `output-file` — you can produce coverage and/or statistics without writing any JSON trace.

## Usage examples

### Full trace with demangling

```bash
FVP_Corstone_SSE-300_Ethos-U55 \
    --application my_app.axf \
    --plugin InstProfiler.so \
    -C TRACE.InstProfiler.symbol-file=my_app.axf \
    -C TRACE.InstProfiler.nm-tool=arm-none-eabi-nm \
    -C TRACE.InstProfiler.output-file=trace.json \
    -C TRACE.InstProfiler.demangle=1
```

### Coverage and statistics only (no JSON trace)

Omit `output-file` (or leave it empty) to skip JSON output entirely — useful when only the analysis outputs are needed:

```bash
FVP_Corstone_SSE-300_Ethos-U55 \
    --application my_app.axf \
    --plugin InstProfiler.so \
    -C TRACE.InstProfiler.symbol-file=my_app.axf \
    -C TRACE.InstProfiler.nm-tool=arm-none-eabi-nm \
    -C TRACE.InstProfiler.demangle=1 \
    -C TRACE.InstProfiler.coverage-file=coverage.json \
    -C TRACE.InstProfiler.stats-file=stats.csv
```

### Capture one function and all its callees

```bash
FVP_Corstone_SSE-300_Ethos-U55 \
    --application my_app.axf \
    --plugin InstProfiler.so \
    -C TRACE.InstProfiler.symbol-file=my_app.axf \
    -C TRACE.InstProfiler.nm-tool=arm-none-eabi-nm \
    -C TRACE.InstProfiler.output-file=trace.json \
    -C TRACE.InstProfiler.demangle=1 \
    -C 'TRACE.InstProfiler.capture-function=_ZN10executorch7runtime6Method19execute_instructionEv'
```

The FVP runs at full speed until the function is first entered, records everything inside it, then finalises the trace automatically when it returns.  The demangled name appears as the root span in Perfetto.

### Trace only one function by address window

```bash
# Find the mangled name first:
arm-none-eabi-nm --print-size my_app.axf | grep run_inference
# e.g.: 00008120 000003a0 T _ZN9Inference3runEv

FVP_Corstone_SSE-300_Ethos-U55 \
    --application my_app.axf \
    --plugin InstProfiler.so \
    -C TRACE.InstProfiler.symbol-file=my_app.axf \
    -C TRACE.InstProfiler.nm-tool=arm-none-eabi-nm \
    -C TRACE.InstProfiler.start-symbol=_ZN9Inference3runEv \
    -C TRACE.InstProfiler.stop-symbol=_ZN9Inference3runEv \
    -C TRACE.InstProfiler.demangle=1 \
    -C TRACE.InstProfiler.output-file=run_inference.json
```

> Using the same symbol for both `start-symbol` and `stop-symbol` records the **first call** to that function and stops when it returns (i.e. when the callee pops back to the caller and re-enters the same symbol from the call stack).

### Trace by instruction window

```bash
-C TRACE.InstProfiler.start-count=1000000 \
-C TRACE.InstProfiler.stop-count=5000000
```

### Simple example folder: half-precision real FFT (Cortex-M55)

A minimal end-to-end example is available under `examples/rfft512_f16`.
It builds and runs a CMSIS-DSP 512-point real FFT in `float16`, then prints the complex sum of FFT bins.

From the example directory, run FVP with InstProfiler and capture `main`:

```bash
cd examples/rfft512_f16

FVP_Corstone_SSE-300_Ethos-U55 \
    -C mps3_board.visualisation.disable-visualisation=1 \
    -C mps3_board.telnetterminal0.start_telnet=0 \
    -C mps3_board.uart0.out_file=- \
    -C cpu0.semihosting-enable=1 \
    -C cpu0.semihosting-stack_base=0 \
    -C cpu0.semihosting-heap_limit=0 \
    --timelimit 60 \
    -a out/rfft512_f16/FVP/Release/rfft512_f16.axf \
    --plugin ../../InstProfiler.so \
    -C TRACE.InstProfiler.symbol-file=out/rfft512_f16/FVP/Release/rfft512_f16.axf \
    -C TRACE.InstProfiler.nm-tool=arm-none-eabi-nm \
    -C TRACE.InstProfiler.capture-function=main \
    -C TRACE.InstProfiler.output-file=trace.json \
    -C TRACE.InstProfiler.coverage-file=coverage.json \
    -C TRACE.InstProfiler.stats-file=stats.csv \
    -C TRACE.InstProfiler.demangle=1 \
    -C TRACE.InstProfiler.tid=1
```

Expected generated files in `examples/rfft512_f16`:
- `trace.json` (Chrome Tracing / Perfetto JSON)
- `coverage.json` (per-function coverage)
- `stats.csv` (per-function wall/self statistics)

### Optional offline HTML conversion (Catapult trace2html)

If you cannot upload traces to an online viewer (for example due to privacy concerns),
you can convert `trace.json` to a local HTML report using Catapult:

```bash
git clone https://chromium.googlesource.com/catapult </path_to/catapult>

/path_to/catapult/tracing/bin/trace2html trace.json --output trace2.html
```

Then open `trace.html` locally in a browser.

### Alternative workflow: FVP Tarmac plugin + conversion script

An alternative way to generate a Perfetto/Chrome timeline is to use the standard FVP Tarmac plugin,
produce a Tarmac trace file, and then convert that file into Chrome Tracing format.

- FVP Tarmac plugin documentation:
    https://developer.arm.com/documentation/100964/1131/Plug-ins-for-Fast-Models/TarmacTrace?lang=en
- Example conversion script:
    https://github.com/Arm-Examples/Helium-Optimization/blob/main/Performance_analysis/tools/arm_tarmac_2_chrometracing.py

This approach is valid, but it usually has higher overhead than direct MTI tracing with InstProfiler:

- It significantly slows down FVP simulation time.
- It is a two-step workflow (generate Tarmac first, then post-process).
- The intermediate Tarmac file can become massive very quickly (several Gbs)
- Python post-processing time increases with Tarmac file size.

## Output file formats

### Chrome Tracing JSON (`output-file`)

A standard [Chrome Tracing JSON](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU) array of `"ph":"X"` (complete event) objects, written incrementally.  Load in [Perfetto UI](https://ui.perfetto.dev) or `chrome://tracing`.

### Coverage JSON (`coverage-file`)

A JSON array, one object per visited function, sorted by `covered_bytes` descending:

```json
[
  {"name":"arm_convolve_f16","mangled":"arm_convolve_f16",
   "addr":"0x8120","range_bytes":928,
   "unique_pcs":231,"covered_bytes":924,"coverage_pct":99.6},
  ...
]
```

Covered-byte estimation uses the delta between adjacent retired PC values:
- delta = 2 → Thumb-16 (2 bytes)
- delta = 4 → Thumb-32 or ARM (4 bytes)
- delta > 4 → assume 4 bytes (skipped PCs = not covered)
- last PC in set → counted as 4 bytes

`coverage_pct` is clamped to 100.0.

### Statistics CSV (`stats-file`)

One row per called function, sorted by `self_sum_us` descending:

```
name,count,wall_sum_us,self_sum_us,wall_avg_us,self_avg_us
"arm_convolve_f16",1024,18432.000,14208.000,18.000,13.875
...
```

- `wall_sum_us` — total time spent inside the function including callees.
- `self_sum_us` — total time excluding time spent in direct callees (like Python `cProfile` `tottime`).
- Timestamps are in the same microsecond units as the JSON trace.

## Symbol file format

Two `nm(1)` output formats are accepted:

**With size** (`nm --print-size`):
```
00008000 00000120 T main
00008120 000003a0 T arm_convolve_f16
```

**Without size** (plain `nm`):
```
00008000 T main
00008120 T arm_convolve_f16
```

When sizes are absent the plugin infers the end of each symbol from the start of the next one.  Using `--print-size` is therefore recommended for more accurate range boundaries.

Only code symbol types are loaded: `T`, `t` (global/local text), `W`, `w` (weak).  ARM mapping symbols (`$a`, `$t`, `$d`, `$x`) are automatically skipped.

Thumb bit (bit 0) is cleared on all addresses so that AArch32/Thumb symbols match the PC values reported by MTI.

## Source layout

| File | Responsibility |
|---|---|
| `InstProfiler.h/.cpp` | MTI plugin lifecycle, parameter handling, call-stack tracking, start/stop gating, coverage accumulation, statistics accumulation |
| `JsonWriter.h/.cpp` | Streaming Chrome Tracing JSON file writer; signal-safe emergency close |
| `SymbolTable.h/.cpp` | Symbol loading (ELF auto-detection, nm parsing), last-hit cache + binary-search lookup |
| `Makefile` | Build rules |

## Limitations

- **Tail calls** and **`longjmp`** are not detected; they appear as incorrect nesting in the trace.
- **Multiple cores**: instantiate the plugin once per CPU with a different `tid` value.
- **C++ templates / inlined functions** do not appear unless the linker emits a symbol for them.
- The plugin attaches to **all** INST-capable components it finds.  Use `start-symbol` / `stop-symbol` to limit the recorded scope if needed.
