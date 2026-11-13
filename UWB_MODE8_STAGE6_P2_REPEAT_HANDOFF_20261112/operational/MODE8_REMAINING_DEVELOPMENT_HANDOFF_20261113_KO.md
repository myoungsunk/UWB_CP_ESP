# Mode8-only implementation continuation — 2026-11-13

## Source and implementation boundaries

**Pinned basis:** `ce8f986793c5a12be0a020df924f05b516edf70e` for the Stage 6 P2-repeat RF engine; operational work branch `codex/mode8-operational-integration-20261113`. Prior verified head `602883f519776faa5fe0a13a78c04249735fc585`. This report accompanies the later incremental commits and their GitHub CI logs.

**Product scope:** Mode 8 only; do not restore Mode 3–7 or the old R03 format. Antenna delay/absolute ranging calibration is deferred by the user; preserve current raw RF/CIR, per-module TX/RX timestamps and calibration provenance without marking it calibrated.

## Work completed in this continuation

1. **M21/M22** — authenticate route journal bytes with HMAC and validate point identities, state order and exact session IDs before process resume; mutually exclusive CLI route lock so two controllers cannot start the same route in parallel. No automatic RF rerun of an uncertain START.
2. **M23** — fix persistent Mode8 Stage6/2 Mbps startup: preserve role-correct ESP32 anchor/tag firmware, prepare R8F6 serialization buffers before first fragmented HELLO and use 921600 diagnostic fallback if profile startup is unsafe. Paired USB `run_pair.py` also accepts explicit initial anchor/tag baud for persisted profiles.
3. **M24** — `ops_wired_continuous.py`: fail-closed *two-USB* fallback for 1,000 transactions. It invokes the existing dual-port Stage6 RF capture and verifies both raw RFB8 and P1/P2 grids; dry-run by default. For a remote anchor with no wire, uninterrupted 1,000-transaction storage is still BLOCKED.
4. **M17** — fail before RF when the anchor LittleFS capacity cannot hold the selected 32-row compact archive plus 8192-byte reserve. Existing journal/CRC and no-autoformat policy retained.
5. **M25** — `ops_pose_align.py`: offline per-radio-domain RF40 wrap-aware ROS/odom interpolation only if same-event sync pairs exist. Clock drift, sync RMS, wrap ambiguity, extrapolation and bad heading interpolation are tested. Does NOT implement ROS drive control or create physical sync evidence.
6. **M26** — `ops_acceptance_gate.py`: verify real evidence file SHA, exact Git SHA and gate ordering. Physical approval is *always false* without separate authoritative real-hardware signoff; the script only checks file integrity.
7. **M27** — anchor persistent `OPS NET AUTOSTART 0/1` USB command for wired operation while retaining default remote Wi-Fi reconnect behavior.
8. **Independent ongoing repo changes** — prior role-specific driver bug fixes and operational custody hardening already present in the branch were not overwritten. Audit/CI must be repeated against the exact final commit because these files may change concurrently.

## Tests and acceptance (explicitly distinct)

- GitHub Actions `.github/workflows/mode8-operational.yml` runs Python hostile-input/route/storage tests, portable GCC/ASan/UBSan, SHA equality of shared vs sketch source headers, and Arduino ESP32 Core 3.3.7 anchor/tag target builds.
- The image, source and artifact hash in a completed CI run certify that the code *compiled*. They are not the verified SHA of the device's actual flashed memory.
- `MODE8_SEQUENTIAL_VERIFICATION_SPEC_KO.md` defines mandatory G0–G12 and optional W01–W03/R01–R02/E01, including raw logs, stop criteria, and no automatic approvals.
- Target build PASS must not be elevated into physical Wi-Fi/TCP/OTA, long-term flash endurance, sudden power-off or accuracy PASS.

## Outstanding work and honest workarounds

| Item | Status | Constraint / safe alternative |
|---|---|---|
| M04: gap-free 1,000 records on untethered Wi-Fi-off anchor | BLOCKED | ≤32 compact rows in RAM; implement physically isolated nonvolatile storage/USB anchor-to-Pi link/validated writer architecture. Two wired USBs are a different deployment option. |
| M05: power loss cannot erase unsaved RAM | BLOCKED | UPS or nonvolatile writes within proven RF timing budget; any missing data must be explicitly incomplete |
| M08/M09: target-enforced signed role-bound OTA and automatic rollback | PARTIAL | host image preflight and device OTA-partition/first-boot checks exist, but bootloader settings, signatures and power-cut tests are not complete; isolate the AP and retain whole-flash recovery |
| M10 tag OTA, M11 Enterprise Wi-Fi, M12 TLS/secure boot | OPTIONAL / NOT COMPLETE | requires board/credential/bootloader decisions; tag Pi USB and isolated WPA network remain supported |
| M13 <200ms while preserving all four CIRs | NOT APPROVED | current 200ms remains default; 190ms must be proven on-device with RX/flash/USB error injection |
| M14/M16/M17 physical power, PLL, 24h soak, flash wear | NOT RUN | needs actual ESP32 boards, radios, Pi, AP and controlled power cut |
| M15 live ROS motion/GT calibration | PARTIAL | offline alignment is available, but user-supplied physical sync and robot trajectory logs are prerequisites |
| M18 actual flashed image readback/physical deployment receipt | NOT RUN | safe backup first, attach flashed BIN SHA and partition hashes for each role; no board access here |

**Acceptance:** source/host/target CI as reported by an exact-successful run only; field operating approval HOLD until G5–G12 actual board tests and separate validation.
