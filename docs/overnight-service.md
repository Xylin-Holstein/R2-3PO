# Running R2-3PO overnight with systemd

The interactive launcher is intended for a desktop session. For an unattended overnight run, use the system service below. It keeps the existing core and worker threads alive without a terminal, starts at boot, restarts after a crash, and records output in the system journal. It uses the same databases as the desktop launcher, so **never run both at once**.

## One-time host preparation

Run these on the R2 Linux machine, not on GitHub:

1. Confirm the repository is deployed at `/home/x/R2_Home/R2` and the local FX assets remain at `/home/x/R2_Home/R2_sounds/FX`. The FX folder is a host directory; it is not expected to exist in this repository.
2. Confirm the dedicated Linux account exists: `id r2`.
3. Install required packages if missing:

   ```sh
   sudo apt-get install -y build-essential libcurl4-openssl-dev libsqlite3-dev libjson-c-dev libpulse-dev libfaketime curl python3
   ```

4. Start Ollama and ensure the exact local model is installed:

   ```sh
   sudo systemctl enable --now ollama
   ollama pull gemma3:4b
   curl -fsS http://127.0.0.1:11434/api/tags
   ```

   The service deliberately does not download models automatically. If Ollama is installed differently and has no system unit named `ollama.service`, start it using the method already used on that host; the runner waits for its local API.

5. Confirm the service account can traverse and read the workspace. Do not use broad `chmod 777` permissions:

   ```sh
   sudo -u r2 test -r /home/x/R2_Home/R2/r2.c && echo "R2 source readable"
   sudo -u r2 test -x /home/x/R2_Home && echo "workspace traversable"
   sudo -u r2 test -r /home/x/R2_Home/R2_sounds/FX && echo "FX directory readable"
   ```

   If a test fails, correct the existing owner/group/ACL deliberately before continuing.

## Build and activate

First stop any interactive R2 session. This matters because the service and interactive launcher share SQLite databases.

From a terminal on the R2 machine:

```sh
cd /home/x/R2_Home/R2

sudo apt-get install -y libfaketime
sudo bash -c '
set -e
cd /home/x/R2_Home/R2
gcc -std=c11 -Wall -Wextra -O2 \
  r2.c shell.c r2_diary.c Log.c Reality.c Addiction.c Reward.c AlternateSelf.c Visual.c Ears.c Eyes.c R2Sounds.c R2Reply.c \
  -o /home/x/R2_Home/r2 \
  -lcurl -lsqlite3 -lpthread -ljson-c -lpulse-simple -lpulse -lm
chown x:r2 /home/x/R2_Home/r2
chmod 0750 /home/x/R2_Home/r2
chmod 0755 /home/x/R2_Home/R2/tools/r2-3po-service.sh
install -o root -g root -m 0644 /home/x/R2_Home/R2/systemd/r2-3po.service /etc/systemd/system/r2-3po.service
systemctl daemon-reload
systemctl enable --now r2-3po.service
'
```

The unit's paths intentionally match the established host layout. If the Linux group for account `r2` is not literally named `r2`, replace `chown x:r2` with `chown x:$(id -gn r2)`.

## Check that he is actually running

```sh
systemctl is-enabled r2-3po.service
systemctl is-active r2-3po.service
systemctl --no-pager --full status r2-3po.service
journalctl -u r2-3po.service -n 100 --no-pager
```

Expected active state is `active`. The journal should report that the local Ollama API/model check passed and R2 entered headless overnight mode. If the service restarts repeatedly, inspect the journal; the most likely causes are a missing `gemma3:4b` model, inaccessible workspace files, missing `libfaketime`, or an Ollama service that is not running.

Follow live logs with `journalctl -u r2-3po.service -f`.

## Keep the computer awake

systemd can keep R2's process alive across crashes and reboots, but it cannot make computation continue while the physical computer is suspended or powered off. On a laptop, keep the power adapter connected and disable automatic suspend for the overnight period in the desktop's Power settings. Do not close the lid unless the lid action is configured not to suspend. A desktop machine should likewise have automatic suspend disabled for the night. R2's virtual clock is private to its process; the Linux system clock is not changed.

## Normal operation

- Start: `sudo systemctl start r2-3po.service`
- Stop cleanly: `sudo systemctl stop r2-3po.service`
- Restart: `sudo systemctl restart r2-3po.service`
- Status: `systemctl status r2-3po.service`
- Logs: `journalctl -u r2-3po.service -f`
- Disable automatic boot: `sudo systemctl disable r2-3po.service`

The service runs without an interactive shell, so it will not display a `R2>` prompt. Stop the service before using `R2_Launch_Code.sh). The launcher also checks whether the service is active to help prevent concurrent database writers.

The service runs the already-compiled executable. After source changes, rebuild it using the compile commands above, then restart the service. The installer does not create or change the local FX assets, alter database contents, or pull model weights.

## Verification boundary

CI can compile the source and check the scripts, but it cannot verify this host's Linux account permissions, Ollama installation/model, actual local database health, speaker/audio routing, or whether desktop power settings allow the machine to stay awake. After activation, check the service status and journal on the machine and perform one real speaker test. The separate read-only database and FX audits documented in the main project instructions remain host-side checks.
