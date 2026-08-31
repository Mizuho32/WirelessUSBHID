if [ -z "$1" ]; then
  ro-env $PWD /tmp $HOME/.cache $HOME/.claude{,.json,.json.lock} $HOME/.espressif -- /usr/bin/bash -lic 'claude'
else
  ro-env $PWD /tmp $HOME/.cache $HOME/.claude{,.json,.json.lock} $HOME/.espressif -- /usr/bin/bash -lic "claude --resume $1"
fi
