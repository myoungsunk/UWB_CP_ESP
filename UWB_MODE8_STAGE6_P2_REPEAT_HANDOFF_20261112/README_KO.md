# UWB Mode 8 Stage 6 펌웨어 및 실기 로그

2026-11-12 Stage 6의 최종 P2 복구 수정본입니다. 시험은 COM4 앵커와 Pi에 USB로 연결된 태그에서 **200 ms, USB UART 2 Mbps**로 진행했습니다.

## 구성

| 경로 | 내용 |
|---|---|
| firmware/anchor_p2_repeat | 앵커 소스, 라이브러리, 시험, 빌드 영수증과 ESP32 산출물 |
| firmware/tag_p2_repeat | 태그 소스, 라이브러리, 시험, 빌드 영수증과 ESP32 산출물 |
| logs/01_final_pass | 최종 1,000거래 원본 RFB8, USB UART, JSONL, SELFTEST 및 수집 영수증 |
| logs/02_p2_repeat_recovery | P2 재수신이 실제로 사용된 첫 1,000거래 원본과 영수증 |
| logs/03_tag_startup_fault | 태그 A의 PLL 보호 정지와 복구 점검 로그 |
| logs/04_baseline_reference | 수정 전 txn 781 및 송신 건너뜀의 원본 JSONL 발췌와 영수증 |
| logs/05_spi_guard_reference | SPI 및 앵커 큐 수정 후 남은 P2 누락의 원본 JSONL 발췌와 영수증 |
| provenance | 플래시·읽기 검증 영수증, 임시 보정 프로파일, 실기 실행에 사용한 도구 |
| tools/p2_repeat_tools | 수집 세션 검증, CIR 분석, P2 회귀시험 |
| RUN_INDEX.json, SHA256SUMS.json | 원본 경로와 포함 범위, 파일별 SHA-256 |

실기 원본은 형식 변환 없이 복사했습니다. 비교 실행 두 건은 영수증과 관련 거래만 수록했습니다. 생성된 Arduino 빌드 트리와 별도 보관 중인 이전 플래시 백업은 용량과 중복을 줄이기 위해 생략했습니다. 영수증에는 당시 장치와 작업 경로가 기록되어 있습니다.

## 결함과 확인 결과

- 수정 전 txn 781의 태그 A는 P2 수신에서 DW1000의 RXRFSL을 기록했습니다. 앵커는 해당 P2를 예약한 시각에 송신했습니다. 앵커가 거래를 13회 건너뛴 사유는 수집 큐의 역압력으로 기록됐습니다.
- 최종 소스는 Stage 6에서 P2 복구 기회를 한 번 제공하며, 태그는 수신한 복사본의 실제 송신 시각과 모듈 증거로 CIR과 거리축을 검증합니다. 앵커는 직렬 전송을 분할하고 RF 작업 사이에 처리해 큐 정체를 줄입니다.
- 첫 1,000거래에서 P2 재수신으로 txn 366의 B와 txn 797의 A 관측을 복구했습니다. 공통 지연축과 전력 그리드는 P1 **999/1,000**, P2 **1,000/1,000**이었습니다.
- 이어진 시작 시도에서 태그 A에 SYS_STATUS 0x02800002와 RF_STATUS 0x03이 기록돼 보호 정지했습니다. 해당 시도는 거래 시작 전에 중단됐습니다.
- 재시작 후 최종 1,000거래에서 공통 지연축과 전력 그리드는 P1·P2 각각 **1,000/1,000**이었습니다. P1 한 건은 RESPONSE 기반 재측정으로 복구됐고, P2 1,000건은 최초 수신으로 확보했습니다.
- 최종 수집의 RFB8 누락, RFB8 CRC 오류, 복원 실패, 펌웨어 주기 누락, 실측 주기 초과는 모두 **0건**입니다. 999개 POLL 간격은 **199.879~200.126 ms**였습니다. USB 조각 CRC 오류는 앵커 **2,383건**, 태그 **5,609건**이었고 모두 FEC로 복구됐습니다.
- 이 판정은 해당 보드와 배치의 수집 결과입니다. 임시 안테나 지연을 사용했으며 절대 거리 정확도는 검증 항목에 포함하지 않았습니다.

## 검증과 빌드

압축을 푼 뒤 패키지 루트에서 실행합니다. 오프라인 재생과 회귀시험은 Python 3.10+와 NumPy를 사용합니다. 해시 및 빌드 산출물 검증만 실행할 때는 Python 표준 라이브러리로 충분합니다.

```powershell
py -3 -B -X utf8 verify_package.py
py -3 -B -X utf8 verify_package.py --replay
py -3 -B -X utf8 tools/p2_repeat_tools/test_p2_repeat.py
```

재생은 영수증에 기록된 세션의 거래만 골라 CIR 무결성, 양단 시각 증거, 실측 주기와 P1·P2 그리드를 다시 계산합니다. USB 실기 집계는 원본 영수증에서 읽습니다. 세션 전후 제어 프레임도 원본 파일에 보존돼 있습니다.

ESP32 빌드 환경은 Arduino ESP32 Core 3.3.7, FQBN esp32:esp32:esp32doit-devkit-v1입니다. 보존된 영수증과 산출물에 대한 검증을 마친 뒤, 재빌드는 별도 작업 복사본에서 아래처럼 수행합니다.

```powershell
Push-Location firmware/anchor_p2_repeat
py -3 -B -X utf8 tools/build.py --role anchor
Pop-Location

Push-Location firmware/tag_p2_repeat
py -3 -B -X utf8 tools/build.py --role tag
Pop-Location
```

보드에 실제 기록한 앱의 파일명은 ANCHOR_DUAL_DS_CIR.ino.bin과 TAG_DUAL_DS_CIR.ino.bin입니다. 같은 dist 역할 폴더에 ELF, MAP, 파티션, 부트로더, 병합 이미지와 컴파일 로그를 함께 보관했습니다.
