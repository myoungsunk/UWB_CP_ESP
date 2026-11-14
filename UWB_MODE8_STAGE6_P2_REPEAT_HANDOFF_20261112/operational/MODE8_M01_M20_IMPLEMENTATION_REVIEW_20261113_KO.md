# M01–M20 완료 여부 및 미구현 사유 — 2026-11-13

## 범위와 증거 원칙

저장소 `myoungsunk/UWB_CP_ESP`, 브랜치 `codex/mode8-operational-integration-20261113`. 전용 **Mode8 Stage6 200ms / 2Mbps / 앵커 DW1000 하나·태그 DW1000 두 개**. Mode3–7 복원은 범위 제외. 편파별 안테나 지연의 절대 보정·거리 정확도 평가는 후속 작업으로 이월하지만 CIR와 RF 시각·보정 출처는 보존.

**코드 구현/오프라인 검증은 실물 보드 운용 인수와 다르다.** 현재 빌드는 Arduino ESP32 Core3.3.7 앵커/태그 구성으로 GitHub Actions에서 확인 가능. 장치 전원·RF·AP·OTA·ROS 실기는 직접 접근할 수 없으므로 수행했다고 표기하지 않는다.

## 상태표 — 목록 전체

| ID | 개발 완료 여부 | 코드/증거 | 미완료인 이유 또는 실기 조건 |
|---|---|---|---|
| M01 | 소프트웨어 구현 | NVS profile CRC+역할·2Mbps 초기 UART 버퍼/진단 복귀 | 실물 100회 재부팅·UART 부팅 로그 필요 |
| M02 | 부분 구현(운영 가능한 분할·유선 대안) | `ops_plan_long_run.py`, `ops_route.py` / 별도 `ops_wired_continuous.py` | 원격 Wi-Fi-OFF 앵커는 최대32개 이후 저장/회수 공백; 진짜 연속 무손실은 M04 |
| M03 | 호스트 route 구현 | 암호학적 HMAC journal, 점별 session, 재개·동시실행 잠금 | 로봇 구동·현장 재개 시험 미실시 |
| M04 | **미완료 — 하드웨어 제약** | 두 USB 유선 연속 1000거래 대안 준비 | 원격 앵커 RAM 32행 초과 원본을 Wi-Fi OFF로 비휘발성 저장할 동시 간섭 없는 하드웨어/경로 필요 |
| M05 | **미완료 — 물리적 불가능 조건** | 손실을 정상 기록으로 승격하지 않는 fail-closed | 정전 시 RAM 원본은 소멸; UPS 또는 전원 독립 비휘발성 로거 필요 |
| M06 | 호스트/펌웨어 구현 | LittleFS tmp/CRC/rename, 재부팅 저널 복구·격리 | 실제 전원 절단 시험 미실시 |
| M07 | 호스트/펌웨어 구현 | 별도 DELETEKEY, nonce·boot·session·CRC 승인/PURGE | 실물 TCP/USB 공격·비밀키 관리 미검증 |
| M08 | **부분 구현, OTA listener 차단** | `ops_image_preflight.py` 역할·이미지 내부 SHA+빌드 출처 검사, 파티션 경계 | **앵커 `opsOtaCanAttestImage()=false`라 OTA 창이 열리지 않음.** 장치가 후보 바이너리의 서명·역할/해시를 실제 쓰기 전에 검증하는 경로·키 필요 |
| M09 | **부분 구현, rollback 미승인** | 부트로더 rollback 설정이 활성화된 때만 pending verify·selftest 처리 | 실제 bootloader CONFIG, OTA slot, eFuse, 전원 절단/복구 시험 미확보 |
| M10 | **미구현 — 안전성 선행조건 부족** | 태그는 현재 Pi USB 수집/플래시 경로, 이미지 역할 사전검사 가능 | 태그 Wi-Fi OTA 추가 시 **서명 검증·OTA 파티션/부트 복구**, 이중 DW1000/메모리 및 RF 모드 간섭 시험이 선행되어야 함. 안전 키·보드 계약 없이 무인 OTA 활성화 금지 |
| M11 | **소스 구현, 기관 실기 HOLD** | CA 있는 PEAP만 허용하는 앵커 `OPS EAP...` 분기 | 기관 CA PEM·계정·AP 실물 없으면 EAP 로그인 검증 불가. 기본 빌드 EAP OFF |
| M12 | **미구현 — 보안/제조 조건** | HMAC 인증만 존재. Host HTTPS/TLS 서버/인증서·실물 Secure Boot, Flash Encryption 미적용 | 실제 인증서/private key·ESP32 보드별 eFuse 변경은 불가역, bootloader/flash layout·복구 규약 및 물리 승인 필요 |
| M13 | **실험 실행기 구현, 고속 운용 미승인** | `ops_period_sweep.py` 200→195→190ms | 실제 RF RX/TX deadline, UART/CIR, 복구 확률, power/PLL 실측 이전에 기본 주기 변경 금지. 150/100ms는 별도 재설계 |
| M14 | **실물 검증 미실시** | 상태/PLL 오류 로그 및 G5/G6 검증 사양 | 실제 앵커·태그·Pi/AP·전원/스코프가 필요 |
| M15 | **호스트/ROS2 로거 구현, 통합 실기 HOLD** | `ops_ros_capture.py` + `ops_pose_align.py`, 실제 RF40와 ROS 독립 동기화 짝 요구 | 실제 로봇 ROS2 토픽/IMU/odom/공통 사건 수집·검산 필요; 로봇 자율주행 제어는 범위 밖 |
| M16 | **스트레스 실행기 구현, 실물 24h 미검증** | `ops_soak_suite.py` + 기존 ACK/FEC 적대적 재생/계측, no-auto-RF-retry | 실제 USB/전원/PLL/메모리/파일시스템 장기 시험 장비 필요 |
| M17 | **소프트웨어 부분 구현** | 저널 CRC, RF 이전 LittleFS 용량/8KiB 여유 검사, 삭제 방지 | flash wear/정전/파일 파편화는 하드웨어 장기 시험이 필요 |
| M18 | **소스 출처·readback 도구 구현, 설치 본 검증은 HOLD** | `ops_image_preflight.py`, `ops_flash_readback.py` 읽기 전용 실제 앱 바이트 비교, CI BIN/ELF | 사용자가 실제 보드·RUNNING APP 파티션 오프셋·전체 플래시 복구본 제공 후에만 실행. readback 자체는 부트 실행/secure boot 보증 아님 |
| M19 | 원본 메타/관측 보존 구현 | RFB8: A/B cal ID, RF TWR 시각, CIR 원시 FP, 반복 Probe | 안테나 지연값 확정과 절대 정확도는 사용자의 요청대로 후속 |
| M20 | route 원본 재감사 구현 | 재개·PURGE 직전 `tag.rfb8`/anchor archive RF evidence 및 journal SHA 재확인 | 실제 디스크 오염·전원 차단 고장 주입은 G8/G9 HOLD |

