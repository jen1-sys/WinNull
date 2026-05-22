# WinNull

**WinNull** is a Windows kernel-mode null driver for Windows 11.

It provides a `/dev/null`-style sink device for Windows and a file-system minifilter that can turn selected files into zero-size black-hole files.

In simple terms:

- Writes succeed
- Data is discarded
- Reads return EOF / 0 bytes
- Matched files stay zero-size
- No file payload is written to disk

WinNull is intended for development, testing, experimentation, and learning Windows kernel/minifilter behavior.

---

## Features

### 1. Direct null device

WinNull exposes a direct null sink device:

```bat
\\.\WinNull
```

Examples:

```bat
echo hello > \\.\WinNull
copy bigfile.bin \\.\WinNull
```

Behavior:

- Open succeeds
- Write succeeds
- Data is discarded
- Read succeeds and returns 0 bytes
- Flush, cleanup, and close succeed

This is similar to `/dev/null` on Unix-like systems.

---

### 2. `.wnull` null files

Any file ending with the `.wnull` extension is treated as a null file.

Example:

```bat
copy bigfile.bin test.wnull
```

The copy operation succeeds, but the file payload is discarded.

The resulting `.wnull` file stays zero-size. Reading or copying the `.wnull` file returns 0 bytes.

---

### 3. `WinNull` folder rule

Files created inside a folder named `WinNull` are treated as null files.

Example:

```bat
mkdir C:\WinNull
copy bigfile.bin C:\WinNull\test.bin
```

The folder must already exist.

WinNull does not create virtual folders in the current version.

Behavior:

- File creation succeeds
- Writes succeed
- Data is discarded
- File remains zero-size
- Reads return EOF / 0 bytes

---

## Current behavior

Current version: **v1.6**

WinNull v1.6 uses zero-size semantics.

Matched files:

- Accept writes
- Discard written data
- Read back as empty
- Stay zero-size
- Do not store payload data on disk

Matched paths:

- `*.wnull`
- Files inside any folder path containing `\WinNull\`

Direct device path:

- `\\.\WinNull`

---

## Included files

A typical WinNull release package contains:

```text
WinNull.sys    - Kernel-mode driver binary
WinNull.inf    - Driver installation file
winnull.cat    - Catalog/signature file
WinNull.bat    - Helper script for install, load, unload, uninstall, and status
```

---

## Installation

Run `WinNull.bat` as Administrator.

The helper script provides menu options for:

- Installing the driver
- Loading the driver
- Unloading the driver
- Uninstalling the driver
- Checking driver status

The driver is configured to start automatically at system startup so that configured `.wnull` files and `WinNull` folders are protected as early as possible after boot.

---

## Manual commands

### Load the driver

```bat
fltmc load WinNull
```

### Unload the driver

```bat
fltmc unload WinNull
```

### Check minifilter status

```bat
fltmc filters
fltmc instances
```

### Check service status

```bat
sc query WinNull
```

---

## Test examples

### Test direct null device

```bat
echo hello > \\.\WinNull
copy bigfile.bin \\.\WinNull
```

Expected result:

- Operation succeeds
- Data is discarded
- No output file is created because `\\.\WinNull` is a device sink

---

### Test `.wnull` file

```bat
copy bigfile.bin test.wnull
dir test.wnull
copy test.wnull out.bin
dir out.bin
```

Expected result:

- `copy bigfile.bin test.wnull` succeeds
- `test.wnull` remains zero-size
- Copying `test.wnull` to another file creates a zero-byte output file

---

### Test `WinNull` folder rule

```bat
mkdir C:\WinNull
copy bigfile.bin C:\WinNull\test.bin
dir C:\WinNull\test.bin
copy C:\WinNull\test.bin C:\temp\out.bin
dir C:\temp\out.bin
```

Expected result:

- Copy succeeds
- `C:\WinNull\test.bin` remains zero-size
- Copying it back out produces a zero-byte file

---

## Driver signing note

Windows 11 requires kernel-mode drivers to be properly signed.

For development, loading may require one of the following:

- Test signing mode
- Disabled driver signature enforcement for the current boot
- A Microsoft-signed driver package

Installing a test certificate may allow the driver package to install, but it does not always allow the kernel driver to load.

Loading is controlled by Windows kernel-mode code integrity policy.

On systems with Secure Boot enabled, test-signed drivers may still be blocked unless the system is configured appropriately.

For normal use on standard Windows 11 systems, a Microsoft-signed driver package is recommended.

---

## Development safety warning

This is a kernel-mode driver.

A bug in a kernel driver can crash Windows.

Recommended development environment:

- Windows 11 virtual machine
- Snapshots enabled
- Test data only
- Driver signing / test mode configured for development

---

## Important project folder warning

Do not place the source project inside a folder named exactly:

```text
WinNull
```

WinNull uses `\WinNull\` as a folder rule.

If the driver is loaded while the project is inside a path containing `\WinNull\`, the driver may null writes to its own source files, build files, project files, or output files.

Use a safer development folder name such as:

```text
WinNull.jen1
WinNull.Dev
WinNull.Source
```

Or use another folder name that does not contain `\WinNull\` as a path component.

---

## Known limitations

- The `WinNull` folder must already exist.
- WinNull does not create virtual folders.
- WinNull is not a full filesystem.
- Directory virtualization is not implemented.
- Process I/O counters may still show logical read/write activity even when no payload data is written to disk.
- Proper driver signing is required for normal loading on Windows 11.

---

## Notes about I/O counters

Tools such as Process Explorer may still show I/O activity when copying data to a `.wnull` file or to a file inside a `WinNull` folder.

This is expected.

The application still issues read/write requests, and WinNull completes those requests successfully.

The important behavior is that payload data is discarded and no real file content is stored.

---

## Project status

Current version: **v1.6**

Working features:

- Single-driver design
- File-system minifilter
- Direct `\\.\WinNull` null device
- `.wnull` null-file extension
- `WinNull` folder rule
- Zero-size matched files
- Runtime unload support
- BAT helper for install/load/unload/uninstall/status

---

## License

MIT License

Copyright (c) jen1

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

---

## Disclaimer

WinNull is experimental kernel-mode software.

Use it at your own risk.

Kernel-mode bugs can cause system instability, data loss, or BSODs.

Always test in a virtual machine before using it on a real system.
