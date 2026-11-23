# ANCHOR v2 design plan

ANCHOR is a noise cancelling earmuff for factory and construction workers. It cancels steady machine noise and lets alarms and voices through. v1 was a low cost proof of concept (about ₹2,200) built on a breadboard. This document is the plan for v2: a two-ear build on custom PCBs, with the electronics inside the ear cups and its own battery.

Status: **design stage.** Nothing in this document has been built yet. Each "target" below is a goal to be measured, not a result.

---

## 1. What v1 taught me

| Area | v1 (built) | Problem found |
|---|---|---|
| Ears | One ear cup does ANC (mono) | Only one ear is protected by cancelling |
| Wiring | Breadboard and about 30 jumper wires on the headband | Bulky, connections come loose when the headband flexes |
| Electronics | ESP32-S3 DevKit, INMP441 breakouts, MAX98357A breakout | Boards sit outside the cups |
| Power | USB power bank on a cable | Not wearable for a shift; DevKit 5 V pin read about 0.05 V, so the amp had to run from 3V3 |
| Delay | Mic to speaker delay measured at 3.5 to 4.5 ms | Too slow for anything but steady, repeating hum |
| Cancelling | Works on steady machine hum | Never measured in dB |
| Sound classifier | Four classes: machine, voice, alarm, other | Accuracy on the headset itself was never measured |

## 2. v2 requirements

| # | Requirement | v2 target | How it is checked |
|---|---|---|---|
| R1 | Cancelling in both ears | Two independent FxLMS channels | Test T2 on each ear |
| R2 | Lower delay | Under 1 ms mic to speaker | Test T1, same method as v1 |
| R3 | Measured noise reduction | Report the real number, whatever it is | Test T2 with a sound level meter |
| R4 | Alarms and voices get through | Alarm amplified within 1.5 s in at least 8 of 10 trials | Test T3, T4 |
| R5 | All electronics inside the cups | Nothing on the headband except one cable | Visual check |
| R6 | Runs on its own battery | Full 8 hour shift on one charge | Test T5 |
| R7 | Charges over USB-C | Charges while worn or off | Test T5 |
| R8 | Fails safe | If the electronics stop, it is still a passive earmuff; output level is capped in firmware | Test T6 |
| R9 | Repairable | Mics, speakers and battery on connectors, not soldered to the main board | Visual check |

## 3. System architecture

![Block diagram](images/block-diagram.png)

- **Left cup:** main PCB (ESP32-S3 module, two amplifiers, charger, regulator, USB-C, button, LEDs), the LiPo battery, left speaker, left REF mic (outside) and left ERR mic (inside).
- **Right cup:** small hub PCB, right speaker, right REF mic and right ERR mic.
- **Headband:** one 8-wire cable between the cups. This replaces the v1 breadboard and jumper wires.

### Signal path (per ear)
1. The REF mic outside the cup hears the machine noise before it reaches the ear.
2. The ESP32-S3 runs FxLMS and makes anti-noise.
3. The amp drives the speaker inside the cup.
4. The ERR mic in front of the speaker hears what is left and the filter adapts.
5. In parallel, a sound classifier listens to the REF mics and switches the mode.

### Mode map (carried over from v1)

| Classifier result | Mode |
|---|---|
| machine or other | Cancel |
| alarm or voice | Hear-through: REF mic sound amplified into the ear |
| quiet | Idle, keep the last filter |
| unsure (under 60% confidence) | Half cancel, half hear-through |
| filter unstable or not calibrated | SAFE: anti-noise muted, passive earmuff only |

## 4. Key part choices

| Function | v1 part | v2 part | Why |
|---|---|---|---|
| Processor | ESP32-S3 DevKitC-1 N16R8 | ESP32-S3-WROOM-1-N16R8 module on my own PCB | Same chip, so v1 firmware ports over; the module is much smaller than the DevKit |
| Microphones | INMP441 breakouts (61 dBA SNR) | ICS-43434 on tiny custom boards (65 dBA SNR) | Lower noise, smaller, same I2S format; 120 dB SPL max input suits loud factories |
| Amplifier | 1 x MAX98357A breakout | 2 x MAX98357A on the main PCB | One per ear; same chip and code as v1 |
| Speakers | 40 mm 8 ohm 0.5 W | 40 mm 32 ohm headphone drivers | Made for a sealed ear cup, better low frequency output where machine hum lives |
| Power | USB power bank | 1000 mAh 1S LiPo, TP4056 charger, AP2112K 3.3 V regulator | Wearable, charges from USB-C |
| Earmuff | Red industrial earmuff (27 dB SNR) | A second earmuff of the same model | Keeps v1 intact so v1 and v2 can be compared side by side |

**Alternative considered:** an audio codec (for example WM8960) with analog mics. It could give lower delay, but it means new drivers and analog layout. I will only switch if I2S digital mics cannot meet R2. Decided after Test T1.

## 5. Delay budget (R2)

