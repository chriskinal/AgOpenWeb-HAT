#!/usr/bin/env python3
"""Read all 8 channels of the HAT's ADC128S102 through /dev/spidev0.3 (no IIO driver).

Frame: 16 bits per conversion; the host sends the *next* channel in bits 13:11 (ADD2..0) of
the first byte, the ADC returns the 12-bit result of the channel selected in the previous
frame. So we clock 9 frames and keep the last 8. Mode 3 (CPOL=1, CPHA=1) per the datasheet
(mode 0 also works). Reference = VA = 5V_MAIN.
"""
import array, fcntl, struct, sys

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/spidev0.3"
VREF = 5.0
SPI_IOC_WR_MODE = 0x40016b01
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046b04
SPI_IOC_MESSAGE_1 = 0x40206b00

def xfer(fd, tx):
    rx = bytearray(len(tx))
    txb = bytes(tx)
    tx_arr = array.array('B', txb); rx_arr = array.array('B', rx)
    tx_addr, _ = tx_arr.buffer_info(); rx_addr, _ = rx_arr.buffer_info()
    # struct spi_ioc_transfer: tx_buf u64, rx_buf u64, len u32, speed_hz u32, delay_usecs u16,
    # bits_per_word u8, cs_change u8, tx_nbits u8, rx_nbits u8, word_delay_usecs u8, pad u8
    msg = struct.pack('QQIIHBBBBBB', tx_addr, rx_addr, len(txb), 1000000, 0, 8, 0, 0, 0, 0, 0)
    fcntl.ioctl(fd, SPI_IOC_MESSAGE_1, msg)
    return bytes(rx_arr)

fd = open(DEV, 'rb+', buffering=0)
fcntl.ioctl(fd, SPI_IOC_WR_MODE, struct.pack('B', 3))
fcntl.ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, struct.pack('I', 1000000))
# One transaction of 9 frames: select ch0, then ch1..7, then ch0 again; results lag by one.
tx = bytearray()
for ch in list(range(8)) + [0]:
    tx += bytes([(ch & 7) << 3, 0])
rx = xfer(fd, tx)
names = {0: "WAS", 1: "current", 2: "VIN_SENSE", 3: "gnd", 4: "KEY_SENSE", 5: "gnd", 6: "gnd", 7: "gnd"}
for ch in range(8):
    i = ch + 1  # result for channel ch arrives in frame ch+1
    counts = ((rx[2 * i] << 8) | rx[2 * i + 1]) & 0x0FFF
    v = counts * VREF / 4096
    extra = ""
    if ch in (2, 4):
        extra = "  -> %.1f V at the wire" % (v / 0.0758)
    print("IN%d %-10s %4d counts  %.3f V%s" % (ch, names[ch], counts, v, extra))
