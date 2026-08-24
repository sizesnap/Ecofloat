# EcoFloat

An autonomous catamaran that maps dissolved oxygen and temperature across a lake and hands back a georeferenced picture of where the water is actually in trouble.

<img width="1331" height="656" alt="download (1) (1)" src="https://github.com/user-attachments/assets/fe5ea701-1b95-4f7c-b329-738b5cb4c011" />

Traditional monitoring pulls a handful of point samples and calls it a lake. EcoFloat runs a GPS waypoint mission, logs DO and temperature continuously along the track, and turns that into a spatial map. On its first real survey it found two zones that point sampling would have missed entirely.

---

## Field results — Deep Quarry Lake, July 29 2026

Deep Quarry Lake, West Branch Forest Preserve, DuPage County IL (~40 acres, ~45 ft max depth). Run under a research permit from the Forest Preserve District of DuPage County.

- **2,862** georeferenced DO + temperature readings in a single morning session
- **Low-DO zone** along the northeastern shore, down to **4.72 mg/L**
- **Supersaturation zone** along the western shore, up to **9.75 mg/L**

That's a ~5 mg/L spread across one small lake. A single grab sample anywhere in the middle reports none of it.

<img width="3300" height="2550" alt="DeepQuarry lake Survey map 7_29_2026" src="https://github.com/user-attachments/assets/b86af440-b4c8-4384-88de-1601c8e968e1" />

---

## How it works

<img width="4032" height="3024" alt="IMG_3256" src="https://github.com/user-attachments/assets/8c12140f-e84d-426c-b3f0-3edebeba979a" />


**Navigation** — ArduRover 4.6.3 on a CoreWing F405 Wing V2. Differential skid steer from two ApisQueen U2 12V thrusters driven by DH 40A ESCs. Missions upload as GPS waypoints and run in Auto with no operator input.

**Sensor pod** — ESP32-S3 (Heltec WiFi LoRa 32 V3) polls a DFRobot SEN0680 optical DO sensor over RS485/Modbus, stamps each reading with a fix from a BZ-251 GPS, writes to a 3,000-reading circular buffer on device, and streams telemetry to shore over LoRa.

**Shore station** — a second Heltec V3 receives the LoRa stream and serves a WiFi access point with a Leaflet map: live DO-colored breadcrumbs over satellite imagery, CSV download, and KML mission upload to LittleFS.

<img width="1206" height="2147" alt="IMG_3137" src="https://github.com/user-attachments/assets/5bc26c1b-8190-4d1c-aeb1-bcb157877e95" />


**Hull** — custom composite fiberglass layup, twin hulls on a cross-tube bracket system, one 3S LiPo per hull wired in parallel. High-visibility orange and white, because a lost boat 200m out is a bad afternoon.



---

## Bill of materials

Full parts list with quantities, costs, vendor links, and build notes: **[`BOM.csv`](BOM.csv)**

Headline numbers — approximately **$753** total, of which the DFRobot SEN0680 optical DO sensor is $199. Everything else is off-the-shelf hobby-grade hardware or hardware-store consumables.

---|---|---|---|---|
| DFRobot SEN0680 RS485 fluorescence DO sensor (freshwater) | 1 | Dissolved oxygen + temperature, 0–20 mg/L, ModBus-RTU, IP68 | $199 | [DFRobot wiki](https://wiki.dfrobot.com/SKU_SEN0680_RS485_Dissolved_Oxygen_Sensor_Freshwater) · [DigiKey](https://www.digikey.com/en/products/detail/dfrobot/SEN0680/28531158) |
| DFRobot DFR0845 active isolated RS485↔UART adapter | 1 | Bridges the Modbus sensor to the ESP32; supplies 12V to the probe | ~$14 | DFRobot |
| Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262) | 2 | Boat sensor pod + shore base station | ~$25 ea | [Heltec](https://heltec.org/project/wifi-lora-32-v3/) · [Rokland](https://store.rokland.com/products/heltec-wifi-lora-32v3) |
| 915 MHz LoRa antenna | 2 | Telemetry link, boat ↔ shore | ~$6 ea | included with board |
| BZ-251 GPS module | 1 | Position fix for each reading | ~$20 | *add your order link* |
| ApisQueen U2 12V underwater thruster (CW + CCW) | 2 | Differential skid-steer propulsion, ~1.7 kg thrust each | ~$85 ea | [ApisQueen](https://www.underwaterthruster.com/products/apisqueen-u2-12v-150w-underwaterthruster-with-esc-5v-1a-bec-output-for-rov-boat) |
| DH 40A bidirectional ESC | 2 | Thruster drive, forward/reverse | ~$20 ea | *add your order link* |
| CoreWing F405 Wing V2 flight controller | 1 | Runs ArduRover 4.6.3, autonomous waypoint navigation | ~$45 | *add your order link* |
| 3S LiPo battery | 2 | One per hull, wired in parallel | ~$30 ea | *add your order link* |
| RadioMaster T8L transmitter + ELRS receiver | 1 | Manual override / failsafe control | ~$45 | *add your order link* |
| Fiberglass cloth + epoxy resin | — | Composite hull layup | ~$60 | local / hardware store |
| Aluminum cross tubes | 2 | Hull-to-hull structure | ~$15 | local / hardware store |
| 3D-printed brackets and electronics trays | — | Resin (Elegoo Saturn 4 Ultra) + FDM; STLs in `hardware/` | ~$10 in resin/filament | printed in-house |
| Marine paint, orange + white | — | High-visibility finish | ~$25 | local / hardware store |

Rough build total: **~$700**, with the DO sensor about a third of it.

---

## Repo layout

```
firmware/
  EcoFloat_Boat/        ESP32-S3 sensor + GPS + LoRa TX
  EcoFloat_Base/        Heltec V3 receiver, web server, Leaflet dashboard
hardware/               CAD sources, STEP, STL
analysis/
  ecofloat_to_kml.py    CSV to Google Earth KML
missions/               KML waypoint mission files
data/                   Survey CSVs
docs/                   Photos, diagrams, build notes
```

Each `.ino` sits in a folder of the same name — an Arduino requirement, so the sketches open cleanly straight from a clone.

---

## Post-processing

`ecofloat_to_kml.py` turns logged CSVs into a Google Earth layer:

```bash
python3 analysis/ecofloat_to_kml.py
```

It auto-discovers CSVs in `~/Downloads/Ecofloat data`, filters GPS jitter and zero-length segments from stationary fixes, and writes KML with color-coded track segments plus a filled disc polygon per reading.

Open the output in **Google Earth Pro (desktop)**. The web version rejects files past its geometry count limit.

---

## Notes from the build

Things that cost real time, recorded so they don't cost anyone else's:

- **Calibrate the compass in its final mounted position.** A bad or missing calibration means Auto mode picks a heading at random the instant you switch into it.
- **Never connect the red BEC wire from an ESC servo lead to the flight controller servo rail.** It backfeeds 5V and causes a power surge. Signal and ground only.
- **DO below ~3.5 mg/L usually means the sensor is out of the water**, not that the lake is dead — that reading is physically impossible in an aerated lake. It's the most reliable air-exposure flag for cleaning merged datasets.
- **The RS485 T/R lines are easy to reverse.** Swapped pins hold the line at ~1V instead of switching cleanly to ground, and the sensor returns zero bytes with no error message at all.
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
