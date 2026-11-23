# PCBs (KiCad 7)

| Board | Size | Qty | Folder |
|---|---|---|---|
| Main board (left cup): ESP32-S3-WROOM-1-N16R8, left MAX98357A, MCP73831 charger, AP2112K-3.3, USB-C | 51 x 38 mm | 1 per headset | [main/](main/) |
| Mic board: ICS-43434, JST-SH 5, sound port hole; REF and ERR builds | 12 x 13 mm | 2 REF + 2 ERR per headset | [mic/](mic/) |
| Right cup hub: right MAX98357A, 8-pin cable in, 2 mic sockets, speaker socket | 30 x 22 mm | 1 per headset | [hub/](hub/) |

Each folder has the schematic (`.kicad_sch`, plus a PDF), the board (`.kicad_pcb`), the DRC report, front and back images, and `fab/` with Gerbers, drill files, `positions.csv` and `bom-jlcpcb.csv` for assembly.

## How these were made

The boards are generated from code in [gen/](gen/): `boards.py` lists every part and connection, `place.py` sets the placement, and `run_all.py` builds the schematics and boards, routes them with Freerouting, adds ground pours, runs DRC and exports the fab files. KiCad reads the schematics back and the script checks every net matches the parts list.

Schematics are drawn with net labels (parts joined by labels with the same name) rather than wires.

## DRC status

All three boards: no clearance, unconnected, short or courtyard errors. Remaining items are silkscreen text overlaps (cosmetic) and "footprint not in project library" notices, because the boards use KiCad's standard libraries.

## Design notes

- **Radio is not used.** The ESP32-S3 antenna keepout and its large courtyard were removed so parts and ground pour can sit near the module. If Wi-Fi or Bluetooth is ever needed, the board must be re-laid out with the keepout back.
- **Mic boards: one PCB, two builds.** The REF build fits R1 (L/R to GND, left slot) and the ERR build fits R2 (L/R to 3.3 V, right slot), exactly like v1's INMP441 wiring. Order the mic PCB twice at JLCPCB, once with `fab/bom-jlcpcb-REF.csv` and once with `fab/bom-jlcpcb-ERR.csv`. Nothing to bridge by hand.
- **Mic GND ring:** the ICS-43434 GND pad is a ring around the sound port; a short trace was added by hand because the autorouter skips that pad shape. Loose copper rings in the library footprint were removed.
- **Charge status:** MCP73831 STAT swings to 5 V, so it reaches GPIO 2 through a 10k/20k divider.
- **Each ear is its own I2S port, wired like v1:** left on GPIO 5, 6, 7, 15 (v1 pins), right on GPIO 17, 18, 21, 16. Both amps get SD_MODE through 10 kΩ from GPIO 8, which has a 10 kΩ pull-up, so the amps are on by default and v1 firmware works without changes. The firmware writes the same sound to both slots, as in v1.
- **Speakers** connect with 2-pin JST-SH, the battery with 2-pin JST-PH (most LiPo packs come with PH).
- **Right amp on the hub:** the headband cable carries only the right ear's digital I2S port (pin 1 to 8: 3V3, GND, BCLK, WS, DIN, GND, DOUT, AMP_SD). Class D speaker outputs never run beside the mic lines. The hub amp runs from 3.3 V through a ferrite bead.
- **Reverse battery protection:** AO3401A P-MOSFET in the battery line (gate to GND).
- **Power switch** drives only the regulator EN pin (100 kΩ pull-down), so its 300 mA rating is not an issue.
- **Cable:** the 8-pin JST-SH cable must be wired pin 1 to pin 1. Pre-made SH cables are sold both straight and reversed; check before plugging in.

## Check before ordering

This is an autorouted first revision and has not been built.

- [ ] Open each board in KiCad 7 or 8 and run DRC and ERC yourself
- [ ] Check the ICS-43434 footprint and sound port against the TDK datasheet
- [ ] Check the MAX98357A SD_MODE voltage window for "left/both" (about 2.5 V here) in the datasheet
- [ ] Look over the autorouted traces and add GND stitching vias if the pours look broken up
- [ ] Run ERC in KiCad (the command-line KiCad 7 used here cannot)
- [ ] Check the battery's plug polarity matches J2 (pin 1 = +) before connecting; the MOSFET protects against a reversed pack, but check anyway
- [ ] Confirm every part in `bom-jlcpcb.csv` is in stock at JLCPCB, then fill in the LCSC part numbers
- [ ] Measure the earmuff and confirm the board outlines fit the cup trays in [../cad/](../cad/)
