# GILA Compressor

A file/folder compressor with an extreme compression ratio, built on **context mixing** (the PAQ/lpaq family).
It is slow on purpose, trading time for the smallest possible archive. Written in C++17, with a single source
file (`gila.cpp`) that builds two programs: `gila-kompres` (compress) and `gila-dekompres` (decompress).

Archive format: `.gila`

## Table of contents

- [Features](#features)
- [Repository contents](#repository-contents)
- [Usage](#usage)
- [Memory level (`-m`)](#memory-level--m)
- [Performance and when to use it](#performance-and-when-to-use-it)
- [Building from source](#building-from-source)
- [How it works](#how-it-works)
- [The `.gila` archive format](#the-gila-archive-format)
- [Safety and data integrity](#safety-and-data-integrity)
- [Exit codes](#exit-codes)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)

## Features

- Very high compression ratio for text, CSV, logs, source code, databases, executables, and raw data.
- Supports **files and folders** (subfolder structure is preserved) and multiple inputs at once.
- **Drag & drop** on Windows: drop a file/folder onto `gila-kompres.exe`, or a `.gila` onto `gila-dekompres.exe`.
- Automatic **CRC32 verification** on decompression: the output is guaranteed identical to the original, or you are told it is not.
- Unicode (UTF-8) file names on Windows.
- Dedicated executable filter (E8/E9) for `.exe`, `.dll`, ELF.
- Files inside a folder are ordered by extension so similar data sits together (better ratio).
- **Deterministic, identical results on every PC**: all model arithmetic is integer-only, with no floating point.
- Progress display with speed and estimated time remaining.

## Repository contents

| File | Description |
|------|-------------|
| `gila.cpp` | Complete source (C++17), one file for both compressor and decompressor |
| `gila-kompres.exe` | Compressor (Windows, prebuilt) |
| `gila-dekompres.exe` | Decompressor (Windows, prebuilt) |
| `READ-ME.txt` | Short plain-text usage summary |
| `README.md` | This documentation |

> **Note:** the prebuilt `.exe` files were built before the source was translated to English, so their on-screen
> messages are still in Indonesian. Rebuild from `gila.cpp` to get English messages. The archive format is unchanged,
> so old and new builds read each other's archives.

## Usage

### Drag & drop (easiest, Windows)

| Goal | Steps | Result |
|------|-------|--------|
| Compress | Drop a file or folder onto `gila-kompres.exe` | `<name>.gila` next to the original |
| Decompress | Drop a `.gila` file onto `gila-dekompres.exe` | Extracted to a same-named folder next to the archive |

If the target name already exists, nothing is overwritten; a new name is chosen instead, e.g. `data (2).gila` or folder `data (2)`.
When started by drag & drop (the program owns its console window), the window waits for Enter before closing so you can read the result.

### Command line

```
gila-kompres  [-m1..-m11] [-o archive.gila] file_or_folder [file_or_folder ...]
gila-dekompres archive.gila [target_folder]
```

Examples:

```
:: compress a folder with the default level (-m8)
gila-kompres C:\data\reports

:: compress several inputs at once, level 10, explicit output name
gila-kompres -m10 -o backup.gila C:\data\log.txt C:\data\database

:: decompress into a specific folder
gila-dekompres backup.gila D:\restored

:: -o also works for decompression
gila-dekompres backup.gila -o D:\restored
```

Options:

| Option | Program | Meaning |
|--------|---------|---------|
| `-m1` ... `-m11` | compress | Memory level, default `-m8` |
| `-o <path>` | compress | Output archive name/location. Default: `<first input name>.gila` next to the input |
| `-o <folder>` or second argument | decompress | Extraction folder. Default: a folder named after the archive |

With no arguments, both programs print a short usage message.

## Memory level (`-m`)

The level sets the size of the model's hash tables. Higher means more memory and a better ratio **on large data**.

| Level | Approx. RAM | Notes |
|-------|-------------|-------|
| `-m6` | ~0.2 GB | Safe for low-RAM machines |
| `-m8` | ~0.7 GB | **Default** |
| `-m10` | ~2.3 GB | Most useful for large data (> 100 MB) |
| `-m11` | ~4.3 GB | Maximum |

Memory use, from the source, with level $L$ and $L_b = \min(L, 9)$:

$$
\text{RAM} \approx \underbrace{64 \cdot 2^{15+L}}_{\text{hash table}} + \underbrace{2^{19+L_b}}_{\text{history buffer}} + \underbrace{2^{16+L_b}}_{\text{match table}} + \underbrace{45\ \text{MB}}_{\text{other tables}} \ \text{bytes}
$$

For example, $L = 8$ gives $512\ \text{MB} + 128\ \text{MB} + 16\ \text{MB} + 45\ \text{MB} \approx 0.7\ \text{GB}$.

> **Important:** decompression needs **as much RAM as the level used for compression**. The level is stored in the archive header, so you do not pass it again, but the target machine must have enough memory.

A higher level does not help small inputs; bigger tables only add initialization time and memory use.
If you see "Not enough memory", lower the level (e.g. `-m6`).

## Performance and when to use it

**Speed:** roughly 0.1 - 0.2 MB/s for both compression and decompression (symmetric), single-threaded. For example, 100 MB takes about 10-15 minutes. This is intentional: context-mixing compressors trade time for ratio.

**Works best on:**

- Text, CSV, logs, source code
- Databases and data dumps
- `.exe` / `.dll` files (thanks to the E8/E9 filter)
- Structured raw/binary data (fixed-size record tables)
- Old Excel/Word files (`.xls`, `.doc`)

**Barely shrinks** (already compressed):

- Images/video/audio: JPG, PNG, MP4, MP3
- Archives: ZIP, RAR, 7z
- PDF
- `.xlsx`, `.docx`, `.pptx` (they are really ZIP files)

For the second group the output is about the same size as the input, and the time is wasted.

Reported figures use:

$$
\text{ratio} = \frac{S_{\text{in}}}{S_{\text{out}}} : 1, \qquad
\text{bits per byte} = \frac{8\, S_{\text{out}}}{S_{\text{in}}}
$$

## Building from source

Requirements: a C++17 compiler (g++/MinGW-w64, clang, or MSVC). On Windows, `windows.h` and `shellapi.h` are used for Unicode arguments and console detection.

Example with g++ (MinGW-w64):

```
g++ -O2 -std=c++17 -DGILA_MODE_COMPRESS   gila.cpp -o gila-kompres.exe   -static -lshell32
g++ -O2 -std=c++17 -DGILA_MODE_DECOMPRESS gila.cpp -o gila-dekompres.exe -static -lshell32
```

On Linux/macOS (no `-lshell32`):

```
g++ -O2 -std=c++17 -DGILA_MODE_COMPRESS   gila.cpp -o gila-kompres
g++ -O2 -std=c++17 -DGILA_MODE_DECOMPRESS gila.cpp -o gila-dekompres
```

Build notes:

- `-DGILA_MODE_COMPRESS` or `-DGILA_MODE_DECOMPRESS` selects the program. With neither, compress is the default.
- `-DTUNE` makes the model parameters (`LR`, `LRF`, `LIM`, `AR`, `SH`, `W0`, `FW`) adjustable through environment variables, for tuning experiments. **Do not** use it for release builds: archives can only be opened by a program with exactly the same parameters.
- The compressor and decompressor must be built from the same version of `gila.cpp`.
- The commands above are generic examples; the exact flags used for the `.exe` files in this repo are not recorded in the source.

## How it works

Every **bit** is predicted and then coded with an arithmetic coder. The more accurate the prediction, the fewer bits are spent. Coding a bit $y$ whose predicted probability was $p$ costs about

$$
-\log_2 P(y) = \begin{cases} -\log_2 p & y = 1 \\ -\log_2 (1-p) & y = 0 \end{cases}
$$

bits, so the whole game is making $p$ close to the truth.

1. **Many context models (18 hashed contexts + 2 direct tables).** Each model guesses the next bit from a different context:
   - orders 2, 3, 4, 5, 6, 8, 12, 16 (preceding bytes)
   - **word** model (current word hash, bigram, and skip-word)
   - **sparse** models (skipping certain bytes)
   - **column/table** model (the byte above in the same column, good for CSV/tables)
   - **indirect** models (the byte that previously followed the order-1 / order-2 context)
   - **record** model (detects a fixed record length from a repeating byte distance, then uses bytes from the previous records)
   - direct order 0 and order 1
2. **Match model.** Looks up an earlier occurrence of the last 6+ bytes through a hash table and predicts the byte that followed it (useful for long repeats).
3. **Adaptive slots.** Each context stores a 16-bit probability, a count $n$, and the last 8 bits of history. The probability moves toward the observed bit $y$ with a learning rate that decays with the count:

$$
p \leftarrow p + \frac{y - p}{n + 1.5}, \qquad n \leftarrow \min(n + 1,\ n_{\max})
$$

   The bit history is also mapped through a StateMap for correction.
4. **Three neural mixers + one final mixer.** Predictions are moved to the logistic (logit) domain and blended with online-trained weights. The domain transforms are

$$
\operatorname{squash}(d) = \frac{4096}{1 + e^{-d/256}}, \qquad \operatorname{stretch}(p) = \operatorname{squash}^{-1}(p)
$$

   With inputs $x_i = \operatorname{stretch}(p_i)$, each mixer computes (the integer implementation shifts right by 16 and clamps the dot product to $[-2047, 2047]$)

$$
p_{\text{mix}} = \operatorname{squash}\!\Big(\sum_i w_i\, x_i\Big)
$$

   and then learns by gradient descent on coding cost:

$$
w_i \leftarrow w_i + \eta \, x_i \,(y - p_{\text{mix}})
$$

   Each mixer picks its weight set from a different context: the number of contexts found and the match length, the partial bits of the current byte, and the last byte together with the bit position.
5. **Four APM/SSE stages.** The mixer output is refined by correction tables (contexts: partial bits, order-1, match state, order-2 hash). Each table interpolates linearly between 33 bins over the stretched probability and is updated with $t \leftarrow t + (g - t) / 2^{r}$, where $g$ is the observed bit and $r$ is the rate (6 by default). The four refined probabilities are combined with fixed weights:

$$
p = \frac{p_1 + 2\,p_2 + 2\,p_3 + 3\,p_4}{8}
$$

6. **32-bit arithmetic coder** with 16-bit probability precision. The interval $[x_1, x_2]$ is split at

$$
x_{\text{mid}} = x_1 + \left\lfloor \frac{(x_2 - x_1)\, p_{16}}{2^{16}} \right\rfloor
$$

   and the bit selects the lower part ($y = 1$) or upper part ($y = 0$). Leading bytes that become equal are shifted out.

Pre-processing:

- **E8/E9 filter** for executables (detected by the `MZ` or `\x7FELF` header, size 512 B - 512 MB): relative x86 CALL/JMP addresses become absolute, so the same target produces the same bytes. Perfectly reversible.
- **Ordering**: inside a folder, subfolders come first, then files grouped by extension, then by name.

The decompressor runs exactly the same model and reads bits from the arithmetic decoder, so the process is symmetric (same speed).

## The `.gila` archive format

```
Offset  Size  Contents
0       4     Magic "GILA"
4       1     Format version (currently 1)
5       1     Memory level (1..11)
6       ...   Arithmetic-coded stream containing, in order:
                - Header: entry count (u32), then for each entry:
                    type (u8: 0 file, 1 folder, 2 executable),
                    path length (u32), UTF-8 path with '/' separator,
                    size (u64, absent for folders)
                - The contents of all files, in entry order
                - CRC32 of all file contents (u32)
```

All numbers are little-endian. The header and data share one compressed stream, so the header is compressed too.

The checksum is the standard CRC-32 (reflected polynomial `0xEDB88320`), computed over the original bytes of every file in order.

## Safety and data integrity

- **CRC32** of all data is computed at compression time and compared at decompression time. On mismatch the program prints a warning and exits with code 2.
- **Path traversal protection**: on extraction, paths that are absolute, contain `:`, or contain a `..` component are rejected and the entry is skipped, so a malicious archive cannot write outside the target folder.
- The header is validated (entry count, path length, type), so corrupt archives are rejected.
- Windows SmartScreen may warn because the `.exe` files are not digitally signed. Click **More info**, then **Run anyway**. You can also build from `gila.cpp` yourself.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success (and CRC32 matched, for decompression) |
| 1 | Failure: input not found, not a valid GILA archive, unsupported version/level, corrupt archive, cannot create/open a file, or not enough memory |
| 2 | Decompression finished but CRC32 did not match (corrupt archive) |

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `Not enough memory` | The level is too high for the available RAM. Recompress with a lower level. For decompression, use a machine with enough RAM (the level is fixed in the archive). |
| `Not a valid .gila file` | The magic does not match; the file is not a GILA archive or is truncated. |
| `Unsupported archive version/level` | The archive was made by a different program version. |
| `CRC32 MISMATCH` | The archive is corrupt or truncated (e.g. an incomplete download). |
| `Cannot write ... (skipped)` | The destination path is not writable, or the path in the archive was judged unsafe. |
| Very slow | Normal (~0.1-0.2 MB/s). Use a lower level for small data, and avoid feeding already-compressed files. |
| SmartScreen warning | See [Safety and data integrity](#safety-and-data-integrity). |
| Messages are in Indonesian | The prebuilt `.exe` files predate the translation. Rebuild from `gila.cpp`. |

## Limitations

- Single-threaded and slow; not suited to tens of GB of data or anything needing speed.
- No parallel compression, no partial extraction (decompression always starts from the beginning), and no encryption/password.
- No metadata is stored (modification time, attributes, permissions), only paths and contents.
- Empty folders are kept, but symlinks and special files are skipped (only regular files and folders are archived).
- A file that fails to open during compression is filled with zeros (the program prints a warning).
- Archives are not compatible across different format versions.
