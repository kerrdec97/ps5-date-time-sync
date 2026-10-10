# PS5 Time & Date Sync by Deckerr97

A lightweight, standalone PS5 payload that restores the console's system date
and time using public NTP servers.

It is intended for consoles with a depleted, disconnected, or faulty CMOS/RTC
battery where the clock resets after the PS5 is completely powered off.

## Download

Download the latest precompiled `ps5-date-time-sync.elf` from the
[Releases](https://github.com/kerrdec97/ps5-date-time-sync/releases/latest)
page.

## Features

- Automatically retrieves the current date and time from public NTP servers.
- Uses Cloudflare, `pool.ntp.org`, and Google Public NTP.
- Tries Cloudflare's published numeric NTP addresses first, allowing clock
  recovery even when DNS is unavailable on the PS5.
- Spreads retries across the addresses a hostname resolves to, and moves on
  immediately when a server refuses service (NTP "Kiss-o'-Death") or reports that it is not
  synchronized.
- Validates the reply endpoint, NTP version and mode, stratum, the server's
  own accuracy (root distance), a random per-request nonce, and the accepted
  date range before changing the clock.
- Compensates for network round-trip time and for the time spent between
  receiving the reply and setting the clock.
- Reads the clock back after the update and rejects failed verification.
- Preserves the console's existing timezone setting.
- Displays one notification per execution that states whether the sync
  worked:

  > Time synced: 2026-10-10 14:02:33 UTC<br>
  > PS5 time & date sync v1.1.0 by Deckerr97

- Exits automatically after synchronization.
- Does not install an application or modify files on the console.
- Is fully standalone and independent of other payloads.

## Usage

1. Connect the PS5 to the Internet.
2. Inject or launch `ps5-date-time-sync.elf` with a compatible payload loader.
3. Wait for the notification. It says either **Time synced** (with the new
   time in UTC) or **Time sync failed** with the reason.
4. The payload exits automatically.

If the clock resets after a complete shutdown, launch the payload once after
booting the console.

## Requirements

- A compatible PS5 exploit and ELF payload loader.
- An active Internet connection.
- Outbound UDP port 123 access for NTP.
- An execution environment that permits `settimeofday`.

The numeric Cloudflare fallbacks remove the initial dependency on DNS. If the
router, firewall, or Internet provider blocks UDP port 123, synchronization
will still fail.

## Compatibility

The CI build uses the PS5 Payload SDK release pinned in
[`.github/workflows/build.yml`](.github/workflows/build.yml).

This does not guarantee operation on every firmware. Runtime compatibility
depends on the exploit, ELF loader, firmware-specific restrictions, and
whether the active environment permits system-clock modification.

## Building

Install the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), set
`PS5_PAYLOAD_SDK` to its installation directory, and run:

```sh
make
```

The output will be:

```text
ps5-date-time-sync.elf
```

To send it through a normal ELF loader:

```sh
make deploy PS5_HOST=192.168.0.233
```

(`make test` does the same thing, following the PS5 Payload SDK convention.)

To upload and launch it through Payload Manager:

```sh
make manager-deploy PS5_HOST=192.168.0.233
```

Every push is also built by GitHub Actions with a pinned, checksum-verified
SDK. The build is reproducible, so the SHA-256 printed by the workflow (and
uploaded next to the ELF as a build artifact) can be compared against a
release download.

## Notification and logs

The on-screen notification shows the outcome:

| Notification | Meaning |
| --- | --- |
| Time synced: *date/time* UTC | The clock was set and verified. The time is shown in UTC; the PS5 still displays it in your configured timezone. |
| Time sync failed: no NTP server answered | No server replied. Check the Internet connection and that UDP port 123 is not blocked. |
| Time sync failed: network unavailable | The payload could not initialize networking. |
| Time sync failed: the clock could not be set | The loader's environment does not permit changing the system clock. |

Detailed information (each server tried, rejection reasons, timing
corrections) is written to the payload output.

If the notification appears more than once after a single intended launch,
the payload manager or ELF loader has most likely launched the ELF multiple
times.

## How it works

```text
Launch payload
      |
      v
Initialize network
      |
      v
Try numeric Cloudflare NTP addresses
      |
      +-- fall back to hostname-based providers if needed
      |   (retries go to a different address when one is available)
      |
      v
Request and validate NTP time
      |
      v
Update and verify the PS5 system clock
      |
      v
Write details to the payload output
      |
      v
Show one notification with the result and exit
```

## Acknowledgements

Special thanks to **John Törnblom** and every contributor to the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) for creating and
maintaining the toolchain that made this payload possible.

Thanks also to the public time-service providers used by the payload:
[Cloudflare Time Services](https://developers.cloudflare.com/time-services/ntp/),
the [NTP Pool Project](https://www.ntppool.org/), and
[Google Public NTP](https://developers.google.com/time).

## License

This project is licensed under the
[GNU General Public License v3.0 or later](LICENSE).

## Disclaimer

Use this software at your own risk. This project is not affiliated with or
endorsed by Sony Interactive Entertainment.

---

Created by **Deckerr97**.
