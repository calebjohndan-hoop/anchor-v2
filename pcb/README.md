# PCB (KiCad)

Three KiCad projects go here:

| Folder | Board |
|---|---|
| `main/` | Main board: ESP32-S3-WROOM-1-N16R8, 2 x MAX98357A, TP4056, AP2112K-3.3, USB-C |
| `mic/` | Mic board: ICS-43434, 0.1 µF, JST-SH 5-pin, L/R jumper, sound port hole |
| `hub/` | Right cup hub: 8-wire cable pads, 2 mic sockets, speaker pads |

Nets, pin map and layout rules are in [../docs/DESIGN.md](../docs/DESIGN.md) sections 6 to 8.

Before ordering, check:
- [ ] ERC and DRC clean
- [ ] Mic sound port hole matches the ICS-43434 datasheet
- [ ] MAX98357A SD_MODE resistor values checked against the datasheet
- [ ] No copper under the ESP32-S3 antenna
- [ ] JLCPCB BOM and CPL files exported, parts in stock
