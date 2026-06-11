# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a C-based sailboat polar diagram generator that processes sailing performance data from NMEA0183 log files and qtVlm VDR SQLite databases to create performance polar diagrams. The tool aggregates True Wind Angle (TWA), True Wind Speed (TWS), and Boat Speed (BSP/STW) measurements into a grid-based polar diagram.

## Build and Run

```bash
# Compile
gcc -o polar_generator polar_generator.c -lm -lsqlite3

# Basic usage - create new polar
./polar_generator -o polar.pol input.nmea

# Process VDR database
./polar_generator -o polar.pol trace.db

# Update existing polar with new data (only keeps equal/better performance)
./polar_generator -i existing.pol -o updated.pol new_data.nmea another_trace.db

# Verbose mode
./polar_generator -v -o polar.pol input.nmea
```

## Architecture

### Data Processing Pipeline

1. **Input parsing**: Two input formats supported
   - NMEA0183 files: Parses MWV (wind) + VHW (speed) sentence pairs with checksum validation.
     Fields are split preserving empty fields (fixed positions) — `nmea_split()` / `nmea_field_num()`.
     SOG is read from RMC/VTG/VBW/RMA/OSD sentences (`parse_sog_sentence()`) when present.
   - VDR SQLite databases: Queries the VDR table for TWA/TWS/STW, plus SOG/RPM/COMMENT/TIME when present.

2. **Denoising / filtering**
   - NMEA STW sliding-window smoothing (`nmea_smoother_t`), reset across maneuvers (large TWA jump).
   - STW vs SOG debounce (`stw_sog_filter_t`, shared by NMEA and VDR): tracks the slowly-varying
     STW−SOG offset (EMA) and rejects abrupt log glitches (hull lifting, blocked paddle wheel).
   - Engine filter (VDR): a point with `RPM > 0` is excluded, EXCEPT while the `COMMENT` holds the
     `Charge` keyword (engine in neutral charging batteries); state runs until the next `RPM = 0`.

3. **Grid bucketing**: Data points are rounded into discrete buckets
   - TWA buckets: 5° increments (0-180°)
   - TWS buckets: 2 knot increments
   - Multiple raw measurements collected per bucket using linked lists

4. **Statistical aggregation**: `aggregate_cell()` (used by `get_polar_value()` and `compute_polar()`)
   - Requires minimum 3 data points per cell
   - Keeps a configurable percentile of boat speed (`g_polar_percentile`, default P90, range P85–P95)
     to target achievable performance rather than the average; the high percentile naturally drops
     the low tail (luffing, tacks). Replaces the former trimmed mean.
   - Results cached in `cached_polar` array

5. **Output format**: Tab-separated values (.pol file)
   - Header row: TWS values
   - Data rows: TWA followed by BSP values for each TWS

### Update Mode (`-i` flag)

When loading an existing polar with `-i`, new data points are filtered: only measurements within 95% or better of the existing polar value are added. This prevents performance degradation from poor sailing conditions while allowing improvements.

### Key Data Structures

- `polar_grid_t`: Main container with `MAX_ANGLES × MAX_SPEEDS` array of linked lists (`data_point_t`)
- `nmea_data_t`: Temporary state for NMEA sentence parsing (waits for MWV+VHW pair; also carries the latest SOG)
- Constants: `MAX_ANGLES=181`, `MAX_SPEEDS=100`, `ANGLE_STEP=5`, `SPEED_STEP=2`

### Data Validation

- TWA: 0-180° (absolute value taken)
- TWS: 0.1-70 knots
- BSP/STW: 0.1-50 knots
- NMEA checksums validated before parsing

## Sample Test Files

The repository includes test data under `Test/`:
- SQLite VDR databases from sailing passages (e.g., `Horta-SantaCruz.db`, `SantaCruz-Mindelo.db`) —
  simulation-mode recordings, clean by construction (good for non-regression checks).
- `Comments.db` — exercises the `COMMENT`/`RPM` columns (engine `Charge` keyword, sea-state/sail tags).
- `Hakefjord.nmea` — a real NMEA0183 log (RMC/RMA/VHW…, no wind sentences) for STW/SOG parsing tests.
