#!/usr/bin/env bash
set -euo pipefail
[[ "${GITHUB_ACTIONS:-}" == true ]] || { echo 'Linux install tests require isolated CI'; exit 1; }
package=$(realpath "$1")
fixture=$(mktemp -d "$RUNNER_TEMP/NordSSH-install.XXXXXX")
export XDG_DATA_HOME="$fixture/data" XDG_CONFIG_HOME="$fixture/config"
mkdir -p "$XDG_DATA_HOME/com.nordssh.desktop"
vault="$XDG_DATA_HOME/com.nordssh.desktop/vault.nordvault"
cp test/fixtures/windows-v1.json "$vault"
before=$(sha256sum "$vault" | cut -d' ' -f1)
# A synthetic older package version exercises dpkg's upgrade path; not published.
dpkg-deb -R "$package" "$fixture/baseline"
sed -i 's/^Version: .*/Version: 0.0.0/' "$fixture/baseline/DEBIAN/control"
dpkg-deb --build "$fixture/baseline" "$fixture/baseline.deb"
sudo dpkg -i "$fixture/baseline.deb"
sudo dpkg -i "$package"
xvfb-run -a bash test/linux-startup.sh /usr/bin/nordssh
[[ "$(sha256sum "$vault" | cut -d' ' -f1)" == "$before" ]]
sudo dpkg -i "$package"
[[ "$(sha256sum "$vault" | cut -d' ' -f1)" == "$before" ]]
sudo dpkg -r nordssh
[[ "$(sha256sum "$vault" | cut -d' ' -f1)" == "$before" ]]
echo 'PASS: Debian install, package-version upgrade, native startup, reinstall and uninstall preserve the encrypted vault.'
