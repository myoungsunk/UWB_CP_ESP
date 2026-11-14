# MODE8-IMPLEMENTATION-ACCEPTANCE-20261113 — 전체 개발 항목 및 순차 검증 인수 보고

**최초 기준**: `ce8f986793c5a12be0a020df924f05b516edf70e`  
**재감사 기준**: `85c42fe8db93b76c26104a6b67e8a39ca07bd882`  
**본 보고 개발 커밋**: `bd8da588a2b05299af2dedb1dcb8c5e2d6229e4b`  
**증거**: [GitHub Actions 43383293287](https://github.com/myoungsunk/UWB_CP_ESP/actions/runs/43383293287) — host-adversarial 및 esp32-build 모두 SUCCESS, Python 35 tests PASS, GCC ASan/UBSan portable C++ 테스트·앵커/태그 Arduino ESP32 Core 3.3.7 빌드, BIN/ELF 아티팩트 게시.

## 1. 범위

**Mode 8 전용**: 이전 Mode 3–7/R03 프로토콜의 복원은 요구하지 않는다. 안테나 TX/RX 지연 정밀 보정과 절대거리 RMSE는 후속 연구로 제외한다. 다만 측정값, calibration ID, raw CIR/FP, RF timestamp 보존은 유지한다.

**운영 최소 범위**: Wi-Fi OFF 중 단일 point ≤32거래, 반복 point는 Pi route 저널을 사용하여 세션을 분리하고 RF 사이 공백을 명시한다. 1,000거래 무중단 5 Hz 운영을 달성했다는 의미가 아니다.

## 2. 전수 개발 상태 (M01–M20)

| ID | 기능 | 현재 소스 판정 | 실제 물리 인수 |
|---|---|---|---|
| M01 | Mode8 Stage6 200ms/2Mbps 운영 프로파일 영속화, CRC·역할 검사·수동 SAVE | IMPLEMENTED | HOLD |
| M02 | 32거래 초과 수집 | PARTIAL: 32거래 단위 순차 운용/회수 | HOLD |
| M03 | Mode8 전용 다중 point route journal, 중복·재시작 보호 | IMPLEMENTED | HOLD |
| M04 | 중단 없는 1,000거래 5Hz RF+앵커 비휘발성 보존 | BLOCKED: 외부 비간섭 저장 설계/하드웨어 필요 | HOLD |
| M05 | 측정 중 전원 차단에도 RAM에 있던 데이터 무손실 | BLOCKED: UPS/비휘발성 보존 필요 | HOLD |
| M06 | LittleFS 저널 검산·복구·임의 덮어쓰기 금지 | IMPLEMENTED | HOLD |
| M07 | 파일 삭제·ACK 별도 승인키, nonce/session/boot/CRC 결합 | IMPLEMENTED | HOLD |
| M08 | OTA 이미지 역할·해시 검증 | PARTIAL: 호스트 사전검사/파티션 guard; 장치 강제 서명 아님 | HOLD |
| M09 | 부트로더 레벨 안전 rollback과 업데이트 실패 복구 | PARTIAL: 조건부 첫부팅 health 검사; rollback 설정/실물 증거 없음 | HOLD |
| M10 | 태그 무선 OTA | NOT IMPLEMENTED, 선택 항목: Pi USB 업데이트로 운용 | NOT APPLICABLE / 필요 시 별도 |
| M11 | WPA Enterprise(EAP) | NOT IMPLEMENTED, 선택 항목: 격리 WPA 개인망 사용 | NOT APPLICABLE / 필요 시 별도 |
| M12 | TLS, Secure Boot, Flash Encryption | NOT IMPLEMENTED, 배포 보안 옵션: HMAC은 기밀성 보장 안 함 | HOLD |
| M13 | 200ms 미만 195/190ms, 150/100ms 고속 운용 | NOT APPROVED: RF/USB 타이밍 재검증 필요 | HOLD |
| M14 | 실제 Wi-Fi OFF/PLL/RXENAB/메모리/전원 검사 | 코드 검증 일부, 장치 시험 미실시 | NOT RUN |
| M15 | ROS/odom/IMU 이동 중 동기화 | 미통합: Pi 데이터에는 RF 실제 시각 보존, 이동 현장 검증 필요 | NOT RUN |
| M16 | ACK/FEC/USB/TCP 지연 스트레스 | 일부 호스트 결함 주입, 물리 24h 운용 미실시 | HOLD |
| M17 | 플래시 수명/부족 공간/전원차단 | CRC·no-autoformat guard, 실제 쓰기 내구 실험 미실시 | HOLD |
| M18 | SHA/비밀키/이미지 출처/현장 영수증 | CI 산출물·매니페스트/호스트 preflight 구현, 현장 SHA 미확인 | HOLD |
| M19 | A/B 지연값·cal ID·raw CIR/FP/시각 보존 | IMPLEMENTED, 절대 accuracy 승인과 분리 | HOLD |
| **M20** | route 복구/삭제 전 tag.rfb8 및 앵커 기록 재감사 | **IMPLEMENTED** 이번 변경: 파일 재생, 기존 journal SHA 비교, 손상 시 STOP | HOLD |

`IMPLEMENTED`는 소스 및 자동 시험 수준을 의미한다. `HOLD`를 `PASS`로 바꾸려면 아래 G 단계의 실제 증거가 필요하다.

## 3. 신규로 발견한 적대적 반례와 수정

**위험**: 이전 `ops_route.py::verify_point()`는 `operational_receipt.json`의 과거 `paired_audit` 값을 확인하고 `tag.rfb8`·앵커 파일이 존재하면 현재 SHA만 새로 계산했다. 데이터가 최초 저장 이후 손상되더라도, route 재개 후 `PURGE`가 앵커 측 유일 보관본을 삭제할 수 있는 경로였다.

**수정** (`bd8da588`):
1. `verify_point()`는 `ops_session.audit_archived_session()`으로 원시 RFB8 및 anchor archive를 다시 열고 P1/P2·양단 provenance·CIR 무결성을 재검산한다.
2. 처음 검증한 route journal에 해시/boot/CRC가 이미 있다면 *새 해시로 덮어쓰지 않고* 일치 여부를 검사해 불일치 시 `RouteHold` 처리한다.
3. `purge()`는 `VERIFIED/PURGE_INTENT` 상태일 때만 호출 가능하고, 승인된 원본을 다시 검산하기 전에는 원격 삭제를 시작하지 않는다.
4. 거짓 PASS 영수증 + 유효하지 않은 RFB8 데이터, 검증 이후 SHA 변경 시 거부 사례를 회귀 시험에 추가했다.

**검증**: 최종 CI #43383293287의 Python 35 tests PASS; C++ ASan/UBSan 및 ESP32 Core 3.3.7 두 역할 빌드 PASS. 추가 시험은 호스트 기반이며 Pi 디스크 실제 손상/정전 주입으로 실기 승인한 것은 아니다.

## 4. 순차 인수 조건 (실행 필수)

자세한 단계별 입력·중단 조건·증거 스키마는 `MODE8_SEQUENTIAL_VERIFICATION_SPEC_KO.md` 참조.

| Gate | 실행 내용 | 인수 조건 |
|---|---|---|
| G0 | Git SHA, 보존본/소스·스케치 일치, 유효한 최신 이미지/파티션 | 불일치 0 |
| G1 | profile CRC·role·baud/부팅, Stop/Drain/ACK 경계 | 다른 세션 오승인 0 |
| G2 | Mode8 route 계획·재시작·호스트 원본 재감사 | 같은 세션 중복 RF START 0; 변조 원본 삭제 0 |
| G3 | TCP 인증·별도 승인·NVS 저널·GET/ACK/PURGE 오류 주입 | 원본 손상/다른 boot·CRC 삭제 0 |
| G4 | 앵커·태그 ESP32 3.3.7 BIN/ELF·메모리 로그 | 호스트·빌드 PASS |
| G5 | 실제 2 DW1000, DEV_ID/SYS_TIME, SPI/전원/PLL | 하드웨어 장애 및 원시 레지스터 불일치 0 |
| G6 | 정지 32거래 × 10 runs, 실제 선택된 P1/P2/반복 시각 검산 | 각 실행 32/32 공통축·CRC 및 타임스탬프 근거 확인, 주기 ±4 ms |
| G7 | Wi-Fi OFF 측정 → LittleFS commit → 재연결 → Pi GET·승인 | Wi-Fi ON 중 RF START 0, 원본 양단 일치, 잘못된 PURGE 0 |
| G8 | 32+32+20·10×32 point route/재시작 및 현재 SHA 재감사 | 누락 point 자동 재측정 0, 부패 파일 삭제 0 |
| G9 | Armed/Measure/Draining/tmp/rename/ACK 단계 정전 | 손실된 RAM을 회복된 것처럼 승인하지 않음, 기존 파일 덮어쓰기 0 |
| G10 | OTA 잘못된 이미지/역할/전송 중단/부트 건강 | 실제 안전 rollback 승인 전 HOLD, 원본 파일 보호 |
| G11 | 1h/24h 반복·큐/heap/USB/FEC·재연결 | 최초 실패/복구 후 보존율 구분, 원본 byte 동일 |
| G12 | 로봇 이동(0.2 m/s)·회전·IMU/odom/Gt | P2 반복 시각 재매핑, 실패 원인 분리 |

현재 **G0–G4의 자동 테스트/빌드 범위만 PASS, G5–G12는 NOT RUN / FIELD HOLD**다. G10의 rollback, G11의 무손실·내구성은 현재 코드/하드웨어로 PASS를 선언할 수 없다.

## 5. 장비·하드웨어 차단 및 우회

- **원천 무중단 1,000거래**: 32개 RAM 기록 후 Wi-Fi 복귀와 파일 회수 동안 RF가 멈춘다. 연속 5 Hz 필수이면 장치 상시 USB/Raspberry Pi 또는 SD/외부 저장, 별도 시각·전원·SPI 버스 설계가 필요하다. 단순 host 코드를 더해 해결된 것으로 보고하지 않는다.
- **측정 중 정전 완전 복구**: RAM 측정 데이터는 복구 불가. UPS 또는 비휘발성 저널을 설계하고 플래시 쓰기가 RF 타이밍에 영향을 주지 않는지 실물 검증해야 한다.
- **태그 OTA/EAP/TLS**: 요청 조건에 필요한지 확인되지 않아 자동으로 활성화하지 않는다. 별도 드라이버/자격증명/인증서 및 ESP32 메모리·파티션 계약을 정한 뒤 개발·실기 검증한다.
- **장치 OTA 역할 서명/rollback**: 호스트 검증만 우회 가능한 설계이므로 격리된 네트워크에서만 사용하고 안전한 전체 플래시 백업 없이는 OTA 테스트를 수행하지 않는다.

## 6. 배포·인수 최종 상태

- **GitHub 소스/호스트 검증/앵커·태그 타깃 빌드**: PASS (CI #43383293287).
- **사용자 실물 ESP32 두 대·Pi/AP/RF/OTA 검증**: NOT RUN.
- **구형 R03/Modes 3–7 및 절대거리 안테나 보정**: 사용자 결정으로 범위 제외.
- **전체 운영 승인**: HOLD; 단일 정지 point ≤32거래는 CI를 통과한 **실험용 후보**로만 취급.
