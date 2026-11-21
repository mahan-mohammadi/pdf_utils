# pdfcrop

A small, fast command-line tool written in C for **extracting, reordering and reversing pages of a PDF**, and for **dumping the text** of selected pages. It is a safe front end for [qpdf](https://qpdf.sourceforge.io/) (page handling) and `pdftotext` from [poppler](https://poppler.freedesktop.org/) (text extraction).

```console
$ pdfcrop book.pdf "1-20,30,last" -o excerpt.pdf
$ pdfcrop -t book.pdf 5-9 > chapter.txt
```

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
  - [1. Install the dependencies](#1-install-the-dependencies)
  - [2. Compile](#2-compile)
  - [3. Install](#3-install)
  - [Uninstall](#uninstall)
  - [Optional: Makefile](#optional-makefile)
- [Usage](#usage)
- [Page selection syntax](#page-selection-syntax)
- [Options](#options)
- [Examples](#examples)
- [Working with encrypted PDFs](#working-with-encrypted-pdfs)
- [Text extraction](#text-extraction)
- [Safety and behaviour details](#safety-and-behaviour-details)
- [Exit status](#exit-status)
- [Environment variables](#environment-variables)
- [Limits](#limits)
- [Troubleshooting](#troubleshooting)

## Features

**Page extraction**

- Extract any selection of pages into a new PDF.
- Flexible page expressions: single pages, ranges, reversed ranges, `last`, `all`, `odd`, `even`, and any comma-separated combination.
- Pages can be repeated or put in any order, so you can also reorder or reverse a document.
- Output is always written **decrypted**, even when the source is password protected.
- Optional **linearized** (web-optimised) output.
- Output is **verified** after writing: `qpdf --check` plus a page-count comparison against the selection (can be disabled).

**Text extraction**

- `--text` dumps the text of the selected pages instead of writing a PDF.
- Text goes to **stdout** by default (pipe-friendly) or to a file.
- Page order follows your selection, e.g. `4-1` outputs the text of page 4 first.
- `--layout` preserves the physical page layout; `--no-page-breaks` removes the form-feed page separators.
- Always UTF-8.

**Inspection**

- `--info` prints the page count and whether the PDF is encrypted.
- `--dry-run` shows exactly what would be done without writing anything.

**Passwords**

- Automatic hidden prompt on a terminal when a password is needed.
- Non-interactive options for scripts: `--password-file`, `--password-env`, `--password-stdin`.
- The password is never placed on a command line, so it does not show up in `ps`. It is passed to qpdf through a `0600` temporary file that is deleted on exit.

**Safety**

- Never overwrites an existing file unless `--force` is given.
- Refuses to write over its own input (detected by inode, so symlinks and hard links cannot fool it).
- Output is written to a temporary file first and moved into place atomically, so a failed run never leaves a half-written file behind.
- Temporary files and the terminal state are cleaned up on errors and on Ctrl-C / SIGTERM / SIGHUP.

**Usability**

- Standard `getopt_long` interface with short and long options, `--help` and `--version`.
- `--quiet` and `--verbose` modes (verbose prints every qpdf / pdftotext command that is run).
- Coloured error messages on terminals (disable with `NO_COLOR`).
- Meaningful exit codes for scripting.
- Backwards-compatible legacy form: `pdfcrop input.pdf "pages" output-name`.

## Requirements

| Requirement | Needed for | Notes |
|---|---|---|
| A C compiler (gcc or clang) | building | C99 or newer, POSIX system |
| `qpdf` | everything | must be in `$PATH` |
| `pdftotext` (poppler) | `--text` only | must be in `$PATH` |

Developed and tested on Linux. It uses only POSIX interfaces (`fork`, `execvp`, `mkstemp`, `getopt_long`, `termios`), so it should also build on macOS and the BSDs. On Windows, use WSL.

## Installation

### 1. Install the dependencies

**Debian / Ubuntu / Linux Mint**

```sh
sudo apt update
sudo apt install build-essential qpdf poppler-utils
```

**Fedora / RHEL / Rocky / Alma**

```sh
sudo dnf install gcc make qpdf poppler-utils
```

**Arch / Manjaro**

```sh
sudo pacman -S base-devel qpdf poppler
```

**openSUSE**

```sh
sudo zypper install gcc make qpdf poppler-tools
```

**Alpine**

```sh
sudo apk add build-base qpdf poppler-utils
```

**macOS (Homebrew)**

```sh
xcode-select --install      # compiler, if not already installed
brew install qpdf poppler
```

If you only need page extraction and never use `--text`, `poppler` is optional.

Check that everything is available:

```sh
qpdf --version
pdftotext -v        # only needed for --text
cc --version
```

### 2. Compile

From the directory containing `pdfcrop.c`:

```sh
cc -O2 -Wall -Wextra -o pdfcrop pdfcrop.c
```

This produces an executable called `pdfcrop` in the current directory. It should compile with no warnings. You can use `gcc` or `clang` instead of `cc`.

Quick test:

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

Make sure `~/.local/bin` is on your `PATH`. If it is not, add this to your `~/.bashrc` or `~/.zshrc` and restart the shell:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Verify the installation:

```sh
command -v pdfcrop
pdfcrop --version
```

### Uninstall

```sh
sudo rm /usr/local/bin/pdfcrop      # system-wide install
rm ~/.local/bin/pdfcrop             # per-user install
```

### Optional: Makefile

If you prefer `make`, save this next to `pdfcrop.c` as `Makefile` (recipe lines must start with a tab):

```make
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

pdfcrop: pdfcrop.c
	$(CC) $(CFLAGS) -o $@ $<

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
sudo make install                 # or: make install PREFIX=$HOME/.local
```

## Usage

```text
pdfcrop [OPTIONS] INPUT.pdf PAGES
pdfcrop [OPTIONS] INPUT.pdf PAGES OUTPUT          (legacy form)
pdfcrop --text [OPTIONS] INPUT.pdf PAGES
pdfcrop --info INPUT.pdf
```

Options may appear before or after the positional arguments.

## Page selection syntax

`PAGES` is a list of entries separated by a comma, semicolon or newline. Spaces around entries are ignored. Pages are numbered from 1.

| Entry | Meaning |
|---|---|
| `3` | page 3 |
| `1-10` | pages 1 through 10 |
| `10-1` | pages 10 down to 1 (reversed) |
| `last` | the final page |
| `5-last` | page 5 through the final page |
| `last-1` | the whole document, reversed |
| `all` | every page |
| `odd` | pages 1, 3, 5, … |
| `even` | pages 2, 4, 6, … |
| `1-3,7,9-last` | any combination, in the order written |

Pages may be repeated (`1,1,1`), and the output follows the order you write. Any page outside the document is an error, and nothing is written.

## Options

| Option | Description |
|---|---|
| `-o, --output FILE` | Output file. Default for PDF mode: `<input>-pages.pdf` in the current directory. `.pdf` is appended if missing. With `--text`: stdout (or `-` explicitly), or a file path used exactly as given. |
| `-f, --force` | Overwrite the output file if it exists. |
| `-n, --dry-run` | Print the summary and the resolved page selection, write nothing. |
| `-i, --info` | Print file path, page count and encryption status, then exit. |
| `-t, --text` | Extract text of the selected pages instead of writing a PDF. Needs `pdftotext`. |
| `--layout` | With `--text`: keep the physical layout of the page. |
| `--no-page-breaks` | With `--text`: do not insert a form feed between pages. |
| `--password-file FILE` | Read the PDF password from the first line of `FILE`. |
| `--password-env VAR` | Read the PDF password from environment variable `VAR`. |
| `--password-stdin` | Read the PDF password from one line on standard input. |
| `--linearize` | Write a linearized (web-optimised) PDF. PDF mode only. |
| `--no-verify` | Skip the post-write `qpdf --check` and page-count check. |
| `-q, --quiet` | Print only errors. |
| `-v, --verbose` | Print the qpdf / pdftotext commands being run. |
| `-h, --help` | Show help. |
| `-V, --version` | Show version. |

Rules:

- Only one of `--password-file`, `--password-env`, `--password-stdin` may be used.
- `--quiet` and `--verbose` cannot be combined.
- `--layout` and `--no-page-breaks` require `--text`.
- `--text` cannot be combined with `--info`.
- `--info` takes exactly one argument, the input file.

## Examples

**Extract pages**

```sh
pdfcrop book.pdf 1-20 -o intro.pdf             # first 20 pages
pdfcrop book.pdf "1-20,30,40-35" -o mix.pdf    # ranges, a single page, a reversed range
pdfcrop book.pdf odd -o odd.pdf                # odd pages only
pdfcrop book.pdf "5-last" -o rest.pdf          # page 5 to the end
pdfcrop book.pdf last-1 -o reversed.pdf        # whole document in reverse
pdfcrop book.pdf all -o copy.pdf --linearize   # full copy, web optimised
pdfcrop book.pdf 1 -o out/title                # creates out/title.pdf (the out/ directory must exist)
pdfcrop book.pdf 1-3                           # creates book-pages.pdf
pdfcrop book.pdf 1-3 result                    # legacy form, creates result.pdf
```

**Inspect and preview**

```sh
pdfcrop --info book.pdf
pdfcrop -n book.pdf "1-5,last"       # see what would happen
pdfcrop -v book.pdf 1-3 -o x.pdf     # show the qpdf commands
```

**Extract text**

```sh
pdfcrop -t book.pdf 5-9                           # text to the terminal
pdfcrop -t book.pdf 5-9 > chapter.txt             # redirect to a file
pdfcrop -t book.pdf 5-9 -o chapter.txt            # write a file directly
pdfcrop -t --layout invoice.pdf 1                 # keep columns / tables aligned
pdfcrop -t --no-page-breaks book.pdf all | wc -w  # word count without form feeds
pdfcrop -t book.pdf all | grep -n "keyword"       # search a PDF
```

**Scripting**

```sh
# Split a book into 10-page chunks
n=$(pdfcrop -i book.pdf | awk '/^Pages:/ {print $2}')
for ((s = 1; s <= n; s += 10)); do
    e=$((s + 9)); ((e > n)) && e=$n
    pdfcrop -q book.pdf "$s-$e" -o "part-$s-$e.pdf" || break
done
```

## Working with encrypted PDFs

If the PDF requires a password and you did not supply one, `pdfcrop` prompts for it with hidden input (terminal only). In scripts, use one of:

```sh
pdfcrop --password-file ~/.secrets/pdfpw secret.pdf all -o open.pdf
PDFPW='hunter2' pdfcrop --password-env PDFPW secret.pdf all -o open.pdf
echo 'hunter2' | pdfcrop --password-stdin secret.pdf all -o open.pdf
```

Notes:

- If a password is required, none is supplied and there is no terminal to prompt on, `pdfcrop` exits with an error instead of hanging.
- A wrong password gives `incorrect PDF password`.
- Passwords containing a newline are not supported.
- PDF output is **always decrypted**. The summary shows `Source encrypted: yes` and `Output encrypted: no`.
- Prefer `--password-file` or `--password-stdin` over `--password-env` on shared machines, since environment variables can be visible to other processes of the same user.

## Text extraction

Text mode works in two steps: the selected pages are first extracted into a temporary decrypted PDF (reusing all of the page-selection and password logic above), then `pdftotext` converts that file to UTF-8 text.

- Output goes to **stdout** unless you pass `-o FILE`. While text goes to stdout, all progress messages go to stderr, so pipes stay clean.
- With `-o FILE`, the path is used exactly as written (no extension is added), and existing files are protected unless `--force` is given.
- Pages are separated by a form-feed character (`\f`) unless `--no-page-breaks` is used.
- `pdftotext` only reads text that is embedded in the PDF. **Scanned documents (images of text) produce empty output**; they need OCR (for example `ocrmypdf`) first.
- If `pdftotext` is not installed, `--text` fails with a clear message and the rest of the tool is unaffected.

## Safety and behaviour details

- **No clobbering.** Without `--force`, an existing output file is never touched. The final step uses a hard link, which fails atomically if the destination exists. With `--force`, the file is replaced atomically with `rename`.
- **Same-file protection.** The input and output are compared by device and inode, so the tool will not overwrite its own input, even through a symlink.
- **Atomic output.** Data is written to a hidden temporary file (`.pdfcrop-XXXXXX`) in the destination directory and moved into place only after it has been verified. In text mode, the intermediate PDF lives in `$TMPDIR` (default `/tmp`).
- **Verification.** After extraction, `qpdf --check` is run on the result and its page count must equal the number of selected pages. Use `--no-verify` to skip this on very large files.
- **File permissions.** Output files get normal permissions according to your `umask` (not the `0600` of the temporary file).
- **Cleanup.** Temporary files (including the password file) are removed on success, on errors and on `SIGINT`/`SIGTERM`/`SIGHUP`. Terminal echo is restored if you press Ctrl-C at the password prompt.
- **Warnings.** qpdf warnings (for example about slightly damaged PDFs) do not abort the run; hard errors do.
- **Dry run.** `--dry-run` still validates the input, password and page expression, so it is a good way to check a command before running it. It does not complain about an existing output file.
- **Compact commands.** Consecutive pages are collapsed to ranges when talking to qpdf (`1,2,3,4` becomes `1-4`), so very large selections do not hit the operating system's argument-length limit.

## Exit status

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Runtime error (bad file, wrong password, invalid pages, qpdf/pdftotext failure, output exists, …) |
| `2` | Usage error (bad options or arguments) |
| `128+N` | Interrupted by signal N (for example 130 for Ctrl-C) |

## Environment variables

| Variable | Effect |
|---|---|
| `TMPDIR` | Directory for the password temp file and, in text mode, the intermediate PDF. Defaults to `/tmp`. |
| `NO_COLOR` | If set, disables coloured error messages. |
| *(your choice)* | Any variable named with `--password-env`. |

## Limits

- At most 1,000,000 pages in a single selection.
- The resolved page expression passed to qpdf must be under 100,000 characters; for extreme selections, split the work into several runs.
- Paths are limited to `PATH_MAX` (typically 4096 bytes).
- Passwords are limited to 4095 bytes and may not contain newlines.

## Troubleshooting

**`qpdf is not installed or not executable`**
Install qpdf (see [Installation](#1-install-the-dependencies)) and make sure it is on your `PATH`.

**`pdftotext is not installed`**
Install `poppler-utils` (Debian/Ubuntu/Fedora), `poppler` (Arch/Homebrew) or `poppler-tools` (openSUSE). Only `--text` needs it.

**`output 'x.pdf' already exists (use --force to overwrite)`**
Choose another name with `-o` or add `--force`.

**`pages must be between 1 and N`**
A page in your expression is outside the document. Run `pdfcrop --info file.pdf` to see the page count, or use `last`.

**`PDF is password protected; use --password-file, --password-env or --password-stdin`**
The PDF needs a password and there is no terminal to prompt on. Supply one with a password option.

**`cannot create temporary file in '...'`**
The destination directory does not exist or is not writable. Create it first (`mkdir -p out`), or check permissions. In text mode, check that `$TMPDIR` is writable.

**Text output is empty**
The PDF is probably a scan. Run OCR on it first.

**`command not found: pdfcrop`**
The install directory is not on your `PATH`. See [Install](#3-install).
