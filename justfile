# Build, index every discovered Source game, and mount the result.
#
# `just up` is idempotent: re-running it unmounts the old mount, stops the
# index watcher, rebuilds the index, and starts both again.

set shell := ["bash", "-euo", "pipefail", "-c"]

root := justfile_directory()
build_dir := root + "/build/linux"
database := build_dir + "/sauce-full.db"
mountpoint := "/mnt/sauce-full"
# Reported permission bits. The mount is still read-only, but copies made with
# cp inherit these, so owner-writable bits keep extracted files editable.
file_mode := "0644"
dir_mode := "0755"
run_dir := build_dir + "/run"
log_dir := build_dir + "/log"

# Show the available recipes.
default:
    @just --list

alias start := up
alias restart := up
alias full := up

# Configure and build the indexer, CLI, and FUSE mount.
build:
    cmake --preset linux
    cmake --build --preset linux

# Build the Doxygen/Breathe/Sphinx site into build/docs/html.
#
# Needs doxygen (pacman -S doxygen) plus the Python requirements, either active
# on PATH or installed into build/docs-venv.
docs:
    #!/usr/bin/env bash
    set -euo pipefail
    sphinx="$(command -v sphinx-build || echo "{{ root }}/build/docs-venv/bin/sphinx-build")"
    cmake -S docs -B build/docs -DSPHINX_EXECUTABLE="$sphinx"
    cmake --build build/docs --target docs
    echo "documentation: {{ root }}/build/docs/html/index.html"

# Build a full index, keep it fresh with the watcher, and mount it.
up: stop build
    @just _ensure-mountpoint
    @just _start-watch
    @just _wait-for-index
    @just _start-mount
    @just status

# Stop the watcher, then rebuild the index once without watching.
index: build
    @just _stop-index
    @mkdir -p "{{ build_dir }}"
    "{{ build_dir }}/sauce-fs-index" --database "{{ database }}"

# Remount the existing index (does not re-index).
mount: _stop-mount build _ensure-mountpoint
    @just _start-mount
    @just status

# Unmount and stop the index watcher.
stop:
    @just _stop-mount
    @just _stop-index

# Report what is running and where the logs are.
status:
    #!/usr/bin/env bash
    set -euo pipefail
    index_pid="{{ run_dir }}/index.pid"
    mount_pid="{{ run_dir }}/mount.pid"
    if [ -f "$index_pid" ] && kill -0 "$(cat "$index_pid")" 2>/dev/null; then
        echo "index watcher: running (pid $(cat "$index_pid"))"
    else
        echo "index watcher: stopped"
    fi
    if mountpoint -q "{{ mountpoint }}" 2>/dev/null; then
        echo "mount:         {{ mountpoint }}"
    else
        echo "mount:         not mounted"
    fi
    if [ -f "$mount_pid" ] && kill -0 "$(cat "$mount_pid")" 2>/dev/null; then
        echo "mount daemon:  running (pid $(cat "$mount_pid"))"
    else
        echo "mount daemon:  stopped"
    fi
    echo "database:      {{ database }}"
    echo "logs:          {{ log_dir }}"

# Print the last lines of the indexer and mount logs.
logs:
    @mkdir -p "{{ log_dir }}"
    @for file in "{{ log_dir }}/index.log" "{{ log_dir }}/mount.log"; do \
        echo "==> $file <=="; \
        tail -n 40 "$file" 2>/dev/null || true; \
    done

# --- internals -------------------------------------------------------------

_ensure-mountpoint:
    #!/usr/bin/env bash
    set -euo pipefail
    target="{{ mountpoint }}"
    if [ -d "$target" ] && [ -w "$target" ]; then
        exit 0
    fi
    if [ ! -d "$target" ]; then
        echo "creating $target"
        mkdir -p "$target" 2>/dev/null || {
            sudo mkdir -p "$target"
            sudo chown "$(id -u):$(id -g)" "$target"
        }
    else
        echo "$target is not writable; taking ownership with sudo"
        sudo chown "$(id -u):$(id -g)" "$target"
    fi

_start-watch:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ run_dir }}" "{{ log_dir }}"
    pidfile="{{ run_dir }}/index.pid"
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "index watcher already running (pid $(cat "$pidfile"))"
        exit 0
    fi
    rm -f "$pidfile"
    : > "{{ log_dir }}/index.log"
    nohup "{{ build_dir }}/sauce-fs-index" \
        --database "{{ database }}" --watch \
        > "{{ log_dir }}/index.log" 2>&1 < /dev/null &
    echo $! > "$pidfile"
    echo "index watcher started (pid $(cat "$pidfile"))"

_wait-for-index:
    #!/usr/bin/env bash
    set -euo pipefail
    pidfile="{{ run_dir }}/index.pid"
    log="{{ log_dir }}/index.log"
    echo "waiting for the initial full index..."
    for _ in $(seq 1 7200); do
        if grep -q "watching .* root(s)" "$log" 2>/dev/null; then
            echo "index ready"
            exit 0
        fi
        if [ ! -f "$pidfile" ] || ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "index watcher exited before the index was ready" >&2
            tail -n 20 "$log" >&2 || true
            exit 1
        fi
        sleep 0.5
    done
    echo "timed out waiting for the index; see $log" >&2
    exit 1

_start-mount:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ run_dir }}" "{{ log_dir }}"
    pidfile="{{ run_dir }}/mount.pid"
    log="{{ log_dir }}/mount.log"
    if mountpoint -q "{{ mountpoint }}" 2>/dev/null; then
        echo "already mounted at {{ mountpoint }}"
        exit 0
    fi
    rm -f "$pidfile"
    : > "$log"
    nohup "{{ build_dir }}/sauce-fs-mount" \
        --database "{{ database }}" \
        --file-mode "{{ file_mode }}" --dir-mode "{{ dir_mode }}" \
        -f "{{ mountpoint }}" \
        > "$log" 2>&1 < /dev/null &
    echo $! > "$pidfile"
    for _ in $(seq 1 50); do
        if mountpoint -q "{{ mountpoint }}" 2>/dev/null; then
            echo "mounted {{ mountpoint }}"
            exit 0
        fi
        if ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "mount daemon exited" >&2
            tail -n 20 "$log" >&2 || true
            exit 1
        fi
        sleep 0.1
    done
    echo "timed out waiting for {{ mountpoint }}; see $log" >&2
    exit 1

_stop-mount:
    #!/usr/bin/env bash
    set -euo pipefail
    pidfile="{{ run_dir }}/mount.pid"
    if mountpoint -q "{{ mountpoint }}" 2>/dev/null; then
        fusermount3 -u "{{ mountpoint }}" 2>/dev/null ||
            umount -l "{{ mountpoint }}" 2>/dev/null || true
        echo "unmounted {{ mountpoint }}"
    fi
    if [ -f "$pidfile" ]; then
        pid="$(cat "$pidfile")"
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$pidfile"
    fi
    pkill -f "[s]auce-fs-mount .*{{ mountpoint }}" 2>/dev/null || true

_stop-index:
    #!/usr/bin/env bash
    set -euo pipefail
    pidfile="{{ run_dir }}/index.pid"
    if [ -f "$pidfile" ]; then
        pid="$(cat "$pidfile")"
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null || true
        rm -f "$pidfile"
    fi
    pkill -f "[s]auce-fs-index .*{{ database }}" 2>/dev/null || true