v1's delay was mostly buffering: audio was processed in blocks, and each block adds its length to the delay.

| Stage | v2 plan |
|---|---|
| Sample rate | 32 kHz |
| Block size | 8 samples (0.25 ms per block) |
| Input and output buffering | about 2 blocks (0.5 ms) |
| Mic and amp internal filters | to be measured |
| **Target total** | **under 1 ms** |

Compute check: two ears, a 128-tap control filter and a 64-tap secondary path model is about 320 multiply-adds per sample per ear. That is about 20 million per second at 32 kHz, which should fit on one core of the 240 MHz ESP32-S3. The classifier runs on the other core. This is an estimate; Test T1 confirms it.

## 6. Pin map (ESP32-S3-WROOM-1-N16R8)

![I2S wiring](images/wiring-i2s.png)

| GPIO | Net | Direction | Notes |
|---|---|---|---|
| 5 | I2S_BCLK | out | Same pin as v1. Also read by I2S1 as a slave clock |
| 6 | I2S_WS | out | Same pin as v1 |
| 7 | I2S_DOUT | out | To both amps' DIN, same pin as v1 |
| 15 | I2S0_DIN (REF) | in | REF_L on the left slot, REF_R on the right slot. Same pin as v1 |
| 16 | I2S1_DIN (ERR) | in | ERR_L on the left slot, ERR_R on the right slot |
| 8 | AMP_SD | out | Low mutes both amps. Through 10 kΩ to AMP_L (left channel), through 220 kΩ to AMP_R (right channel) |
| 4 | BUTTON | in | To GND, internal pull-up |
| 47 | LED_STATUS | out | Through resistor |
| 48 | LED_MODE | out | Through resistor |
| 1 | VBAT_SENSE | analog in | 100 kΩ / 100 kΩ divider from battery |
| 2 | CHG_STAT | in | TP4056 CHRG pin (open drain), 10 kΩ pull-up |
| 19, 20 | USB D-, D+ | USB | Native USB, used for flashing and serial |
| 0 | BOOT | in | Boot button to GND |
| EN | RESET | in | Reset button, 10 kΩ pull-up, 1 µF to GND |
| 43, 44 | UART TX, RX | test pads | Backup serial |

**Do not use:** GPIO 26 to 32 (flash), GPIO 33 to 37 (octal PSRAM on the R8 module), and strapping pins 3, 45 and 46.

**To check in the datasheets before layout:**
- The MAX98357A SD_MODE voltage windows that pick left or right channel. The 10 kΩ and 220 kΩ values assume its internal 100 kΩ pull-down: 3.3 V through 10 kΩ gives about 3.0 V (left) and through 220 kΩ gives about 1.0 V (right).
- That I2S1 can run as a slave on the same BCLK and WS pins that I2S0 drives. Bring-up test B1 proves this on the v1 DevKit before any PCB is ordered.

## 7. Power design

```
USB-C 5 V ──> TP4056 charger (RPROG 2 kΩ ≈ 600 mA) ──> LiPo 1000 mAh
   │                                                    │
   └─> SS14 diode ──> VSYS <── DMG2305UX P-MOSFET <──────┘
                       │      (battery cut off from VSYS while USB is in)
                       ├─> slide switch ──> VSYS_SW
                       │                     ├─> MAX98357A x2 (2.5 to 5.5 V)
                       │                     └─> AP2112K-3.3 ──> 3V3: ESP32-S3, 4 mics
```

- USB-C: 5.1 kΩ resistors on CC1 and CC2, USBLC6-2SC6 ESD protection on D+ and D-.
- Battery with a built-in protection circuit (most 1S LiPo packs have one). Firmware also shuts down at 3.5 V, because the regulator drops out below about that.
- Bulk capacitors: 10 µF and 0.1 µF at each amp, 22 µF and 0.1 µF at the module.

**Rough runtime estimate (R6):** ESP32-S3 at 240 MHz with radio off about 60 mA, mics about 2 mA, amps idle about 5 mA plus audio. Around 100 to 120 mA in total gives roughly 8 hours from 1000 mAh. This is a guess until Test T5. If it falls short, the upgrade is a buck-boost regulator (for example TPS63001) to use the whole battery.

## 8. PCBs (KiCad, 2 layers)

| Board | Size target | Qty to order | Contents |
|---|---|---|---|
| Main | about 45 x 35 mm, fits the left cup | 5 made, 2 assembled | ESP32-S3 module, 2 x MAX98357A, TP4056, AP2112K, USB-C, ESD, 2 buttons, switch, 2 LEDs, connectors |
| Mic | about 10 x 10 mm | 10 assembled (8 used, 2 spare) | ICS-43434, 0.1 µF, 5-pin JST-SH, L/R solder jumper, sound port hole |
| Right hub | about 20 x 15 mm | 5 made, hand soldered | 8-pin cable pads, 2 mic connectors, speaker pads |

