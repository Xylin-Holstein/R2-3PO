# Running R2-3PO overnight with systemd

R2 already has a headless shell mode, a supervised systemd unit, a local-model preflight, a private 1999 clock, and a sleep-inhibitor attempt. Use the existing deployment files below; do not create a second service or run the interactive launcher alongside it.

- Runner: `tools/r2_service_runner.sh`
- Unit: `deploy/r2-3po.service`
- Installer: `tools/install_r2_overnight.sh`

The overnight service uses the same persistent databases as the desktop launcher. **Only one R2 core may run at a time.** The installer refuses to rebuild while an R2 process is already running, checks database paths and permissions, takes a private SQLite snapshot, requires a clean integrity audit, builds to a temporary executable, and only then enables/starts the service.

## One-time host preparation

Run these on the R2 Linux machine, not on GitHub.

1. Confirm the repository is deployed at `/home/x/R2_Home/R2`. The sound assets remain a separate local directory at `/home/x/R2_Home/R2_sounds/FX`; that directory is not supposed to exist in GitHub.
2. Confirm the dedicated Linux account exists: `id r2`.
3. Install the required build and runtime packages if missing:

   ```sh
   sudo apt-get install -y build-essential libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libpulse-dev libfaketime curl python3 ffmpeg vlc python3-vlc xdotool pulseaudio-utils python3-tk mpg123
   ```

   The R2 launcher and installer do not run `apt-get update` automatically.

4. Start Ollama and ensure the exact local model is installed:

   ```sh
   sudo systemctl enable --now ollama
   ollama pull gemma3:4b
   curl -fsS http://127.0.0.1:11434/api/tags
   ```

   If Ollama is installed without a system unit named `ollama.service`, start it using the host's existing method. The R2 runner waits up to 60 seconds for the local API to provide `gemma3:4b`; it does not download model weights or contact an external API.

5. Confirm the service account can access the existing workspace and local FX assets. Do not solve permission problems with broad `chmod 777` permissions:

   ```sh
   sudo -u r2 -- sh -c 'test -x /home/x/R2_Home' && echo "workspace traversable"
   sudo -u r2 -- sh -c 'test -r /home/x/R2_Home/R2/r2.c' && echo "source readable"
   sudo -u r2 -- sh -c 'probe=$(mktemp /home/x/R2_Home/R2/.r2-write-check.XXXXXX) && rm -f -- "$probe"' && echo "persistent data directory writable"
   sudo -u r2 -- sh -c 'test -r /home/x/R2_Home/R2_sounds/FX' && echo "FX directory readable"
   ```

   If a check fails, correct the existing owner/group/ACL deliberately before continuing. The core stores its five SQLite databases under `R2/` (`r2_memory.db`, `r2_reality.db`, `r2_fridge.db`, `r2_rewards.db`, and `r2_addictions.db`), so account `r2` needs read/write access to that directory and its existing database files.

## Build, audit, and activate

First exit any interactive R2 session. The service and interactive launcher share persistent SQLite databases.

Run the existing installer:

```sh
sudo bash /home/x/R2_Home/R2/tools/install_r2_overnight.sh
```

It will:
- verify `r2`, build tools, `libfaketime`, and the expected deployed files;
- refuse to run beside an already active R2 process;
- check permissions and refuse symlinked database paths;
- create a private snapshot of the memory, Reality, fridge, reward, and addiction databases and require the integrity audit to pass;
- compile the full C runtime to a temporary executable so a failed build does not replace the prior executable;
- install the systemd unit, reload systemd, and enable/start the overnight service.

The installer intentionally stops if the database audit fails. Preserve the report and snapshot and investigate the issue rather than deleting or replacing the database files.

## Verify that R2 is actually active

```sh
systemctl is-enabled r2-3po.service
systemctl is-active r2-3po.service
systemctl --no-pager --full status r2-3po.service
journalctl -u r2-3po.service -n 100 --no-pager
```

Expected state is `enabled` and `active`. The journal should report that the local Ollama model check passed and R2 entered service mode. If the unit keeps restarting, inspect the logs; common causes are a missing `gemma3:4b` model, missing `libfaketime`, inaccessible workspace/database files, or Ollama not running.

Follow live logs with `journalctl -u r2-3po.service -f`.

## Keep the physical computer awake

The runner attempts to acquire a system sleep inhibitor before doing startup checks. If the journal says sleep inhibition is unavailable, disable automatic suspend for the overnight period in the desktop's Power settings, keep the power adapter connected, and do not close the laptop lid unless its action is configured not to suspend. A powered-off or suspended computer cannot continue running R2. The private clock changes only R2's process time; it does not change Linux's system clock.

## Normal operation

- Start: `sudo systemctl start r2-3po.service`
- Stop cleanly: `sudo systemctl stop r2-3po.service`
- Restart: `sudo systemctl restart r2-3po.service`
- Status: `systemctl status r2-3po.service`
- Logs: `journalctl -u r2-3po.service -f`
- Disable automatic boot: `sudo systemctl disable r2-3po.service`

The service intentionally runs without an interactive `R2>` prompt. Stop the service before launching `R2_Launch_Code.sh`; the desktop launcher checks whether the service is active to help prevent concurrent database writers.

After future source changes, stop R2 and rerun the installer so the executable is rebuilt, the database audit is repeated, and the service is restarted safely. The installer does not create or rename the local FX assets.

## Verification boundary

CI can compile the source and statically check the supervisor wiring, but it cannot verify the host's Linux permissions, installed Ollama model, actual SQLite contents, speaker/audio routing, or desktop power settings. After activation, inspect the status and journal on the host and perform a real speaker test. The FX collection is a local runtime asset, not a repository folder.
