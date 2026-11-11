# Dual-DW1000 Mode 8 — 구현 소스 및 호스트 검증 패키지

**릴리스** `D8_R3_20261111`  
**상태** 다섯 결함의 호스트 적대적 재감사 통과 · 앵커·태그 ESP32 컴파일 성공 · 실물 시험 대기

## 먼저 읽을 보고서

**[R3 적대적 재감사](docs/ADVERSARIAL_REAUDIT_R3_KO.md)**에 다섯 결함의 수정 내역, 시험 결과, 이전 R2 반례 재현을 기록했습니다. R1과 R2 보고서는 해당 릴리스의 이력입니다.

## 가장 먼저 확인할 사항

이 패키지는 USB 직렬 제어용 앵커·태그 Mode 8 R3 소스와 `dist/anchor/`, `dist/tag/`의 ESP32 `.bin`, `.elf`, `.map`을 담고 있습니다. 보드 정의는 `esp32:esp32:esp32doit-devkit-v1`, Arduino ESP32 Core는 3.3.7입니다. 실물 보드 업로드와 RF 시험은 대기 상태입니다. [타깃 빌드 기록](dist/target_build_receipt.json)

원본 R1L80 ZIP에 장치 등록 비밀 키가 없으므로, 더미 키를 넣거나 기존 인증을 무력화하지 않고 **USB 직렬 제어 전용의 별도 측정 펌웨어**로 분리했습니다. 이 새 바이너리 대상에는 기존 Wi-Fi/TCP/OTA, 경로 제어, RFB5 및 Mode 3–7이 들어 있지 않습니다. 기존 소스는 `evidence/BASELINE_R1L80_UNMODIFIED.zip`에 그대로 보존했으며, 새 Mode 8 내부의 레거시 회귀시험이 완료됐다는 뜻은 아닙니다.

## 구현 범위

- 태그 DW1000-A/B 독립 초기화, CS·RSTn 및 수신 상태 분리. RF 스위치 제어 없음.
- 같은 POLL/P1/P2를 A/B에서 수신하고, RX 자동 재활성화와 이중 버퍼를 끈 상태에서 CIR을 순차 복사.
- 한 거래: POLL → RESPONSE_A → RESPONSE_B → Probe 1 → Probe 2.
- A/B RESPONSE는 각 모듈 자신의 POLL 수신 시각으로 예약. A/B 원시 시계를 서로 빼지 않음.
- Probe 1을 FINAL로 사용하는 독립 비대칭 DS-TWR. Probe 2가 P1의 실제 송신 시각을 확인해야 최종 거리 유효 처리.
- A1/B1/A2/B2 CIR 각 992 복소 샘플, 총 15,872 B. 반복 읽기 일치 및 CRC32 기록.
- 원시 CIR, FP 상대축, DS 기반 물리 지연 **추정축**, 공통 그리드 전력 보간 결과를 PC에서 생성.
- 한쪽 POLL/Probe/RESPONSE 실패 시 성공한 쪽 관측을 보존. 하드웨어 무결성 오류는 래치 후 중단.
- RFB8 바이너리 기록, 3개 결과 슬롯, 별도 출력 작업, 역압력 및 UART 처리량 제한.

**공유 SPI를 사용할 뿐, 두 RF 클록 또는 복소 위상을 동기화한 시스템이 아닙니다.** 물리 지연축은 DS-TWR/FP와 보정 상태에 의존하는 추정값입니다.

## 확정 핀맵

| 신호 | 태그 A | 태그 B |
|---|---:|---:|
| SCK | GPIO18 | GPIO18 |
| MISO | GPIO19 | GPIO19 |
| MOSI | GPIO23 | GPIO23 |
| CS | GPIO21 | GPIO25 |
| IRQ | GPIO22 | GPIO26 |
| RSTn | GPIO17 / TX2 | GPIO27 |
| 전원·접지 | 3V3 / GND | 3V3 / GND |

태그 GPIO4/5는 사용하지 않습니다. **앵커는 원본 소스의 CS21/IRQ22/RST5를 유지합니다. 앵커 GPIO5는 RF 스위치가 아니라 RSTn입니다.** IRQ는 입력으로 확보하되 이 구현은 폴링 방식이며, ISR 안에서 SPI를 호출하지 않습니다.

## 폴더 안내

| 위치 | 내용 |
|---|---|
| `firmware/ANCHOR_DUAL_DS_CIR/` | 앵커 스케치 및 공통 헤더 사본 |
| `firmware/TAG_DUAL_DS_CIR/` | 태그 스케치 및 공통 헤더 사본 |
| `shared/` | 두 스케치의 원본 공통 코드. 수정 후 sync 실행 |
| `vendor/anchor`, `vendor/tag` | 원본 역할별 DW1000Ng. 변경 없이 사용 |
| `tools/build.py`, `upload.py` | 실제 타깃 빌드·해시 검증·업로드 |
| `tools/run_pair.py`, `capture_audit.py` | 세션·양쪽 boot·거래·CIR 무결성을 최종 판정에 반영하는 엄격한 수집기 |
| `tools/rfb8.py`, `analyze.py` | 스트림 검증, 거리 독립 검산, CIR/시간축 NPZ·CSV |
| `tests/` | 순수 C++ 합성 시험, Python 도구 시험, 호스트 전용 선언 스텁 |
| `evidence/host/` | 실제 실행한 호스트 컴파일·시험 로그 및 **합성** 예제 |
| `dist/target_build_receipt.json` | ESP32 빌드 결과, 대상 보드와 산출물 해시 |
| `docs/` | 단계별 판정, 프로토콜, 빌드, 하드웨어 검증 안내 |

## 실제 실행한 검증

GCC의 C++ 합성 검사 **11,413개 assertion**과 Python 시험 **54개**를 통과했습니다. GCC 실행에서 ASan·UBSan 오류 보고가 없었고, 앵커·태그 구문 검사도 통과했습니다. [R3 재감사 기록](docs/ADVERSARIAL_REAUDIT_R3_KO.md)

`SYNTHETIC_ONLY*`, `SYNTHETIC_CIR_i16.raw`, `synthetic_tag_record.json`은 합성 데이터입니다.

## 다음 실행 순서

빌드와 업로드 절차는 [빌드 안내](docs/BUILD_AND_FLASH_KO.md)를 참고합니다. 기존 보드의 롤백 방법을 확보한 뒤 역할을 확인하고, 업로드 후 [하드웨어 인수 절차](docs/HARDWARE_ACCEPTANCE_KO.md)를 진행합니다.

기본값은 STAGE 5, 300 ms 반복주기, 921600 baud, 20 MHz CIR SPI 및 반복 읽기 검증입니다. 0/7/14/19/51 ms는 **명목 패킷 배치**이며 실측 처리시간이나 51 ms 반복주기가 아닙니다. 전체 CIR과 메타데이터를 계속 전송하는 100 ms 주기는 현재 직렬 출력 예산에서 거부합니다.

앵커·태그와 수집기는 R3 조합으로 사용합니다. `capture_receipt.json`은 기록 검증 결과를 `audit`에, 마지막 정지 요청의 송신·응답 결과를 `shutdown`에 담습니다. 양쪽 항목이 통과해야 전체 `pass=true`가 됩니다.

## 배포 무결성

수정·재빌드하기 전에 `python tools/verify_package.py`로 전달 파일을 검사할 수 있습니다. `PACKAGE_SHA256.json`은 배포 시점의 소스와 로그를 고정합니다. 빌드·시험을 다시 수행하면 해당 로그/receipt가 바뀌므로 그 이후 해시 차이는 새 실행 결과와 구분해서 해석해야 합니다.
