"""ANCHOR v2 boards: parts and connections. Run gen_sch.py to write the KiCad schematics."""
from anchor_eda import Part

R0603 = "Resistor_SMD:R_0603_1608Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
LED0603 = "LED_SMD:LED_0603_1608Metric"
SH5 = "Connector_JST:JST_SH_SM05B-SRSS-TB_1x05-1MP_P1.00mm_Horizontal"
SH8 = "Connector_JST:JST_SH_SM08B-SRSS-TB_1x08-1MP_P1.00mm_Horizontal"
PH2 = "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal"
SH2 = "Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal"
MH = "MountingHole:MountingHole_2.2mm_M2"


def R(ref, val, a, b, mpn=""):
    return Part(ref, "Device:R", val, R0603, {"1": a, "2": b}, mpn=mpn)


def C(ref, val, a, b="GND", fp=None):
    return Part(ref, "Device:C", val, fp or (C0805 if val in ("10u", "22u") else C0603), {"1": a, "2": b})


def amp(ref, sd, p, n, vdd):
    return Part(ref, "Audio:MAX98357A", "MAX98357A", "Package_DFN_QFN:TQFN-16-1EP_3x3mm_P0.5mm_EP1.23x1.23mm",
                {"DIN": "I2S_DOUT", "GAIN_SLOT": vdd, "GND": "GND", "PAD": "GND", "~{SD_MODE}": sd, "NC": None,
                 "VDD": vdd, "BCLK": "I2S_BCLK", "LRCLK": "I2S_WS", "OUTP": p, "OUTN": n}, mpn="MAX98357AETE+T", lcsc="C910544")


# headband cable: the right ear's own I2S port (pin 6 is an extra ground beside the clocks)
CABLE_MAIN = {"1": "+3V3", "2": "GND", "3": "I2S_BCLK_R", "4": "I2S_WS_R", "5": "I2S_DIN_R", "6": "GND", "7": "I2S_DOUT_R", "8": "AMP_R_SD"}
CABLE_HUB = {"1": "+3V3", "2": "GND", "3": "I2S_BCLK", "4": "I2S_WS", "5": "I2S_DIN", "6": "GND", "7": "I2S_DOUT", "8": "AMP_SD"}


def mic_conn(ref, data, label=None):
    return Part(ref, "Connector_Generic:Conn_01x05", f"JST-SH 5 ({label or data})", SH5,
                {"1": "+3V3", "2": "GND", "3": "I2S_BCLK", "4": "I2S_WS", "5": data}, mpn="SM05B-SRSS-TB")


# ---------------------------------------------------------------- mic board (x4)
def mic_board():
    return [
        Part("U1", "Sensor_Audio:ICS-43434", "ICS-43434", "Sensor_Audio:InvenSense_ICS-43434-6_3.5x2.65mm",
             {"WS": "I2S_WS", "LR": "LR_SEL", "GND": "GND", "SCK": "I2S_BCLK", "VDD": "+3V3", "SD": "I2S_SD"}, mpn="ICS-43434", lcsc="C5656610"),
        C("C1", "100n", "+3V3"),
        # one PCB, two builds: REF mic = R1 fitted (L/R low, left slot), ERR mic = R2 fitted (L/R high, right slot)
        R("R1", "0R (REF build only)", "LR_SEL", "GND"),
        R("R2", "0R (ERR build only)", "LR_SEL", "+3V3"),
        mic_conn("J1", "I2S_SD"),
    ]


# ---------------------------------------------------------------- right cup hub
def hub_board():
    return [
        Part("J1", "Connector_Generic:Conn_01x08", "JST-SH 8 to main board", SH8, CABLE_HUB, mpn="SM08B-SRSS-TB"),
        # right amplifier lives here so only digital audio crosses the headband
        amp("U1", "AMP_SD", "SPK_R_P", "SPK_R_N", "AMP_VDD"),
        Part("FB1", "Device:FerriteBead_Small", "600R@100MHz", "Inductor_SMD:L_0603_1608Metric", {"1": "+3V3", "2": "AMP_VDD"}),
        C("C3", "10u", "AMP_VDD"), C("C4", "100n", "AMP_VDD"),
        mic_conn("J2", "I2S_DIN", "REF R"),
        mic_conn("J3", "I2S_DIN", "ERR R"),
        Part("J4", "Connector_Generic:Conn_01x02", "Speaker R (JST-SH 2)", SH2, {"1": "SPK_R_P", "2": "SPK_R_N"}, mpn="SM02B-SRSS-TB"),
        C("C1", "10u", "+3V3"),
        C("C2", "100n", "+3V3"),
        Part("H1", "Mechanical:MountingHole", "M2", MH, {}),
        Part("H2", "Mechanical:MountingHole", "M2", MH, {}),
    ]


