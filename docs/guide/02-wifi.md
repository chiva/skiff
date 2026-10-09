# 2. Wi-Fi

The PSP's Wi-Fi was designed in 2004. Modern routers have dropped some of what it needs, so the
most common reason a PSP "can't connect" is a router setting, not the PSP.

## What the PSP can and cannot do

| | PSP |
|---|---|
| Band | 2.4 GHz only (no 5 GHz) |
| Standard | 802.11b only |
| Security | Open, WEP, WPA-PSK (TKIP and AES on later firmware) |
| WPA2-only / WPA3 | ❌ The PSP-1000 cannot join them; other models are unreliable |
| Captive portals (hotel / café login pages) | ❌ |

## The recommended setup: a dedicated network for old devices

Create a separate Wi-Fi network just for the PSP (and other old gadgets). Most routers call it a
**guest network** or **IoT network**. Set it to:

- **Band:** 2.4 GHz.
- **Security:** "WPA/WPA2 Personal" or "WPA/WPA2 mixed" (sometimes shown as "WPA-PSK + WPA2-PSK" or
  "TKIP+AES"). Not "WPA2 only", not "WPA3".
- **802.11 mode:** a mode that includes **b** (e.g. "b/g/n mixed"). Some routers hide this under
  "legacy rates" or "allow 802.11b".
- **Password:** long and random. Older security is weaker, so a strong password matters more here.

Why a separate network? WPA with TKIP is an old, weaker security mode. Keeping it on its own network
means your laptop and phone stay on modern security, and a weakness in the old mode cannot reach
them.

### Extra safety (optional, for people with a capable router)

If your router supports VLANs or firewall rules, allow the PSP's network to reach **only your RomM
server** and the internet, nothing else on your home network.

## Connect the PSP

1. Settings → Network Settings → Infrastructure Mode → New Connection.
2. Scan, choose your network, and enter the password. On the security screen pick **WPA-PSK
   (TKIP)** first; if that fails, try **WPA-PSK (AES)**.
3. Leave address settings on "Easy" unless you know you need otherwise.
4. Run **Test Connection** at the end.

✅ **Check:** the test shows an IP address and "Internet Connection: Succeeded". If RomM is on your
home network only, "Internet Connection: Failed" is fine as long as you get an IP address.

## Can't connect?

| Symptom | Likely cause | What to try |
|---|---|---|
| Network not in the scan list | 5 GHz only, or hidden network | Enable 2.4 GHz; type the name manually |
| "Connection timed out" during test | Router refuses 802.11b | Enable 802.11b / legacy rates |
| "The access point cannot be found" or auth failure | WPA2-only or WPA3 | Switch the network to WPA/WPA2 mixed |
| Worked yesterday, not today | Router firmware update reset the mode | Re-check the three settings above |
| Phone hotspot not working | Most phones offer WPA2/WPA3 only | Use a router guest network instead |

Next: [Install Skiff](03-install-skiff.md)
