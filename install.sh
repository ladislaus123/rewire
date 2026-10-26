#!/bin/bash
# Install the rewire binary so any terminal can run: rewire
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
built="$root/build/rewire"
dest="/usr/local/bin/rewire"
path_line='export PATH="/usr/local/bin:$PATH"'

if [[ ! -x "$built" ]]; then
    echo "No binary at $built. Building it first."
    cmake -S "$root" -B "$root/build"
    cmake --build "$root/build" --target rewire
fi

sudo mkdir -p /usr/local/bin
sudo cp "$built" "$dest"
sudo chmod +x "$dest"
# The linker signature is rejected for a binary copied into /usr/local/bin.
# macOS then kills it with SIGKILL (Code Signature Invalid). Sign the copy in place.
sudo codesign --force --sign - "$dest"

if [[ -f "$HOME/.zshrc" ]] && grep -qxF "$path_line" "$HOME/.zshrc"; then
    echo "~/.zshrc already puts /usr/local/bin on PATH."
else
    printf '\n%s\n' "$path_line" >> "$HOME/.zshrc"
    echo "Added /usr/local/bin to ~/.zshrc."
fi

"$dest" --help >/dev/null
echo "Installed $dest"
echo "Open a new terminal and run: rewire"
echo "In this terminal, run: source ~/.zshrc"
