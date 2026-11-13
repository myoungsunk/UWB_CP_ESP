# MODE8-DEV-REGISTER-001 — 미반영 기능 전체 관리표

**기준**: \`ce8f986\` Stage6 P2-repeat, \`85c42fe8\` 운영 통합 및 후속 Mode8 전용 개발.
**범위 확정**: RF는 Mode 8 전용. 과거 Mode 3–7 및 R03 이전 패킷/479B 저장 형식 호환은 요구하지 않는다. 안테나 지연의 절대 보정/거리 정확도 검증은 후속 연구로 분리하되 실제 TX/RX 보정값과 raw CIR/FP, cal_id는 지금부터 누락 없이 보존한다.

**판정 분류**: IMPLEMENTED(실제 소스와 자동 시험 존재), PARTIAL(작동 범위 제한), HOLD(하드웨어 또는 추가 구조가 필요), NOT_STARTED. GitHub Actions의 성공은 RF·전원·OTA 실물 시험을 대신하지 않는다.

| ID | 미반영/확장 요구 | 현재 개발 판정 | 사용자가 수용할 기능 계약 / 차단 요소 |
|---|---|---|---|
| M01 | 재부팅 뒤 Mode8 Stage6 200ms·2Mbps 운영 설정 복원 | IMPLEMENTED / FIELD HOLD | 역할·CRC·버전 검사 후 선택적 복원; 부패하면 Stage5/921600 진단. 자동 RF 시작 금지. 초기 \`PROFILE SAVE\` 필요 |
| M02 | 32거래 초과 장기 측정 | PARTIAL / HOST IMPLEMENTED | `ops_plan_long_run.py`로 32거래 이하 point 자동 분할, `ops_route.py`의 검증·회수·재개 수행; point 사이 RF 중단 있음. 두 USB 유선 연속 수집은 M24 별도 |
| M03 | 다중 route/point 관리, crash resume 및 유일 식별 | IMPLEMENTED / FIELD HOLD | \`ops_route.py\` 의 영속 journal·고유 세션·완료 검증·재개. ROS 로봇 주행은 구현하지 않음 |
| M04 | 무중단 1000거래(5 Hz) Wi-Fi OFF RF와 동시에 앵커 원본 보존 | HOLD — 기존 ESP32 내부 RAM 제한 | RF 미간섭 외부/분리 저장 또는 별도 데이터 회수 하드웨어 필요. USB 실험용 1000거래와 운영형은 별개 |
| M05 | 측정 중 전원 차단 시 미커밋 RAM 원본 무손실 | HOLD — 물리적으로 RAM 보존 불가 | 전원 차단 뒤 진짜 raw 원본을 복원하려면 UPS/비휘발성 로깅 필요. 현재는 fail-closed와 ≤32거래 손실 범위 분명히 표시 |
| M06 | 커밋 저널의 중단/CRC 복구, 자동 덮어쓰기 방지 | IMPLEMENTED / FIELD HOLD | tmp 검증·원자 rename, 손상 tmp 보존/격리; 실제 플래시 정전 주입 미실시 |
| M07 | 파일 ACK/삭제 권한의 구분, 승인 없는 삭제 금지 | IMPLEMENTED / FIELD HOLD | 별도 USB DELETEKEY, 세션·boot·CRC 및 연결 nonce와 ACK/PURGE 용도별 HMAC. 실물 보안 시험 필요 |
| M08 | OTA 이미지 역할·해시 검증 | PARTIAL / OTA RECEIVER LOCKED | 호스트 `ops_image_preflight.py`에서 앵커/태그 역할·SHA 검증. 앵커 `opsOtaCanAttestImage()`가 false라 실제 ArduinoOTA 포트는 의도적으로 차단. 서명 공개키·장치 검증/롤백 통합 전 활성화 금지 |
| M09 | OTA 실패 시 자동 rollback | PARTIAL / HARDWARE BLOCKED | 조건부 `ESP_OTA_IMG_PENDING_VERIFY` 부팅 자가검사/partition guard 소스 존재. 실제 롤백 활성 부트로더·efuse/파티션·정전 시험 없음 |
| M10 | 태그 자체 Wi-Fi OTA | NOT IMPLEMENTED / SAFETY HOLD | 현재 태그는 USB/연결된 Pi로 유지보수. tag Wi-Fi 추가 시 이중 DW1000 메모리/스케줄 검증 및 별도 역할 서명키/안전 부트 필요. 안전 조건 미확정 상태에서는 OTA listener 개방 금지 |
| M11 | Enterprise Wi-Fi(EAP) | PARTIAL / SOURCE IMPLEMENTED | 앵커 `OPS EAPID/EAPUSER/EAPPASS`, `OPS NET EAP 0/1` 구현. 기관 CA를 컴파일에서 주입하지 않으면 PEAP 거부. 실제 기관 CA/AP 인증·NVS 암호화 미검증 |
| M12 | TLS·Secure Boot·Flash Encryption | NOT IMPLEMENTED / DEPLOYMENT SECURITY HOLD | 현 TCP는 LAN HMAC 인증(기밀성 없음). TLS 서버 인증서/개인키·ESP32 heap 확보, eFuse 불가역 provision/복구 규약·서명부트로더 필요; 임의 기본키/보안 비활성화로 대체 금지 |
| M13 | 200ms 미만 측정 주기 | TRIAL CODE / FIELD NOT APPROVED | `ops_period_sweep.py` 200→195→190ms 실기 시험 실행기 추가, 에러 시 더 짧은 시험 중단. 150/100ms는 프로토콜·UART 일정 재설계 필요 |
| M14 | Wi-Fi OFF/ON, PLL, RXENAB, 바운더리, 32거래 RAM 실기 검증 | FIELD HOLD | 반복 전원/수신, heap 최저치, brownout/WDT, 임시 파일 손실, 200ms 지터, raw CIR |
| M15 | 이동 중 odom/IMU/ROS 연계 | PARTIAL / HOST IMPLEMENTED | `ops_ros_capture.py` ROS2 odom/IMU 원본 수집, `ops_pose_align.py` 독립 RF-ROS 동기화 표본이 있을 때만 보간. 동기화 하드웨어 및 실제 로봇 이동 검증·ROS 주행제어 미구현 |
| M16 | USB·ACK·FEC 장기 스트레스 | PARTIAL / TEST RUNNER IMPLEMENTED | `ops_soak_suite.py` 여러 유선 1000거래 원본·SHA·저널 수집, 중간 실패 시 RF 자동 재시도 금지. 실제 1h/24h 장치 스트레스/손실률 미실시 |
| M17 | 플래시 내구성·정전 복구 | PARTIAL / FIELD HOLD | LittleFS temp/CRC/no-autoformat, 사전 용량+8KiB 예약 검사 구현. 1000회 쓰기·정전/전압·실제 wear 증거는 없음 |
| M18 | 이미지 SHA·배포 출처·인수 기록 | PARTIAL / READBACK TOOL IMPLEMENTED | `ops_flash_readback.py`가 장치 실행 partition offset을 별도 확인한 뒤 읽기 전용으로 바이트 대조; `ops_image_preflight.py` 빌드 provenance 검사. 실보드 실행 SHA·플래시 전체 백업·부트 실행 증거 없음 |
| M19 | 안테나 A/B TX/RX 지연값과 보정 출처의 데이터 보존 | IMPLEMENTED(원본/메타 유지) | 실험 임시값은 calibrated=false를 유지. 정확도 시험·보정값 확정은 범위 제외 |
| EX-01 | 기존 Mode3–7/Legacy R03 프로토콜 | OUT OF SCOPE | 구현·호환 강요 금지 |
| EX-02 | DS-TWR 절대 정확도 승인 | DEFERRED | LoS 다중 거리·편파·안테나 지연 보정 이후 수행 |

## 구현 우선 순서 및 검증 문턱

- **G0**: 기준 SHA, 패킷/기록 스키마 고정 및 숨은 코드 복사본 검사
- **G1**: M01, M07 호스트/펌웨어 변경과 음성 입력·전원 취소 Mock/적대적 시험
- **G2**: M02/M03 point journal·다운로드·ACK·삭제 순서 검증(수집 공백이 있음을 명시)
- **G3**: M06, M08/M09 OTA/파일 파티션 오류 경로를 코드에 추가하고 안전한 차단 조건을 확인
- **G4**: ESP32 Arduino Core 3.3.7 앵커/태그 별도 컴파일/링크·BIN/ELF·메모리 확인
- **G5**: 실제 플래시/부팅/PLL/Wi-Fi RF 32거래, Pi/RFB8·저장 양단 감사
- **G6**: 정전·OTA·통신 장애 주입. 미검증 오류를 pass 처리 금지.
- **G7**: 연속 1000거래/무중단 문제는 M04/M05 하드웨어 계약 충족 후 새 승인.

**운영 금지**: G4 통과만으로 통합형 연구실 운영 펌웨어 승인을 표시하지 않는다. MID-MEASUREMENT 파워컷의 데이터 유실을 숨기는 "복구 완료" 표현을 금지한다.

## 2026-11-13 추가 데이터 보존 결함

**M20 — route 재시작 이후 로컬 원본 재감사(IMPLEMENTED / FIELD HOLD)**: 기존에는 `operational_receipt.json`의 과거 `paired_audit` 결과를 신뢰하고 파일 존재·현재 SHA만 확인했으므로, 기록이 나중에 손상된 경우에도 다음 point/PURGE를 진행할 수 있었다. `verify_point()`에서 `audit_archived_session()`을 다시 실행하고 기존 journal SHA가 존재하면 바이트 일치를 필수화했다. `purge()` 진입 직전에도 같은 검사를 수행한다. 자동 시험 `test_preverified_receipt_does_not_replace_raw_replay`, `test_verified_route_rejects_changed_hash_before_purge` 추가. 이 결과는 모의 실행 기준이며 실기 재개·저장장치 고장 주입은 G8/G9에 남는다.

**M17 추가 구현 (2026-11-13)** — RF 시작 전 LittleFS `totalBytes/usedBytes`와 요청한 아카이브 크기(`32B header + count×1312B row`) 및 8192B 보수 여유를 계산하여 용량이 부족하면 `START_GUARD`로 거부한다. 저장 전 공간 검사는 정전·동시 쓰기·실제 flash erase 내구를 보장하지 않으며 G9/G11 물리 검증은 계속 HOLD이다.

### 추가 구현 M21–M26 (2026-11-13)

| ID | 개발 내용 | 코드 및 판정 |
|---|---|---|
| M21 | route journal HMAC·불가능한 상태/세션/순서 검증 | IMPLEMENTED / FIELD HOLD: `ops_route.py` 서명 검증, 자동 재실행 금지 |
| M22 | 동일 route 복수 실행기 경쟁 방지 | IMPLEMENTED / FIELD HOLD: POSIX/Windows 비차단 단일 소유 파일 잠금 |
| M23 | 영속 Stage6/2Mbps UART 부팅 송신 경로 | IMPLEMENTED / FIELD HOLD: HELLO/STOP 초기 조각 송신 버퍼 사전 준비, 실패 시 진단 복귀; 실제 재부팅 검증 필요 |
| M24 | 유선 앵커+태그 두 USB의 연속 1,000거래 대안 | IMPLEMENTED(host wrapper) / FIELD HOLD: `ops_wired_continuous.py`; 무선 단독 앵커의 M04 문제를 해결하지는 않음 |
| M25 | RF/A/B/ROS 시간·odom 정합 도구 | IMPLEMENTED(host) / FIELD HOLD: `ops_pose_align.py`; 독립 물리 동기화 사건이 없으면 정렬 거부, 실제 ROS 시간측정 미실시 |
| M26 | G0–G12 원시 증거 SHA·순서 검증 도구 | IMPLEMENTED(host) / FIELD HOLD: `ops_acceptance_gate.py`는 증거를 검산하지만 독립 실물 인수를 자동 PASS 처리하지 않음 |

M17 선행 개선: LittleFS 가용 용량이 아카이브+8KiB 보수 여유보다 부족하면 RF 이전에 START 거부. 하드웨어 wear/정전 시험은 HOLD 유지.

### M27 — 전용 유선 수집 시 앵커 Wi-Fi 재부팅 정책

**IMPLEMENTED / FIELD HOLD**. 앵커 USB 정비 명령 `OPS NET AUTOSTART 0`은 NVS에 Wi-Fi 자동 연결 금지를 저장하고 현재 네트워크를 OFF로 만든다. 앱을 재부팅하거나 USB 포트를 다시 열어도 자동 Wi-Fi 복귀를 시도하지 않는다. 원격 정비로 돌아갈 때 USB에서 `OPS NET AUTOSTART 1`을 실행한다. 디폴트는 기존처럼 ON이며 단독 USB 실험에서만 OFF를 선택한다. 저장 실패·Wi-Fi OFF 실패를 정상으로 오인하지 않고 오류 응답으로 나타낸다. `testWifiAutostartIsPersistentAndExplicit` Mock, CI 타깃 빌드 검증만 수행. 실물 NVS·AP·UART 재부팅 반복 시험은 별도.

**M11 코드 보완 (2026-11-13)**: 앵커에 PEAP Enterprise 구성용 `OPS EAPID/EAPUSER/EAPPASS` USB 프로비저닝과 `OPS NET EAP 0/1` 스위치를 추가했다. 기관 CA가 컴파일 시 `MODE8_EAP_CA_CERT_PEM`으로 제공되지 않으면 Enterprise 활성화가 실패하도록 했다. CA·인증서/계정·실물 AP가 없는 기본 이미지는 WPA 개인망만 사용한다. 타깃 컴파일 통과는 실제 EAP 인증 성공을 보장하지 않는다.

### 2026-11-13 M01–M20 구현 판정의 근거

세부 근거와 미구현 사유: `MODE8_M01_M20_IMPLEMENTATION_REVIEW_20261113_KO.md`. 컴파일·호스트 QA는 각 Git SHA의 GitHub Actions 성공만을 근거로 표시하고, 실물 RF·Pi·OTA·정전·EAP·ROS 주행 검증은 `NOT_RUN`으로 분리한다. M04/M05/M10/M12는 현재 데이터/안전 전제 없이 “구현 완료”라고 판정하지 않는다.