# ---------------------------------------------------------------- main board (left cup)
def main_board():
    esp = {"GND": "GND", "3V3": "+3V3", "EN": "EN", "IO0": "BOOT", "IO1": "VBAT_SENSE", "IO2": "CHG_STAT",
           "IO4": "BUTTON", "IO5": "BCLK_MCU", "IO6": "WS_MCU", "IO7": "I2S_DOUT", "IO8": "AMP_SD",
           "IO15": "I2S_DIN_L", "IO16": "I2S_DIN_R", "IO17": "BCLK_R_MCU", "IO18": "WS_R_MCU", "IO21": "I2S_DOUT_R",
           "IO19": "USB_DN", "IO20": "USB_DP",
           "IO47": "LED1", "IO48": "LED2", "TXD0": "TXD0", "RXD0": "RXD0"}
    for n in ["IO3", "IO9", "IO10", "IO11", "IO12", "IO13", "IO14", "IO35", "IO36", "IO37",
              "IO38", "IO39", "IO40", "IO41", "IO42", "IO45", "IO46"]:
        esp[n] = None


    return [
        # USB-C and protection
        Part("J1", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
             {"GND": "GND", "VBUS": "VBUS", "CC1": "USB_CC1", "CC2": "USB_CC2", "D+": "USB_DP", "D-": "USB_DN",
              "SBU1": None, "SBU2": None, "SHIELD": "GND"}, mpn="TYPE-C-31-M-12"),
        R("R1", "5.1k", "USB_CC1", "GND"), R("R2", "5.1k", "USB_CC2", "GND"),
        Part("U1", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6",
             {"I/O1": "USB_DP", "I/O2": "USB_DN", "GND": "GND", "VBUS": "VBUS"}, mpn="USBLC6-2SC6"),
        # charging and power path
        Part("U2", "Battery_Management:MCP73831-2-OT", "MCP73831 500 mA", "Package_TO_SOT_SMD:SOT-23-5",
             {"STAT": "CHG_STAT_RAW", "V_{SS}": "GND", "V_{BAT}": "VBAT", "V_{DD}": "VBUS", "PROG": "PROG"}, mpn="MCP73831T-2ACI/OT"),
        R("R4", "2k", "PROG", "GND"),
        R("R15", "10k", "CHG_STAT_RAW", "CHG_STAT"), R("R17", "20k", "CHG_STAT", "GND"),
        C("C1", "4.7u", "VBUS"), C("C2", "4.7u", "VBAT"),
        Part("D1", "Device:D_Schottky", "SS14", "Diode_SMD:D_SMA", {"A": "VBUS", "K": "VSYS"}, mpn="SS14"),
        Part("Q1", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23", {"G": "VBUS", "S": "VSYS", "D": "VBAT"}, mpn="AO3401A"),
        R("R3", "100k", "VBUS", "GND"),
        C("C13", "10u", "VSYS"),
        Part("J2", "Connector_Generic:Conn_01x02", "Battery (JST-PH 2)", PH2, {"1": "BAT_P", "2": "GND"}, mpn="S2B-PH-SM4-TB"),
        # reverse battery protection: P-MOSFET conducts both ways when the battery is the right way round
        Part("Q2", "Transistor_FET:AO3401A", "AO3401A", "Package_TO_SOT_SMD:SOT-23", {"G": "GND", "D": "BAT_P", "S": "VBAT"}, mpn="AO3401A"),
        # power switch only drives the regulator enable, so it carries no load current
        Part("SW1", "Switch:SW_SPDT", "Power", "Button_Switch_SMD:SW_SPDT_PCM12", {"A": "VSYS", "B": "LDO_EN", "C": None}, mpn="PCM12SMTR"),
        R("R18", "100k", "LDO_EN", "GND"),
        # 3.3 V
        Part("U3", "Regulator_Linear:AP2112K-3.3", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5",
             {"VIN": "VSYS", "GND": "GND", "EN": "LDO_EN", "NC": None, "VOUT": "+3V3"}, mpn="AP2112K-3.3TRG1"),
        C("C3", "1u", "VSYS"), C("C4", "10u", "+3V3"), C("C5", "22u", "+3V3"), C("C6", "100n", "+3V3"),
        # battery sense
        R("R5", "100k", "VBAT", "VBAT_SENSE"), R("R6", "100k", "VBAT_SENSE", "GND"), C("C8", "100n", "VBAT_SENSE"),
        # MCU
        Part("U4", "RF_Module:ESP32-S3-WROOM-1", "ESP32-S3-WROOM-1-N16R8", "RF_Module:ESP32-S3-WROOM-1", esp, mpn="ESP32-S3-WROOM-1-N16R8", lcsc="C2913202"),
        R("R7", "10k", "EN", "+3V3"), C("C7", "1u", "EN"),
        R("R8", "10k", "BOOT", "+3V3"), R("R16", "10k", "BUTTON", "+3V3"),
        Part("SW2", "Switch:SW_Push", "Reset", "Button_Switch_SMD:SW_Push_1P1T_NO_CK_KMR2", {"1": "EN", "2": "GND"}),
        Part("SW3", "Switch:SW_Push", "Boot", "Button_Switch_SMD:SW_Push_1P1T_NO_CK_KMR2", {"1": "BOOT", "2": "GND"}),
        Part("SW4", "Switch:SW_Push", "Mode", "Button_Switch_SMD:SW_Push_1P1T_NO_CK_KMR2", {"1": "BUTTON", "2": "GND"}),
        R("R13", "1k", "LED1", "LED1_A"), R("R14", "1k", "LED2", "LED2_A"),
        Part("D2", "Device:LED", "Status", LED0603, {"A": "LED1_A", "K": "GND"}),
        Part("D3", "Device:LED", "Mode", LED0603, {"A": "LED2_A", "K": "GND"}),
        Part("TP1", "Connector:TestPoint", "TXD0", "TestPoint:TestPoint_Pad_D1.0mm", {"1": "TXD0"}),
        Part("TP2", "Connector:TestPoint", "RXD0", "TestPoint:TestPoint_Pad_D1.0mm", {"1": "RXD0"}),
        Part("TP3", "Connector:TestPoint", "GND", "TestPoint:TestPoint_Pad_D1.0mm", {"1": "GND"}),
        Part("TP4", "Connector:TestPoint", "3V3", "TestPoint:TestPoint_Pad_D1.0mm", {"1": "+3V3"}),
        # I2S clocks with series resistors (long run to the right cup)
        R("R9", "33", "BCLK_MCU", "I2S_BCLK"), R("R10", "33", "WS_MCU", "I2S_WS"),
        # amplifiers: SD_MODE level picks the channel (10k = left, 220k = right, low = shutdown)
        R("R11", "10k", "AMP_SD", "AMP_L_SD"), R("R12", "10k", "AMP_SD", "AMP_R_SD"),
        R("R19", "10k", "AMP_SD", "+3V3"),   # amps on by default (v1 firmware never drives GPIO 8)
        R("R20", "33", "BCLK_R_MCU", "I2S_BCLK_R"), R("R21", "33", "WS_R_MCU", "I2S_WS_R"),
        amp("U5", "AMP_L_SD", "SPK_L_P", "SPK_L_N", "VSYS"),
        C("C9", "10u", "VSYS"), C("C10", "100n", "VSYS"),
        # connectors
        Part("J3", "Connector_Generic:Conn_01x02", "Speaker L (JST-SH 2)", SH2, {"1": "SPK_L_P", "2": "SPK_L_N"}, mpn="SM02B-SRSS-TB"),
        mic_conn("J4", "I2S_DIN_L", "REF L"),
        mic_conn("J5", "I2S_DIN_L", "ERR L"),
        Part("J6", "Connector_Generic:Conn_01x08", "JST-SH 8 to right cup", SH8, CABLE_MAIN, mpn="SM08B-SRSS-TB"),
        Part("H1", "Mechanical:MountingHole", "M2", MH, {}), Part("H2", "Mechanical:MountingHole", "M2", MH, {}),
    ]


BOARDS = {"mic": ("ANCHOR v2 mic board (ICS-43434)", mic_board, "A4"),
          "hub": ("ANCHOR v2 right cup hub", hub_board, "A4"),
          "main": ("ANCHOR v2 main board (left cup)", main_board, "A2")}