## 개발 우선·시험 계약

- **현장 없이 추가 구현 가능했고 이번에 반영한 항목**: M02 route 자동 plan 생성기, M11 CA-gated EAP 접속 코드, M15 ROS2 odom/IMU 로거, M13 195/190ms 단계 시험기, M16 USB 반복 soak, M18 read-only flashed application SHA 대조. 기존 EAP/LAN/ROS/USB 실기 증거가 없으므로 이들을 *물리적 완료*라고 표시하지 않는다.
- **별도 구조/장비/보안키가 먼저 필요한 항목**: M04, M05, M08, M09, M10, M12. 특히 암호화·서명 검증을 생략한 OTA 활성화와 보드 eFuse의 무분별한 변경을 허용하지 않는다.
- **운영 권장**: 현재 앵커 WPA 개인망 + Mode8 정지 point ≤32, Pi 태그 USB 또는 두 USB 유선 연속 실험. eFuse/rollback/기관 EAP는 별도 장치·비밀키 보호·실기 인수 뒤에만 활성화.

## 검증 및 중단 기준

개발 CI는 GitHub Actions의 `host-adversarial` Python/C++ ASan·UBSan과 `esp32-build` 두 역할 컴파일/링크·BIN/ELF 영수증. 순차 시험 G0–G12와 M02/M11/M13/M15/M16/M18 추가 표는 `MODE8_SEQUENTIAL_VERIFICATION_SPEC_KO.md` 참조.

**실험 중단**: RF 물리축/CIR 근거 불일치, 다른 session/boot ACK/PURGE, OTA unsigned candidate가 활성화됨, Wi-Fi ON RF 시작, DEV_ID/PLL/RXENAB fail, 원본 덮어쓰기/삭제, 이상한 A/B 시각 도메인 보정, 190ms에서 period miss, 이미지 flash SHA 실패.

**최종 판정**: SOURCE/HOST/TARGET BUILD PASS는 정확한 최종 커밋의 CI가 성공할 때만. HARDWARE FIELD, ENTERPRISE EAP AUTH, SECURE OTA, ROS LIVE 및 24h SOAK는 실제 장치 원본 없이는 NOT RUN/HOLD.
