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
   - NMEA0183 files: Parses MWV (wind) + VHW (speed) sentence pairs with checksum validation
   - VDR SQLite databases: Queries VDR table for TWA/TWS/STW columns

2. **Grid bucketing**: Data points are rounded into discrete buckets
   - TWA buckets: 5° increments (0-180°)
   - TWS buckets: 2 knot increments
   - Multiple raw measurements collected per bucket using linked lists

3. **Statistical aggregation**: `get_polar_value()` and `compute_polar()`
   - Requires minimum 3 data points per cell
   - 5+ points: Uses trimmed mean (removes top/bottom 20% outliers)
   - 3-4 points: Simple average
   - Results cached in `cached_polar` array

4. **Output format**: Tab-separated values (.pol file)
   - Header row: TWS values
   - Data rows: TWA followed by BSP values for each TWS

### Update Mode (`-i` flag)

When loading an existing polar with `-i`, new data points are filtered: only measurements within 95% or better of the existing polar value are added. This prevents performance degradation from poor sailing conditions while allowing improvements.

### Key Data Structures

- `polar_grid_t`: Main container with `MAX_ANGLES × MAX_SPEEDS` array of linked lists (`data_point_t`)
- `nmea_data_t`: Temporary state for NMEA sentence parsing (waits for MWV+VHW pair)
- Constants: `MAX_ANGLES=181`, `MAX_SPEEDS=100`, `ANGLE_STEP=5`, `SPEED_STEP=2`

### Data Validation

- TWA: 0-180° (absolute value taken)
- TWS: 0.1-50 knots
- BSP/STW: 0.1-20 knots
- NMEA checksums validated before parsing

## Sample VDR Database Files

The repository includes several SQLite VDR databases from sailing passages (e.g., Horta-SantaCruz.db, Mindelo-LeMarin.db) used for testing and development.
