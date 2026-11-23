# PCBs (KiCad 7)

| Board | Size | Qty | Folder |
|---|---|---|---|
| Main board (left cup): ESP32-S3-WROOM-1-N16R8, 2 x MAX98357A, MCP73831 charger, AP2112K-3.3, USB-C | 51 x 38 mm | 1 per headset | [main/](main/) |
| Mic board: ICS-43434, JST-SH 5, L/R solder jumper, sound port hole | 12 x 13 mm | 4 per headset | [mic/](mic/) |
| Right cup hub: 8-pin cable in, 2 mic sockets, speaker socket | 30 x 22 mm | 1 per headset | [hub/](hub/) |

Each folder has the schematic (`.kicad_sch`, plus a PDF), the board (`.kicad_pcb`), the DRC report, front and back images, and `fab/` with Gerbers, drill files, `positions.csv` and `bom-jlcpcb.csv` for assembly.

## How these were made

The boards are generated from code in [gen/](gen/): `boards.py` lists every part and connection, `place.py` sets the placement, and `run_all.py` builds the schematics and boards, routes them with Freerouting, adds ground pours, runs DRC and exports the fab files. KiCad reads the schematics back and the script checks every net matches the parts list.

Schematics are drawn with net labels (parts joined by labels with the same name) rather than wires.

## DRC status

All three boards: no clearance, unconnected, short or courtyard errors. Remaining items are silkscreen text overlaps (cosmetic) and "footprint not in project library" notices, because the boards use KiCad's standard libraries.

## Design notes

- **Radio is not used.** The ESP32-S3 antenna keepout and its large courtyard were removed so parts and ground pour can sit near the module. If Wi-Fi or Bluetooth is ever needed, the board must be re-laid out with the keepout back.
- **Mic L/R jumper ships open.** Bridge pads 1-2 with solder for a left-slot mic (REF L, ERR L) and 2-3 for a right-slot mic (REF R, ERR R). Each headset needs two of each.
- **Mic GND ring:** the ICS-43434 GND pad is a ring around the sound port; a short trace was added by hand because the autorouter skips that pad shape. Loose copper rings in the library footprint were removed.
- **Charge status:** MCP73831 STAT swings to 5 V, so it reaches GPIO 2 through a 10k/20k divider.
- **Amp channels:** SD_MODE through 10 kΩ selects left and through 220 kΩ selects right, assuming the MAX98357A's internal 100 kΩ pull-down.
- **Speakers** connect with 2-pin JST-SH, the battery with 2-pin JST-PH (most LiPo packs come with PH).

## Check before ordering

This is an autorouted first revision and has not been built.

- [ ] Open each board in KiCad 7 or 8 and run DRC and ERC yourself
- [ ] Check the ICS-43434 footprint and sound port against the TDK datasheet
- [ ] Check the MAX98357A SD_MODE voltage windows in the datasheet
- [ ] Look over the autorouted traces: keep amp outputs and I2S clocks short and away from each other, and add GND stitching vias if the pours look broken up
- [ ] Confirm every part in `bom-jlcpcb.csv` is in stock at JLCPCB, then fill in the LCSC part numbers
- [ ] Measure the earmuff and confirm the board outlines fit the cup trays in [../cad/](../cad/)
