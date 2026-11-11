# DUAL_DS_CIR / Mode 8, protocol version 1

## Scope

One anchor with one DW1000, one ESP32 tag with two DW1000 radios. Serial-control-only development firmware. No RF-switch output. RF polling rather than interrupt-driven register access. This protocol is not wire-compatible with legacy FCB5/RFB5 and is not authenticated or encrypted. Session/config/boot identifiers prevent accidental mixing, not malicious injection.

RF fixed profile: channel 5, 850 kbps, 16 MHz PRF, 256-symbol preamble, code 3, standard SFD, frame check enabled. Receiver auto-reenable and double buffering disabled. Original role-specific DW1000Ng libraries are preserved. R2 uses smartPower=false for 850 kbps; actual TX power and emissions require a hardware acceptance test. The Config CRC includes contract revision byte 2 at input offset 39 to prevent mixing with original Mode-8 R1 firmware.

## Timeline

| Nominal RMARKER time relative to anchor POLL | Event |
|---:|---|
| 0 ms | anchor POLL, received by tag A/B |
| 7 ms | RESPONSE_A, based on A's local POLL RX timestamp |
| 14 ms | RESPONSE_B, based on B's local POLL RX timestamp |
| 19 ms | anchor Probe 1 / DS FINAL, A1/B1 simultaneous reception |
| 51 ms | anchor Probe 2, A2/B2 simultaneous reception; confirms actual P1 TX |

Times are nominal slots, not total RF occupied airtime, end-of-frame timestamps, processing completion times, or repeat periods. Delayed TX resolution and antenna delay are handled explicitly:

```
dx = ((requested_timestamp - TX_ANTD) & MASK40) & ~511
planned_timestamp = (dx + TX_ANTD) & MASK40
```

Each actual TX timestamp must equal planned. A late TX is not silently converted to immediate TX. During either tag RESPONSE, the other tag receiver is off. P1/P2 are captured with both receivers armed, then both off before sequential accumulator reads. Read order alternates A→B / B→A by txn parity.

## Air frame, 96 application bytes, little endian

Hardware FCS is handled by DW1000 and not included in these 96 bytes. An additional application CRC32 is included.

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | ASCII FCB8 |
| 4 | 1 | version = 1 |
| 5 | 1 | 1 POLL, 2 RESP_A, 3 RESP_B, 4 P1, 5 P2 |
| 6 | 2 | application length = 96 |
| 8 | 4 | session |
| 12 | 4 | txn |
| 16 | 4 | common configuration CRC |
| 20 | 4 | sender MCU boot ID |
| 24 | 4 | peer MCU boot ID, zero if unknown |
| 28 | 1 | sender radio: A=0, B=1, anchor=255 |
| 29 | 1 | accepted RESPONSE bitmask |
| 30 | 1 | TX verification mask: bit0 POLL, bit1 P1 confirmed |
| 31 | 1 | stage 2–6 |
| 32 | 5 | anchor POLL TX |
| 37 | 5 | anchor RESPONSE_A RX |
| 42 | 5 | anchor RESPONSE_B RX |
| 47 | 5 | current Probe planned TX |
| 52 | 5 | previous P1 actual TX, reported in P2 |
| 57 | 5 | local tag POLL RX, in RESPONSE |
| 62 | 5 | local tag RESPONSE planned TX, in RESPONSE |
| 67 | 4 | A carrier-integrator diagnostic, signed int32 |
| 71 | 4 | B carrier-integrator diagnostic, signed int32 |
| 75 | 17 | reserved, must be zero |
| 92 | 4 | IEEE CRC32 of bytes 0–91 |

Configuration ID includes stage, slot timing, period, receive grace, minimum lead and fixed RF profile. Baud rate, local CIR read speed, calibration and repeated-read selection are local rather than on-air identity fields.

## Distance validity and missing data

For each i=A/B, in its own local clock domain:

```
R1 = delta40(anchor_RESP_i_RX, anchor_POLL_TX)
D1 = delta40(tag_i_RESP_TX, tag_i_POLL_RX)
R2 = delta40(tag_i_P1_RX, tag_i_RESP_TX)
D2 = delta40(anchor_P1_TX, anchor_RESP_i_RX)
ToF = (R1*R2 - D1*D2)/(R1+R2+D1+D2)
range = ToF * (1/63897600000) * 299702547
```

