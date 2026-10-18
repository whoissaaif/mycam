# MyCam on Linux (preview)

MyCam turns your phone into a webcam for Zoom, OBS, Discord and browsers. On Linux it runs in the
background and shows up as a camera called **MyCam**.

**Status:** a preview. The iPhone (over Wi-Fi) is tested on Ubuntu 24.04. Android phones (USB and Wi-Fi)
are built in but not tested on Linux yet. There is no settings window yet: you use a terminal for the few
things that need one.

## Install

Download `mycam_<version>_amd64.deb`, then:

```
sudo apt install ./mycam_<version>_amd64.deb
systemctl --user start mycam
```

MyCam now starts by itself every time you log in. The install also:

- sets up the camera device (`/dev/video10`, named "MyCam") and loads it at every boot;
- lets MyCam use phones plugged in over USB, with no `sudo`.

If the install says **v4l2loopback is missing**, install it and reboot:

```
sudo apt install v4l2loopback-dkms
```

## Connect an iPhone (or iPad)

The iPhone and the PC must be on the same network.

1. In a terminal: `mycam-companion --pair`. A QR code appears.
2. Point the iPhone's camera at it and open the link.
3. Safari warns that the connection isn't private. That is expected (MyCam makes its own certificate):
   tap **Show Details**, then **visit this website**.
4. Allow the camera. The terminal says "Paired with ..." and you can close it.

Next time, just open the page again on the iPhone: no QR code needed. Tip: in Safari, tap Share →
**Add to Home Screen**, so clearing Safari's history doesn't make the iPhone forget the PC.

## Use it

Pick **MyCam** as the camera in your app. While no phone is connected it shows "Waiting for the phone".
The phone's camera only runs while an app is showing MyCam, and turns off about 4 seconds after the last
one closes it.

**OBS:** right-click the MyCam source → **Properties**, scroll down, and untick **Use Buffering**. With
buffering on, the picture runs noticeably behind.

## If something doesn't work

| Problem | Fix |
|---|---|
| OBS crashes when you add a camera | Ubuntu 24.04's own OBS (30.0.2) crashes on any virtual camera. Use the OBS team's build: `sudo add-apt-repository ppa:obsproject/obs-studio && sudo apt install obs-studio`. |
| MyCam isn't in the app's camera list | Check MyCam runs: `systemctl --user status mycam`. Then check the camera exists: `v4l2-ctl --list-devices` should list "MyCam". If not, reboot once after installing. |
| The install says v4l2loopback is already loaded without MyCam's device | Another program (OBS's virtual camera, say) loaded it with its own settings. Reboot, and MyCam's settings apply. |
| The iPhone can't open the page | A firewall may block port 47900: `sudo ufw allow 47900/tcp`. Also check both are on the same network. |
| `mycam-companion --pair` says MyCam isn't running | `systemctl --user start mycam` |
| Anything else | The log: `journalctl --user -u mycam -f`. It never contains pairing codes. |

## Uninstall

```
sudo apt remove mycam
```

## For developers

Building from source, the architecture and the tests: [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md).
The daemon can also run by hand: `mycam-companion --daemon [--device /dev/videoN] [--size WxH] [--pair] [--wifi]`
(stop the service first: `systemctl --user stop mycam`). When reinstalling a `.deb` of the same version,
use `sudo apt install --reinstall`, or apt keeps the old one.
