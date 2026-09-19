# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Polar Doctor builds sailboat performance polars (boat speed for each true wind angle / true
wind speed) from real sailing data: NMEA0183 logs, qtVlm VDR SQLite databases, or live
capture. Since 2.0 it is a **web application**: a C server (`polar_doctor_web`) embeds its
single-page UI (HTML/CSS/JS/Canvas) and is used from any browser. There is no GUI toolkit;
the only dependencies are **glib-2.0** and **sqlite3**.

## Build and Run

```bash
make                                   # -> polar_doctor_web (Windows/MSYS2: polar_doctor_web.exe)
./polar_doctor_web ~/Boat --port 18081 # bind 127.0.0.1 by default, no password needed
sudo make install                      # binary + systemd service (see web/polar_doctor_web.service)
```

- Default port **8081** (8080 is n2k-mux-web on the target machine). Test on another port
  and never kill a process you did not start.
- Listening on a non-loopback address requires credentials (`WEB_AUTH=user:pass`) or
  `--allow-anonymous`. Settings precedence: command line > environment > `web.conf`
  (`g_get_user_config_dir()/polar_doctor/web.conf`, i.e. `%LOCALAPPDATA%` on Windows where a
  commented template is created on first run; `--config FILE`; must be 0600 on POSIX).
- The UI JavaScript lives in C string literals in `web/server.c`: after editing it, extract the
  `<script>` from the served page and run `node --check` on it. French text with apostrophes
  goes in backtick template literals (help) or uses `’`; `\n` inside JS strings is `\\n` in C.

## Source Layout

- `libpolar.h` / `libpolar.c` — core types, constants, globals and prototypes (no UI)
- `import.c` — NMEA + VDR parsing, smoothing, STW/SOG denoising, engine filter, percentile
  aggregation, polar grid
- `polar_data.c` — `PolarData` model: .pol load/save, interpolation, VMG optimal angles
- `boat_config.c` — boat inventory, polar definitions and routing, `boat.cfg` INI, recent boats
- `web/server.c` — HTTP server, JSON API, embedded UI, live capture, authentication,
  POSIX/Windows portability layer
- `web/polar_doctor_web.service`, `web/polar_doctor_web.default` — systemd unit and settings

## Architecture

### Server
Single process, single thread, one `poll()` loop over the HTTP listening socket, the live
capture socket and a ~1 s tick (VDR tail). Requests are handled sequentially
(accept → read → respond → close); state (loaded polar, boat, live grids) is in globals.
The UI polls `/api/live` every second (no SSE/WebSocket). Main endpoints: `/api/polar`,
`/api/curve`, `/api/boat`, `/api/boats`, `/api/open`, `/api/newboat`, `/api/select`,
`/api/config` (GET JSON / POST INI), `/api/save` (POST .pol text), `/api/import`,
`/api/percentile`, `/api/live*`. POSTs are validated by reloading a `.tmp` file before an
atomic replace (`replace_file()`: `rename` on POSIX, `MoveFileExW` on Windows).

Auth: login page + `pd_auth` cookie (HttpOnly, SameSite=Strict, 1 year) = HMAC-SHA256 of
`user:pass` with a server secret in `~/.config/polar_doctor/web_secret`; HTTP Basic is still
accepted for scripts. 401 responses carry no `WWW-Authenticate` (it would open the browser popup).

### Data Processing Pipeline

1. **Input parsing**
   - NMEA0183: fields are split preserving empty fields (fixed positions) — `nmea_split()` /
     `nmea_field_num()`. Wind: `MWV` with reference `T` (true, water-referenced) has priority;
     its 0–360° angle is folded to a 0–180° TWA. `MWD` + heading (`HDT`/`HDG`/`VHW`) is the
     fallback (ground-referenced, biased by current). `MWV,R` (apparent) is ignored. STW from
     `VHW`; SOG from RMC/VTG/VBW/RMA/OSD (`parse_sog_sentence()`).
   - NMEA 2000 as YDRAW text (`hh:mm:ss.ddd R <29-bit id> <bytes>`, n2k-mux TCP 2700):
     `parse_ydraw_line()` → `n2k_apply_frame()` fills the same `nmea_data_t` (130306 wind ref 4 =
     MWV,T priority, ref 3 / ref 0+heading fallback, apparent ignored; 128259 STW; 129026 SOG;
     127250 heading). All single-frame, no fast-packet. `parse_nav_line()` dispatches 0183 vs
     YDRAW per line — used by file import and live capture.
   - VDR SQLite: the `VDR` table's TWA/TWS/STW, plus SOG/RPM/COMMENT/TIME when present.
2. **Denoising / filtering**
   - NMEA STW sliding-window smoothing (`nmea_smoother_t`), reset across maneuvers.
   - STW vs SOG debounce (`stw_sog_filter_t`): tracks the slowly-varying STW−SOG offset and
     rejects abrupt log glitches.
   - Engine filter (VDR): `RPM > 0` excluded, except while the comment holds the charge
     keyword (engine in neutral); live capture uses the Engine button instead.
3. **Grid bucketing**: TWA 5° buckets, TWS 2 kn buckets; raw points kept in linked lists.
4. **Aggregation** (`aggregate_cell()`): minimum 3 points per cell; keeps percentile
   `g_polar_percentile` (default P90, P85–P95) to target achievable rather than average speed.
5. **Output**: `.pol`, semicolon-separated; header `TWA\TWS;0;<tws…>` — the **TWS 0 column is
   a sentinel**, always present and never displayed.

### Update mode
"Update" seeds the grid from the existing polar and adds the new data, then re-aggregates at
the percentile: the polar can go up or down (no "keep only better" filter since 1.3.0).

### Multi-polar routing
A boat folder holds `boat.cfg` and its `.pol` files. Each polar definition has criteria
(mainsail / headsail / sea state; empty = any). During live capture every point goes to all
matching polars (`polar_def_matches`, case-insensitive), each grid seeded from its `.pol`.

### Key Data Structures
- `polar_grid_t`: `PG_MAX_ANGLES (181) × PG_MAX_SPEEDS (100)` linked lists of `data_point_t`
- `PolarData`: display/edit model, `MAX_ANGLES (37) × MAX_SPEEDS (16)`
- `nmea_data_t`: sentence-assembly state (latest TWA/TWS/STW/SOG/TWD/heading, `has_mwv_true`)
- `BoatConfig` / `PolarDef`: inventory and polar definitions

### Data Validation
TWA 0–180°, TWS 0–70 kn, BSP/STW 0–50 kn; NMEA checksums validated before parsing.

## Sample Test Files

Under `Test/`:
- VDR databases from passages (e.g. `Horta-SantaCruz.db`, `SantaCruz-Mindelo.db`) — simulation
  recordings, clean by construction (good for non-regression checks).
- `Comments.db` — exercises the `COMMENT`/`RPM` columns (charge keyword, sail/sea tags).
- `Hakefjord.nmea` — a real NMEA0183 log (RMC/RMA/VHW…, no wind sentences) for STW/SOG tests.
- `n2k-mux-sim.ydraw` — 5 s of NMEA 2000 YDRAW from the n2k-mux simulator (apparent + true/water
  wind, STW, SOG, heading): 53 points.

Unit tests: `tests/*.c`, run with `make test` (link the core modules, not the server).
