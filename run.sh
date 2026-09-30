#!/usr/bin/env bash
# Raccourci conservé pour les habitudes : tout se fait désormais dans build_and_run.sh
exec "$(dirname "$(readlink -f "$0")")/build_and_run.sh" "$@"