Connectors: mics on 5-pin JST-SH (3V3, GND, BCLK, WS, SD); battery on 2-pin JST-PH; speakers on 2-pin JST-PH or pads.

The 8-wire headband cable carries 3V3, GND, BCLK, WS, REF data, ERR data, SPK_R+ and SPK_R-.

Layout rules:
- The ICS-43434 is bottom ported, so each mic board needs a hole under the sound port, sized from the datasheet.
- 33 Ω series resistor on BCLK and WS at the ESP32-S3, because they run about 35 cm to the right cup. Keep a ground wire beside the clock in the cable.
- Keep the ESP32-S3 antenna area clear of copper (radio is off, but follow the module guide).
- Class D amp outputs short, and away from mic lines.
- Assembly by JLCPCB, because the ICS-43434 LGA package and the MAX98357A TQFN package are hard to hand solder.

## 9. Enclosure and mounting (Onshape, printed in PETG)

Parts to model:
1. **Speaker baffle ring** (x2): holds the 40 mm driver in the cup and has a clip for the ERR mic about 5 to 10 mm in front of the driver centre.
2. **REF mic housing** (x2): mounts on the outside of each cup over a small hole, with foam to block wind.
3. **Left cup tray:** holds the main PCB and the battery behind the speaker baffle, with a USB-C cutout, button cap and LED light pipe.
4. **Right cup tray:** holds the hub board.
5. **Headband cable clips:** keep the cable along the inside of the band.

Measurements to take from the earmuff before modelling: cup inner depth, cup inner width and height, shell thickness, the space behind the foam, and the headband width.

## 10. Firmware plan (Arduino IDE, ESP32 core 3.x)

- **Core 1, audio task:** I2S0 full duplex (amps out, REF mics in) and I2S1 slave input (ERR mics). 32 kHz, 8-sample blocks. Two FxLMS channels. Output limiter caps the level so the speaker can never get dangerously loud.
- **Start-up calibration:** about 2 seconds of quiet test noise per ear to learn the speaker to ERR mic path. SAFE mode until it passes.
- **Core 0, classifier:** downsample REF audio to 16 kHz, 1 second windows, four classes. A mode only changes after two windows in a row agree, so it doesn't flicker.
- **Training data:** record clips through the v2 headset's own REF mic over USB, so the model learns from the same mic it will hear in use.
- **Instability guard:** if the ERR mic level rises instead of falling, reset the filter and go to SAFE.
- **Watchdog:** if the audio task stalls, mute the amps. The earmuff still blocks noise passively.

## 11. Bring-up and test plan

### Before ordering PCBs (on the v1 DevKit and parts)
| Test | What | Pass |
|---|---|---|
| B1 | Run I2S0 as master and I2S1 as slave on the same BCLK and WS, with two INMP441s | Both inputs stay sample-aligned for 10 minutes |
| B2 | Drop the block size to 8 samples at 32 kHz | Delay measured under 1 ms with no glitches |

### After assembly
| Test | What | Pass |
|---|---|---|
| T1 | Mic to speaker delay, same method as v1 | Under 1 ms |
| T2 | Sound level meter at the ear cup, steady machine hum, cancelling off then on, each ear | Record the dB difference (no fixed pass mark, the number is reported as measured) |
| T3 | Play an alarm 10 times over machine noise | Amplified within 1.5 s in at least 8 of 10 |
| T4 | 10 spoken calls over machine noise | Voice heard clearly in at least 8 of 10 |
| T5 | Full charge, run in cancel mode until shutdown | At least 8 hours |
| T6 | Pull the battery, crash the firmware, block the ERR mic | No loud output; earmuff still blocks noise |

## 12. Timeline

| Week | Work | Output |
|---|---|---|
| 1 | Bring-up tests B1 and B2 on the v1 parts | Proof the two-ear I2S plan works |
| 2 | Main board schematic | KiCad schematic |
| 3 | Mic and hub board schematics and layout; main board layout | KiCad PCBs, DRC clean |
| 4 | Measure the earmuff, model the enclosure parts | Onshape CAD |
| 5 | Order PCBs and parts | Cart and order |
| 6 to 7 | Assembly, firmware port to two ears | Working v2 |
| 8 | Tests T1 to T6, demo video | Results in README |

## 13. Risks

| Risk | Effect | Plan |
|---|---|---|
| I2S1 slave input does not stay aligned with I2S0 | Two-ear ANC fails | Test B1 first; fallback is a TDM microphone (ICS-52000) on one data line |
| Delay stays above 1 ms | Cancelling limited to steady hum | Smaller blocks; fallback is a codec (section 4) |
| Long clock lines to the right cup pick up noise | Clicks, wrong samples | Series resistors, ground beside clock, shorter cable |
| Battery life under 8 hours | Not a full shift | Buck-boost regulator, lower clock speed |
| Classifier wrong on the headset | Alarms missed or noise let through | Train on clips recorded through the headset's own mic |
| Hear-through too loud | Harms hearing | Firmware output limiter and gain cap, tested in T6 |
