# Packets with error correction

None of the modems (except AX.25) can detect transmission errors: a wrong bit silently changes the received data. `PacketStream` adds reliability on top of any Arduino `Stream`: a `ModemStream`, `MFSKStream`, `PSKStream` or `OFDMStream`, but also a `Serial`:

- The data is sent in **packets** of up to 64 bytes.
- Each packet has a **CRC-16** and **Reed-Solomon** parity bytes, which correct up to 8 wrong bytes per packet.
- Packets which can't be corrected are **dropped**: you only read correct data.

Packets are an extension of TinyModem: the original minimodem doesn't support them.

**Advantages**

- Only correct data is delivered.
- Most errors are corrected, so no data has to be sent again.
- Works with every modem and every `Stream`, without changes.

**Disadvantages**

- About 30% overhead (18 bytes per 64 bytes of data) plus the framing.
- Lost or added bytes can't be corrected: such a packet is dropped. FSK can lose bytes when a start bit isn't detected; MFSK, PSK and OFDM produce wrong bytes rather than lost ones.
- There is no retransmission: a dropped packet is lost.

## Arduino

```cpp
I2SStream i2s;
MFSKStream modem(i2s, RXTX_MODE);
PacketStream<64, 16> packets(modem);  // max payload, parity bytes

void setup() {
  ...
  modem.begin(cfg);
  packets.begin();
}

void loop() {
  packets.print("Hello");
  packets.flush();  // send the packet (and end the transmission)
  while (packets.available()) Serial.write(packets.read());
}
```

- A packet is sent when 64 bytes were written or when you call `flush()`. `flush()` also calls `flush()` of the underlying stream, which ends the transmission of a modem.
- `statistics()` provides the number of `received`, `corrected` and `dropped` packets and the number of `corrected_bytes`.
- The template parameters define the maximum payload and the number of parity bytes (default `<64, 16>`): up to `ECC_BYTES / 2` wrong bytes per packet are corrected. Both sides must use the same parameters. A packet is limited to 255 bytes in total.
- `PacketEncoder` and `PacketDecoder` provide the same functionality as `Print`, for example to use them in an AudioTools pipeline.

See the [packet-modem](../examples/packet-modem) example.

## Command line

```sh
tinymodem --tx --packets mfsk16 < file.bin
tinymodem --rx --packets mfsk16 > file.bin
```

`--packets` works with all modems except AX.25 (which has its own CRC). The command line tool always uses `<64, 16>`. At the end of the reception it prints `### PACKETS received=… corrected=… corrected_bytes=… dropped=… ###` (unless `-q`).

## Format

A packet is transmitted as:

```
0x7E | payload | CRC-16 | Reed-Solomon parity | 0x7E
```

- The packets are delimited by HDLC flags (`0x7E`) with byte stuffing: `0x7E` and `0x7D` in the data are sent as `0x7D`, followed by the byte XOR `0x20`. This way the receiver finds the next packet even after noise or a lost byte.
- CRC-16-CCITT (polynomial `0x1021`, initial value `0xFFFF`) over the payload.
- Reed-Solomon over the payload and the CRC, with the codec of the arduino-audio-tools. Shorter packets use a shortened code (the missing bytes are zeros which aren't sent), so there is no padding.

## Measured performance

Measured with the unit tests and the command line tool:

| Modem and noise | Packets received | Notes |
|---|---|---|
| FSK 1200 baud at 8000 Hz, noise 0.15 | 36 of 40 | 158 wrong bytes corrected, the 4 packets with lost bytes were dropped. Without packets, 1.2% of the bytes were wrong. |
| `mfsk16`, noise 1.2 | all | |
| `psk125`, noise 1.0 | all 64 of a 4 KB file | |
| `ofdm32`, noise 0.12 | all 64 of a 4 KB file | 3 bytes corrected |
| `ofdm32`, noise 0.15 | 62 of 64 | 46 bytes corrected, 2 packets dropped |

No wrong data was delivered in any of the tests. The Reed-Solomon decoder needs about `3 × (MAX_PAYLOAD + 2) + 28 × ECC_BYTES` bytes of stack (650 bytes for `<64, 16>`).
