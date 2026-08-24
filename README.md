# EcoFloat

An autonomous catamaran that maps dissolved oxygen and temperature across a lake and hands back a georeferenced picture of where the water is actually in trouble.

Traditional monitoring pulls a handful of point samples and calls it a lake. EcoFloat runs a GPS waypoint mission, logs DO and temperature continuously along the track, and turns that into a spatial map. On its first real survey it found two zones that point sampling would have missed entirely.

---

## Field results — Deep Quarry Lake, July 29 2026

Deep Quarry Lake, West Branch Forest Preserve, DuPage County IL (~40 acres, ~45 ft max depth). Run under a research permit from the Forest Preserve District of DuPage County.

- **2,862** georeferenced DO + temperature readings in a single morning session
- **Low-DO zone** along the northeastern shore, down to **4.72 mg/L**
- **Supersaturation zone** along the western shore, up to **9.75 mg/L**

That's a ~5 mg/L spread across one small lake. A single grab sample anywhere in the middle reports none of it.

---

## How it works

**Navigation** — ArduRover 4.6.3 on a CoreWing F405 Wing V2. Differential skid steer from two ApisQueen U2 12V thrusters driven by DH 40A ESCs. Missions upload as GPS waypoints and run in Auto with no operator input.

**Sensor pod** — ESP32-S3 (Heltec WiFi LoRa 32 V3) polls a DFRobot SEN0680 optical DO sensor over RS485/Modbus, stamps each reading with a fix from a BZ-251 GPS, writes to a 3,000-reading circular buffer on device, and streams telemetry to shore over LoRa.

**Shore station** — a second Heltec V3 receives the LoRa stream and serves a WiFi access point with a Leaflet map: live DO-colored breadcrumbs over satellite imagery, CSV download, and KML mission upload to LittleFS.

**Hull** — custom composite fiberglass layup, twin hulls on a cross-tube bracket system, one 3S LiPo per hull wired in parallel. High-visibility orange and white, because a lost boat 200m out is a bad afternoon.

---

## Post-processing

`ecofloat_to_kml.py` turns logged CSVs into a Google Earth layer:

```bash
python3 ecofloat_to_kml.py
```

It auto-discovers CSVs in `~/Downloads/Ecofloat data`, filters GPS jitter and zero-length segments from stationary fixes, and writes KML with color-coded track segments plus a filled disc polygon per reading.

Open the output in **Google Earth Pro (desktop)**. The web version rejects files past its geometry count limit.

---

## Repo layout

```
firmware/
  sensor-pod/        ESP32-S3 sensor + GPS + LoRa TX
  base-station/      Heltec V3 receiver, web server, Leaflet dashboard
analysis/
  ecofloat_to_kml.py CSV to Google Earth KML
missions/            KML waypoint mission files
data/                Survey CSVs
docs/                Build notes and field photos
```

---

## Notes from the build

Things that cost real time, recorded so they don't cost anyone else's:

- **Calibrate the compass in its final mounted position.** A bad or missing calibration means Auto mode picks a heading at random the instant you switch into it.
- **Never connect the red BEC wire from an ESC servo lead to the flight controller servo rail.** It backfeeds 5V and causes a power surge. Signal and ground only.
- **DO below ~3.5 mg/L usually means the sensor is out of the water**, not that the lake is dead — that reading is physically impossible in an aerated lake. It's the most reliable air-exposure flag for cleaning merged datasets.
- GPS TX/RX crossover, tube OD tolerances, and hull-to-hull variation in a hand layup all bit at least once. Measure before you print.

---

## Roadmap

- Weekly repeat surveys — Sept 5, 12, 19, 26 and an Oct 10 turnover follow-up, same mission file, same morning window, weather covariates logged each run
- Expansion to Herrick Lake and Rice Lake, pending Forest Preserve coordination
- Additional parameters under evaluation: pH, turbidity, conductivity
- Interpolated heat maps (`scipy.griddata`) plus statistical analysis — ANOVA across zones, regression of DO against temperature and distance from shore

---

## Acknowledgments

Dan Grigas, Fisheries Ecologist at the Forest Preserve District of DuPage County, for the research permit, site access, and ongoing collaboration.

## License

TBD — see repository owner before reuse.
