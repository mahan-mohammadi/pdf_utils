# pdfcrop

A fast, single-file command-line PDF toolkit written in C — a local, scriptable CLI alternative to online PDF suites. It acts as a safe front end for [qpdf](https://qpdf.sourceforge.io/) and optional tools ([poppler](https://poppler.freedesktop.org/), Ghostscript, Tesseract / OCRmyPDF, ImageMagick, LibreOffice, and pyHanko). Each command checks only for the tools it needs and tells you what to install if something is missing.

```console
$ pdfcrop merge cover.pdf report.pdf:1-10 -o final.pdf
$ pdfcrop compress --level medium -j 4 -O smaller/ *.pdf
$ pdfcrop ocr --lang eng --deskew scan.pdf
$ pdfcrop watermark --stamp-text "DRAFT {date}" --opacity 0.25 final.pdf

```

## Table of contents

* [Features](https://www.google.com/search?q=%23features)
* [Requirements](https://www.google.com/search?q=%23requirements)
* [Installation](https://www.google.com/search?q=%23installation)
* [1. Install the dependencies](https://www.google.com/search?q=%231-install-the-dependencies)
* [2. Compile](https://www.google.com/search?q=%232-compile)
* [3. Install](https://www.google.com/search?q=%233-install)
* [Uninstall](https://www.google.com/search?q=%23uninstall)
* [Optional: Makefile](https://www.google.com/search?q=%23optional-makefile)


* [Usage](https://www.google.com/search?q=%23usage)
* [Commands](https://www.google.com/search?q=%23commands)
* [Page selection syntax](https://www.google.com/search?q=%23page-selection-syntax)
* [Options](https://www.google.com/search?q=%23options)
* [Examples](https://www.google.com/search?q=%23examples)
* [Working with encrypted PDFs](https://www.google.com/search?q=%23working-with-encrypted-pdfs)
* [Batch and parallel processing](https://www.google.com/search?q=%23batch-and-parallel-processing)
* [Safety and behaviour details](https://www.google.com/search?q=%23safety-and-behaviour-details)
* [Exit status](https://www.google.com/search?q=%23exit-status)
* [Environment variables](https://www.google.com/search?q=%23environment-variables)
* [Limits](https://www.google.com/search?q=%23limits)
* [Troubleshooting](https://www.google.com/search?q=%23troubleshooting)

## Features

**Page manipulation**

* **Extract, reorder, reverse** (`extract`): select pages with flexible expressions (`1-10`, `10-1`, `odd`, `even`, `last`, `all`), repeat pages, or linearize output.
* **Remove pages** (`remove`): delete selected pages and keep the rest.
* **Split** (`split`): break a PDF into fixed-size chunks (`--every N`) or custom semicolon-separated ranges (`--ranges`).
* **Merge** (`merge`): combine multiple PDFs with optional per-file page selections (`file.pdf:1-3,last`).
* **Rotate** (`rotate`): rotate all or selected pages by multiples of 90 degrees.
* **Crop** (`crop`): trim page margins in points (`ALL`, `V,H`, or `T,R,B,L`).
* **Page numbers** (`pagenum`): stamp customizable page numbers (`{n}`, `{total}`) at any position.

**Optimise, convert, and create**

* **Compress** (`compress`): lossless recompression via `qpdf`, or lossy presets (`low`, `medium`, `high`), custom DPI downsampling, and grayscale conversion via Ghostscript. Keeps the original untouched if compression produces no size reduction.
* **Repair** (`repair`): rebuild damaged PDFs using `qpdf`, with automatic Ghostscript fallback.
* **Convert from PDF** (`convert`): export to `png`, `jpg`, `tiff`, `svg`, `eps`, `ps`, `html`, `txt`, `pdfa`, `docx`, `odt`, and `rtf`.
* **Create PDF** (`create`): combine images (JPEG natively; PNG/WebP/etc. via ImageMagick), Office/HTML/text files (via LibreOffice), and existing PDFs into a single PDF.
* **Text extraction** (`text`): dump UTF-8 text to stdout or a file, with optional layout preservation and page-break control.

**Security, editing, OCR, and signing**

* **Encrypt & decrypt** (`encrypt`, `decrypt`): AES-128 or AES-256 encryption with user/owner passwords and permission restrictions (`print`, `copy`, `modify`, `annotate`, `all`), or clean decryption.
* **Find & replace text** (`edit`): best-effort literal string replacement inside uncompressed PDF streams via `qpdf --qdf` and `fix-qdf`.
* **OCR** (`ocr`): make scanned PDFs searchable using `ocrmypdf` (with deskew/rotate/sidecar support) or an automatic `pdftoppm` + `tesseract` fallback.
* **Watermarks & visual stamps** (`watermark`): overlay or underlay text (with `{date}` expansion), images, or another PDF with configurable position, angle, scale, color, and opacity.
* **Digital & visual signatures** (`sign`, `verify`): stamp a visible signature, apply cryptographic PKCS#12 signatures via `pyhanko`, and verify signatures with `pdfsig`.
* **Inspection** (`info`): print file path, size, page count, encryption status, and metadata.

**Batching & safety**

* Process multiple files at once, read file lists from `--from-list`, run jobs in parallel (`-j N`), continue past errors (`-k`), or replace originals atomically (`--in-place`).
* Passwords are never placed on the command line (passed via `0600` temp files and zeroed on exit).
* Never clobbers existing files without `--force`; refuses to overwrite its own input unless `--in-place` is set.

## Requirements

Only a C compiler and `qpdf` are required to build and use the core commands. All other tools are optional and checked at runtime only when invoked.

| Requirement | Needed for | Notes |
| --- | --- | --- |
| C compiler (`gcc` or `clang`) | building | C99 or newer, POSIX system, links with `-lm` |
| `qpdf` (and `fix-qdf`) | all commands except `verify` | must be in `$PATH` (`fix-qdf` is used by `edit`) |
| `poppler` (`pdftotext`, `pdftoppm`, `pdftocairo`, `pdftohtml`, `pdfinfo`, `pdfsig`) | `text`, `convert`, `ocr` (fallback), `verify`, `info`, page sizing | optional |
| `gs` (Ghostscript) | lossy `compress`, `crop`, `convert --to pdfa`, `repair` fallback | optional |
| `ocrmypdf` or `tesseract` | `ocr` | optional (`auto` prefers `ocrmypdf`) |
| `magick` or `convert` (ImageMagick) | non-JPEG images in `watermark`, `sign`, `create` | optional |
| `soffice` / `libreoffice` | `convert` to `docx`/`odt`/`rtf`, `create` from Office/HTML/txt | optional |
| `pyhanko` | cryptographic `sign --cert` | optional (`pip install pyhanko pyhanko-certvalidator`) |

Developed for POSIX systems (Linux, macOS, BSDs; use WSL on Windows).

## Installation

### 1. Install the dependencies

Install the core requirements (`qpdf` and `poppler`), plus any optional tools you plan to use.

**Debian / Ubuntu / Linux Mint**

```sh
sudo apt update
sudo apt install build-essential qpdf poppler-utils
sudo apt install ghostscript ocrmypdf tesseract-ocr imagemagick libreoffice

```

**Fedora / RHEL / Rocky / Alma**

```sh
sudo dnf install gcc make qpdf poppler-utils
sudo dnf install ghostscript ocrmypdf tesseract ImageMagick libreoffice

```

**Arch / Manjaro**

```sh
sudo pacman -S base-devel qpdf poppler
sudo pacman -S ghostscript ocrmypdf tesseract tesseract-data-eng imagemagick libreoffice-fresh

```

**openSUSE**

```sh
sudo zypper install gcc make qpdf poppler-tools
sudo zypper install ghostscript tesseract-ocr ImageMagick libreoffice

```

**Alpine**

```sh
sudo apk add build-base qpdf poppler-utils
sudo apk add ghostscript tesseract-ocr imagemagick libreoffice

```

**macOS (Homebrew)**

```sh
xcode-select --install
brew install qpdf poppler
brew install ghostscript ocrmypdf tesseract imagemagick libreoffice

```

For cryptographic signing with `sign --cert`:

```sh
pip install pyhanko pyhanko-certvalidator

```

### 2. Compile

From the directory containing `pdfcrop.c`:

```sh
cc -O2 -Wall -Wextra -o pdfcrop pdfcrop.c -lm

```

Verify the build:

```sh
./pdfcrop --version
./pdfcrop --help

```

### 3. Install

**System-wide** (all users):

```sh
sudo install -m 755 pdfcrop /usr/local/bin/pdfcrop

```

**Current user only** (no root needed):

```sh
mkdir -p ~/.local/bin
install -m 755 pdfcrop ~/.local/bin/pdfcrop

```

Ensure `~/.local/bin` is on your `PATH`:

```sh
export PATH="$HOME/.local/bin:$PATH"

```

### Uninstall

```sh
sudo rm /usr/local/bin/pdfcrop
rm ~/.local/bin/pdfcrop

```

### Optional: Makefile

Save this next to `pdfcrop.c` as `Makefile` (recipe lines must start with a tab):

```make
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  ?= -lm
PREFIX  ?= /usr/local

pdfcrop: pdfcrop.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: pdfcrop
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 pdfcrop $(DESTDIR)$(PREFIX)/bin/pdfcrop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pdfcrop

clean:
	rm -f pdfcrop

.PHONY: install uninstall clean

```

Then:

```sh
make
sudo make install

```

## Usage

```text
pdfcrop COMMAND [OPTIONS] FILE...
pdfcrop [OPTIONS] INPUT.pdf PAGES [OUTPUT]

```

Options may appear before or after positional arguments. If the first positional argument is not a command name (or if a file with that exact name exists in the current directory), `pdfcrop` defaults to `extract` (or `text` / `info` when legacy `-t` / `-i` flags are given).

## Commands

| Command | Summary | Default suffix / output |
| --- | --- | --- |
| `extract` | Keep, reorder, or reverse pages | `-pages.pdf` |
| `remove` | Delete selected pages | `-trimmed.pdf` |
| `split` | Split into chunks or custom ranges | `<stem>-part001.pdf`, … |
| `merge` | Combine PDFs (`FILE` or `FILE:PAGES`) | `merged.pdf` |
| `rotate` | Rotate pages by 90/180/270 degrees | `-rotated.pdf` |
| `crop` | Trim page margins in points | `-cropped.pdf` |
| `pagenum` | Add page numbers | `-numbered.pdf` |
| `compress` | Shrink PDFs (lossless or lossy) | `-compressed.pdf` |
| `repair` | Rebuild damaged PDFs | `-repaired.pdf` |
| `convert` | Export PDF to images, vector, HTML, text, PDF/A, or Office | format-dependent |
| `create` | Convert images / Office / HTML / text into a PDF | `created.pdf` |
| `text` | Dump UTF-8 text of selected pages | stdout (or `.txt` with `-O`/`-o`) |
| `encrypt` | Add password and permission restrictions | `-encrypted.pdf` |
| `decrypt` | Remove password protection | `-decrypted.pdf` |
| `edit` | Find and replace literal text strings | `-edited.pdf` |
| `ocr` | Add a searchable text layer to scanned PDFs | `-ocr.pdf` |
| `watermark` | Stamp text, image, or PDF over/under pages | `-watermarked.pdf` |
| `sign` | Apply visible and/or PKCS#12 cryptographic signature | `-signed.pdf` |
| `verify` | Check digital signatures | stdout |
| `info` | Print file size, page count, encryption, and metadata | stdout |

## Page selection syntax

Used by `extract`, `remove`, `rotate`, `text`, `convert`, `watermark`, `pagenum`, `sign`, `split --ranges`, and `merge` (`FILE:PAGES`). Entries are separated by commas, semicolons, or newlines (except `split --ranges`, which uses semicolons to separate output files).

| Entry | Meaning |
| --- | --- |
| `3` | page 3 |
| `1-10` | pages 1 through 10 |
| `10-1` | pages 10 down to 1 (reversed) |
| `last` | final page |
| `5-last` | page 5 through the final page |
| `last-1` | whole document, reversed |
| `all` | every page |
| `odd` | pages 1, 3, 5, … |
| `even` | pages 2, 4, 6, … |
| `1-3,7,9-last` | any combination, in the order written |

## Options

### Common options

| Option | Description |
| --- | --- |
| `-o, --output FILE` | Output file path (single input, `merge`, or `create`; `-` = stdout for `text`). |
| `-O, --outdir DIR` | Write outputs into `DIR` (created automatically if missing). |
| `--suffix TEXT` | Override the default output filename suffix. |
| `--in-place` | Replace the original file atomically (not valid for `text`, `convert`, `merge`, `create`, `split`, `info`, `verify`). |
| `-f, --force` | Overwrite existing outputs. |
| `-n, --dry-run` | Show what would happen without writing anything. |
| `-j, --jobs N` | Process `N` files in parallel (disables interactive password prompts). |
| `-k, --keep-going` | Continue processing remaining files if one fails. |
| `--from-list FILE` | Read additional input paths from `FILE` (one per line, `#` comments ignored, `-` = stdin). |
| `--password-file FILE` | Input PDF password from the first line of `FILE`. |
| `--password-env VAR` | Input PDF password from environment variable `VAR`. |
| `--password-stdin` | Input PDF password from stdin. |
| `--no-verify` | Skip post-write `qpdf --check` and page-count verification. |
| `-q, --quiet` | Print only errors. |
| `-v, --verbose` | Print external commands as they run. |
| `-h, --help` | Show help. |
| `-V, --version` | Show version. |

### Command-specific options

| Command(s) | Options |
| --- | --- |
| `extract`, `remove`, `text`, `convert`, `rotate` | `--pages EXPR`, `--linearize`, `--layout`, `--no-page-breaks`, legacy `-t` / `-i` |
| `split` | `--every N` (default `1`), `--ranges "1-3;4-last"` |
| `rotate` | `--angle DEG` (multiple of 90, default `90`), `--on PAGES` (or `--pages`) |
| `crop` | `--margins SPEC` in points: `ALL`, `V,H`, or `T,R,B,L` |
| `pagenum` | `--format TEXT` (default `{n} / {total}`), `--start N` (default `1`), `--position`, `--size PT`, `--color`, `--opacity`, `--on PAGES` |
| `compress` | `--level lossless|low|medium|high` (default `medium`), `--dpi N`, `--grayscale` |
| `convert` | `--to png|jpg|tiff|svg|eps|ps|html|txt|pdfa|docx|odt|rtf`, `--dpi N` (default `150`), `--quality N` (JPEG) |
| `encrypt` | `--user-password-file F`, `--user-password-env V`, `--owner-password-file F`, `--owner-password-env V`, `--deny print,copy,modify,annotate|all`, `--bits 128|256` (default `256`) |
| `edit` | `--replace 'OLD=>NEW'` (repeatable, up to 64 per run) |
| `ocr` | `--lang CODES` (default `eng`, e.g. `deu+eng`), `--engine auto|ocrmypdf|tesseract`, `--sidecar FILE.txt`, `--deskew`, `--rotate`, `--force-ocr` |
| `watermark`, `sign` | `--stamp-text TEXT` (`{date}` supported), `--stamp-image JPG/PNG`, `--stamp-pdf FILE`, `--on PAGES` (`watermark`: `all`, `sign`: `last`), `--layer over|under`, `--position center|top|bottom|left|right|top-left|top-right|bottom-left|bottom-right`, `--size PT`, `--angle DEG`, `--opacity 0-1`, `--scale F`, `--color NAME|#RRGGBB` |
| `sign` (digital) | `--cert FILE.p12`, `--cert-pass-file F`, `--cert-pass-env V`, `--reason TEXT`, `--location TEXT`, `--field NAME` |

## Examples

**Extract, remove, split, merge, rotate, crop, and number pages**

```sh
pdfcrop book.pdf "1-20,30,40-35" -o mix.pdf
pdfcrop remove book.pdf "2,5-7" -o cleaned.pdf
pdfcrop split --every 5 -O parts/ book.pdf
pdfcrop split --ranges "1-3;4-10;11-last" -O chapters/ book.pdf
pdfcrop merge cover.pdf body.pdf:1-20 appendix.pdf:last -o complete.pdf
pdfcrop rotate --angle 90 --on odd book.pdf
pdfcrop crop --margins 36,24 paper.pdf
pdfcrop pagenum --position bottom-right --format "Page {n} of {total}" report.pdf

```

**Compress, repair, convert, create, and extract text**

```sh
pdfcrop compress --level medium report.pdf
pdfcrop compress --level high --dpi 120 --grayscale scan.pdf
pdfcrop compress --level lossless --in-place report.pdf
pdfcrop repair broken.pdf
pdfcrop convert --to png --dpi 200 --pages 1-3 slides.pdf -O images/
pdfcrop convert --to docx report.pdf
pdfcrop convert --to pdfa archive.pdf
pdfcrop create photo1.jpg photo2.png notes.docx -o bundle.pdf
pdfcrop text --layout invoice.pdf 1

```

**Encrypt, decrypt, edit, OCR, watermark, and sign**

```sh
pdfcrop encrypt --bits 256 --deny print,copy secret.pdf
pdfcrop decrypt --password-file ~/.secrets/pw secret-encrypted.pdf
pdfcrop edit --replace 'Draft=>Final' --replace '2025=>2026' contract.pdf
pdfcrop ocr --lang eng --deskew --rotate --sidecar out.txt scan.pdf
pdfcrop watermark --stamp-text "CONFIDENTIAL {date}" --color red --opacity 0.2 report.pdf
pdfcrop sign --stamp-image sig.png --position bottom-right --on last contract.pdf
pdfcrop sign --cert me.p12 --reason "Approved" --location "Amsterdam" contract.pdf
pdfcrop verify contract-signed.pdf
pdfcrop info report.pdf

```

## Working with encrypted PDFs

If an input PDF requires a password and none was provided, `pdfcrop` prompts with hidden input on a terminal (up to two attempts). For scripts or parallel runs (`-j`), supply the password non-interactively:

```sh
pdfcrop --password-file ~/.secrets/pdfpw extract secret.pdf all -o open.pdf
PDFPW='hunter2' pdfcrop --password-env PDFPW decrypt secret.pdf
echo 'hunter2' | pdfcrop --password-stdin info secret.pdf

```

* Passwords are written to a `0600` temporary file for `qpdf` and deleted on exit or signal; in-memory buffers are zeroed on cleanup.
* Passwords containing newlines are not supported.
* When using `encrypt` with `--deny` and no owner password, a random 128-bit owner password is generated automatically so restrictions cannot be trivially stripped.

## Batch and parallel processing

Most commands accept multiple input files directly or via `--from-list`:

```sh
pdfcrop compress --level medium -j 4 -k -O compressed/ *.pdf
find ./scans -name '*.pdf' | pdfcrop ocr --lang eng --from-list - -j 2 -O searchable/

```

* With `-j N` (`N > 1`), interactive password prompts are disabled; pass `--password-file` or `--password-env` if inputs are encrypted.
* Use `-k` / `--keep-going` so a single broken file does not halt the rest of the batch.
* With `extract`, `remove`, or `text` on multiple files, pass `--pages EXPR` so all positional arguments are treated as input files.

## Safety and behaviour details

* **No clobbering:** Without `--force`, outputs are published via `link()` + `unlink()`, failing atomically if the destination already exists. With `--force` or `--in-place`, `rename()` replaces the target atomically.
* **Same-file protection:** Input and output are compared by device and inode to prevent accidental self-overwrites unless `--in-place` is explicitly requested.
* **Atomic writes:** Output files are written to a hidden temporary file in the destination directory and moved into place only after `qpdf --check` succeeds.
* **Cleanup:** All temporary files and directories are removed on exit, error, `SIGINT`, `SIGTERM`, and `SIGHUP`, and terminal echo is restored if interrupted at a password prompt.
* **No wasted compression:** If `compress` produces a file equal to or larger than the input, nothing is written and the original is kept.

## Exit status

| Code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | Runtime error (missing tool, bad file, wrong password, invalid pages, conversion failure, output exists, …) |
| `2` | Usage error (bad options or arguments) |
| `128+N` | Interrupted by signal `N` (for example `130` for Ctrl-C) |

## Environment variables

| Variable | Effect |
| --- | --- |
| `TMPDIR` | Directory for temporary files, decrypted intermediates, and password files (defaults to `/tmp`). |
| `NO_COLOR` | Disables coloured warnings and error messages when set. |
| *(custom)* | Any variable passed to `--password-env`, `--user-password-env`, `--owner-password-env`, or `--cert-pass-env`. |

## Limits

* Up to `1,000,000` pages per selection; resolved qpdf page expressions must be under `100,000` bytes.
* Up to `64` `--replace` pairs per `edit` invocation.
* Paths are limited to `PATH_MAX` (typically 4096 bytes).
* Passwords are limited to `4095` bytes and may not contain newlines.

## Troubleshooting

**`'tool' is not installed or not in PATH`**
Install the missing package listed in [Requirements](https://www.google.com/search?q=%23requirements) and verify it is on your `$PATH`.

**`output 'x.pdf' already exists (use --force to overwrite)`**
Pick a different output path with `-o` / `-O`, use `--in-place`, or pass `--force`.

**`text not found` during `edit**`
`edit` only replaces plain literal strings inside PDF content streams. Text split by kerning, encoded in subset/CID fonts, or stored as scanned images cannot be replaced directly.

**`Text output is empty`**
The PDF is likely a scan with no embedded text layer. Run `pdfcrop ocr` on it first.

**`PDF is password protected; use --password-file, --password-env or --password-stdin`**
A password is needed, but stdin/stderr is not a terminal (or `-j` parallel mode is active). Supply the password using one of the non-interactive flags.

**`undefined reference to 'cos'` / `'sin'` when compiling**
Make sure `-lm` is at the end of the compiler command: `cc -O2 -Wall -Wextra -o pdfcrop pdfcrop.c -lm`.

```

```
