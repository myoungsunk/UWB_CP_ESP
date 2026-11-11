# DUAL_DS_CIR / Mode 8, protocol version 1

## Scope

One anchor with one DW1000, one ESP32 tag with two DW1000 radios. Serial-control-only development firmware. No RF-switch output. RF polling rather than interrupt-driven register access. This protocol is not wire-compatible with legacy FCB5/RFB5 and is not authenticated or encrypted. Session/config/boot identifiers prevent accidental mixing, not malicious injection.

RF fixed profile: channel 5, 850 kbps, 16 MHz PRF, 256-symbol preamble, code 3, standard SFD, frame check enabled. Receiver auto-reenable and double buffering disabled. Original role-specific DW1000Ng libraries are preserved. Smart TX Power is disabled at 850 kbps. Actual TX power and emissions require a hardware acceptance test. The Config CRC uses contract revision byte 7 at input offset 39 for the Stage 5 and Stage 6 schedules below, and byte 4 for other stages. Default Stage 5 Config ID: 459867368.

## Timeline

| Nominal RMARKER time relative to anchor POLL | Event |
|---:|---|
| 0 ms | anchor POLL, received by tag A/B |
| 10 ms | RESPONSE_A, based on A's local POLL RX timestamp |
| 20 ms | RESPONSE_B, based on B's local POLL RX timestamp |
| 28 ms | anchor Probe 1, A1/B1 simultaneous reception |
| 325 ms | anchor POLL2, received by tag A/B |
| 335 ms | second RESPONSE_A, 10 ms after A's local POLL2 RX |
| 345 ms | second RESPONSE_B, 20 ms after B's local POLL2 RX |
| 353 ms | anchor Probe 2, A2/B2 simultaneous reception; confirms actual P1 TX |

The table applies to the tested Stage 5 setup at a 650 ms transaction period and 921600 baud. Set `PERIOD_US 650000` on both boards for this procedure. The firmware initializes at 300 ms for configuration compatibility. With the two exchanges and USB CIR streaming, the 300 ms setting delayed POLL handling on the test tag in transactions 1 through 19. POLL2 is scheduled at half the configured period. Each probe uses the same POLL-to-probe interval. Stage 6 bounds each record write to 128 bytes and uses the same over-the-air exchange at its configured period. Times are nominal slots, not total RF occupied airtime, end-of-frame timestamps, processing completion times, or repeat periods. Delayed TX resolution and antenna delay are handled explicitly:

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
| 5 | 1 | 1 POLL, 2 RESP_A, 3 RESP_B, 4 P1, 5 P2, 6 RESP2_A, 7 RESP2_B, 8 POLL2 (types 6 through 8 are for Stages 5 and 6) |
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
| 32 | 5 | anchor POLL TX (anchor P1 TX for types 6 and 7) |
| 37 | 5 | anchor RESPONSE_A RX |
| 42 | 5 | anchor RESPONSE_B RX |
| 47 | 5 | current Probe planned TX |
| 52 | 5 | previous P1 actual TX, reported in P2 |
| 57 | 5 | local tag POLL RX, or local P1 RX for types 6 and 7 |
| 62 | 5 | local tag response planned TX (types 2, 3, 6, and 7) |
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

For Stage 5 and Stage 6 P2, use POLL2 TX/RX as the start of a separate DS exchange, types 6/7 as the responses, and P2 TX/RX as the final frame. Match the POLL2 frame on both hosts, verify both tag RX slots against the on-air CRC, and bind each fresh response to the matching POLL2 and actual tag TX. P2 uses its actual TX time from the matching anchor record.

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

P1 uses its confirmed DS range. In paired Stage 5 and Stage 6 analysis, P2 uses the separately measured POLL2, RESPONSE2 and P2 timestamps. Tag-only analysis produces an FP-relative P2 axis by default. With `--assume-static`, tag-only analysis may reuse a valid P1 distance as an explicitly labeled static-scene estimate.

For each Probe, normalized power is interpolated onto a common delay grid. Complex CIR is NOT phase-rotated, coherently added, or treated as having a common LO phase. A/B receiver gain calibration remains a separate requirement. FP-detection bias, NLOS, antenna group delays, clock changes and motion over the ranging interval can corrupt the physical-axis estimate. Range/FP error covariance is not inferred from a single record. Reported sigma is only a declared range contribution, not total axis uncertainty.

## References used for implementation

- Original supplied R1L80 sources and role-specific DW1000Ng (preserved in this package).
- Qorvo/Decawave APS013, DS-TWR and delayed TX background: https://forum.qorvo.com/uploads/short-url/x34DrF7EW5fQP9wY3aNESqPKz8z.pdf
- Arduino CLI compile/upload command docs, links in BUILD_AND_FLASH_KO.md.

## Capture and analysis rules

The received `probe_rx[slot].air` bytes must match the shared frame. A receiver with a missed POLL and no associated RF frame can be absent from a partial analysis. Each physical range requires that receiver's own valid POLL, its local RESPONSE TX of the expected type and radio, and the applicable Probe RX evidence. P1 also requires a matching received P2 slot and six timestamps consistent with the hardware events. P2 DS requires an anchor log with correct POLL/P1/P2 TX slot IDs, planned and actual TX timestamps that equal the RF timestamps, and raw RESPONSE RX evidence for the radio. An anchor log can supply the calibration flag only when this provenance matches. Raw CIR and FP-relative axes are retained when their own integrity checks pass.

`capture_receipt.pass=true` requires records from the exact requested session and both HELLO boots, all transaction IDs `0..count-1` once in order per role, paired RF frame equality, valid raw CIR slots, a clean reader stream and acknowledged shutdown. In Stage 2 the anchor has received no tag RESPONSE. Its wrapper and Probe peer boot IDs are zero. The tag wrapper still carries its own HELLO boot ID and the anchor's boot ID.

`STOP` and `STOP <request>` both stop the session. The request is an optional nonzero uint32 in decimal. The `STOPPED` event echoes it in `stop_request` and carries the device role and session. A bare or automatic stop uses zero. Final capture success requires both acknowledgments to match the final request and session. This confirms firmware command processing.
