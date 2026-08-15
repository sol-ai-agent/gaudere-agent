#!/bin/sh
set -eu

quadlet_directory="${XDG_CONFIG_HOME:-$HOME/.config}/containers/systemd"
state_directory="${XDG_DATA_HOME:-$HOME/.local/share}/gaudere/state"

install -d -m 0700 "$quadlet_directory" "$state_directory"
install -m 0600 deploy/quadlet/gaudere-agent.container \
    "$quadlet_directory/gaudere-agent.container"

systemctl --user daemon-reload

echo "Installed gaudere-agent.container"
echo "Start with: systemctl --user start gaudere-agent.service"
echo "Inspect with: journalctl --user -u gaudere-agent.service"
