# mosquitto setup

3 files here:
- `mosquitto.conf` — the broker config (2 listeners: `1883` plain MQTT for local backend, `1773` websockets for devices via Cloudflare Tunnel).
- `sijagakali.pwfile.txt` — password file backup (hashed, not plaintext — safe to keep, `mosquitto_passwd` uses bcrypt/PBKDF2).
- `sijagakali.acl.txt` — ACL file backup (no secrets, just topic rules).

`mosquitto.conf` currently points at these two files under `C:\Program Files\mosquitto\` (Windows). On Linux, edit its `password_file`/`acl_file` lines to the Linux paths used below.

## Windows

Password/ACL files live at `C:\Program Files\mosquitto\sijagakali.pwfile` / `sijagakali.acl` (matches `mosquitto.conf` as committed).

```powershell
# Create the password file (first user needs -c; every user after must NOT have -c, or it wipes the file)
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -c -b "C:\Program Files\mosquitto\sijagakali.pwfile" sijagakali-backend "your-backend-password"
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -b "C:\Program Files\mosquitto\sijagakali.pwfile" node-001 "your-device-password"

# Copy sijagakali.acl.txt's content into:
notepad "C:\Program Files\mosquitto\sijagakali.acl"

# Point the mosquitto service at this repo's config, then restart (needs an elevated/Admin PowerShell)
sc.exe config mosquitto binPath= "\"C:\Program Files\mosquitto\mosquitto.exe\" run -c \"<path-to-this-repo>\mosquitto-conf\mosquitto.conf\""
Restart-Service mosquitto
```

Verify: `Get-Content "C:\Program Files\mosquitto\mosquitto.log"` (or wherever the service logs) should show `Opening ipv4 listen socket on port 1883` and `Opening websockets listen socket on port 1773`.

## Linux

Install mosquitto + its CLI tools first:

```bash
sudo apt install mosquitto mosquitto-clients   # Debian/Ubuntu
# sudo dnf install mosquitto mosquitto-clients # Fedora/RHEL
```

Password/ACL files conventionally live under `/etc/mosquitto/`:

```bash
sudo mosquitto_passwd -c -b /etc/mosquitto/sijagakali.pwfile sijagakali-backend "your-backend-password"
sudo mosquitto_passwd -b /etc/mosquitto/sijagakali.pwfile node-001 "your-device-password"

sudo cp sijagakali.acl.txt /etc/mosquitto/sijagakali.acl
```

Copy this repo's `mosquitto.conf` in (or `include_dir` it), fixing the two Windows paths to Linux ones:

```bash
sudo cp mosquitto.conf /etc/mosquitto/conf.d/sijagakali.conf
sudo sed -i \
  -e 's#C:\\Program Files\\mosquitto\\sijagakali.pwfile#/etc/mosquitto/sijagakali.pwfile#' \
  -e 's#C:\\Program Files\\mosquitto\\sijagakali.acl#/etc/mosquitto/sijagakali.acl#' \
  /etc/mosquitto/conf.d/sijagakali.conf

sudo systemctl restart mosquitto
sudo journalctl -u mosquitto -f   # verify: same "Opening ipv4/websockets listen socket" lines
```

## Adding a device or resetting a password (either OS)

```
mosquitto_passwd -b <pwfile-path> <device-id> "new-password"
```
No `-c` flag, and no ACL edit needed — `pattern readwrite sijagakali/%u/#` in `sijagakali.acl` already scopes any username to its own `sijagakali/<username>/#` topics automatically. The username must match the device's `DEVICE_ID` in firmware / `esp32-dummy`'s `DEVICE_ID` env var exactly.