Products are computed in double, not 64-bit integer multiplication. Python analysis independently uses integer products and rational division. Interval bounds, finite range and a 0–300 m development guard are checked. This guard is not an RF operating-range specification.

Final P1 range requires local POLL, verified local RESPONSE TX, accepted anchor RESPONSE RX, local P1 with LDE, coherent metadata, and at least one received P2 confirming the exact P1 actual TX timestamp. A received P2 on B can confirm P1 TX for both channels even if A missed P2. No P2 at either radio means P1 CIR is retained, but final distance is invalid. A/B failures remain independent when not due to a shared integrity failure.

`math_valid` is computational validity, NOT proven LOS, calibrated absolute accuracy, or motion-free ground truth. Antenna calibration IDs default to zero.

## RFB8 output

24-byte header `<4sBBHIIII>`:

```
magic='RFB8', version=1, kind=1(event)/2(record), header_bytes=24,
record_sequence, json_bytes, raw_bytes, CRC32(JSON || raw)
```

Tag records always reserve four 3968-byte slots: A1, B1, A2, B2. A missing slot remains zero with present/valid=false; zeros are not valid measurement data. Anchor records have raw_bytes=0. Each available CIR has an independent CRC and repeat-read result. Raw complex samples are signed little-endian int16 I, then int16 Q. The parser validates lengths, header, outer CRC, metadata role and raw length, and supports resynchronization after serial noise.

RX metadata includes adjusted/raw HW timestamps, LDE bit, FP_INDEX as fp_raw/64, RXPACC, CIR_PWR, F1–3, noise, carrier integrator, ESP observation time and exact raw air frame bytes. ESP timestamps are software observation times, not RF arrival times. No Pi/ROS time synchronization has been added.

Power columns are raw DW1000 16 MHz diagnostic formulas without the old empirical strong-signal adjustment. `power_formula=DW16_RAW_NO_EMPIRICAL` identifies this intentional difference. Raw diagnostics are preserved for later calibration.

## Host physical-delay reconstruction

Native sample spacing is `1/998400000 s`, distinct from the HW timestamp tick. Native arrays remain untouched.

```
relative_delay = (k - fp_raw/64) / 998400000
physical_delay_estimate = range_estimate/299702547 + relative_delay
```

P1 uses its confirmed DS range. P2 uses a separately recomputed DS range only when matching anchor logs verify P2's actual TX. This second range shares POLL/RESPONSE timestamps with P1 and is correlated, not an independent replicate. Without anchor evidence, P2 remains relative-only unless `--assume-static` is explicitly supplied. A reused P1 distance is clearly labeled as a static-scene assumption.

For each Probe, normalized power is interpolated onto a common delay grid. Complex CIR is NOT phase-rotated, coherently added, or treated as having a common LO phase. A/B receiver gain calibration remains a separate requirement. FP-detection bias, NLOS, antenna group delays, clock changes and motion over the ranging interval can corrupt the physical-axis estimate. Range/FP error covariance is not inferred from a single record. Reported sigma is only a declared range contribution, not total axis uncertainty.

## References used for implementation

- Original supplied R1L80 sources and role-specific DW1000Ng (preserved in this package).
- Qorvo/Decawave APS013, DS-TWR and delayed TX background: https://forum.qorvo.com/uploads/short-url/x34DrF7EW5fQP9wY3aNESqPKz8z.pdf
- Arduino CLI compile/upload command docs, links in BUILD_AND_FLASH_KO.md.

## R2 fail-closed capture and analysis rules

The per-RF-slot received `probe_rx[slot].air` bytes must match the corresponding shared frame and anchor metadata. P1/P2 on-air CRC and identities must match, P2 must confirm actual P1 TX, and P1's six timestamps must match the local hardware RX/TX event fields before any physical axis is generated. The independent P2 distance requires a complete matching anchor event log (POLL/P1/P2 actual TX, per-radio RESPONSE RX and all three on-air frames). A metadata conflict disables physical axes, while raw CIR and FP-relative axes are retained when their own integrity checks pass. Explicit `--assume-static` cannot override a conflict.

`capture_receipt.pass=true` additionally requires records from the exact requested session and both HELLO boots, all transaction IDs `0..count-1` once in order per role, paired RF frame equality, raw CIR slot CRC/length verification, no decoder sequence gaps/corruption and no serial reader or hardware error. CRC is not a message authentication code. A pass does not certify calibrated absolute ranging or RF channel fidelity.
