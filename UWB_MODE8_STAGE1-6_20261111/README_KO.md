# UWB Mode 8 Stage 1–6 펌웨어·로그·원시 데이터 인계

## 512 MB 이하 압축본

파일 바이트가 동일한 사본을 통합했다. `DUPLICATE_PATHS.csv`는 원래 경로, ZIP 내 보존 경로, SHA-256을 보여 준다. 원래 디렉터리 배치가 필요한 경우 ZIP을 푼 뒤 `python restore_duplicate_paths.py`를 실행하면 같은 바이트의 사본이 복원된다.

같은 거래의 앵커·태그 결합 NPZ가 남아 있는 태그 단독 NPZ만 제외했다. `OMITTED_TAG_ONLY_NPZ.csv`에 해당 거래의 결합 결과, 원본 태그 RFB8, 분석 스크립트 경로를 기록했다. 태그 단독 NPZ는 원래 경로 복원 후 `python <analyze.py> <tag.rfb8> --out <출력 폴더>`로 생성할 수 있다. 태그 단독 분석의 CSV·검증 로그는 보존했다.

펌웨어, 실기 원시 캡처, 합성 재현 입력, 플래시·시험·실패 기록, Pi 보관 자료는 이전 패키지와 동일한 바이트로 보존했다. CSV의 저장 경로는 이 압축본을 기준으로 갱신했다. 아래 본문에서 이전의 `DUPLICATE_BUILD_PRODUCTS.csv`에 해당하는 매핑은 이 압축본의 `DUPLICATE_PATHS.csv`에 통합돼 있다.

2026-11-11 벤치 시험 기록. 성공, 실패, 중단, 전원 재연결 뒤의 재시험을 함께 보존했다. 하드웨어 배치는 COM4 앵커와 Raspberry Pi USB 태그 A·B였다. 이 인계에서 새 시험이나 플래시는 하지 않았다.

## 파일 찾기

| 파일 또는 폴더 | 내용 |
|---|---|
| `CAPTURE_RUNS.csv` | 세션별 설정, 양쪽 장치 식별자와 부팅 ID, 거래 수, 판정, 원시 캡처와 검산 경로 |
| `FIRMWARE_IMAGES.csv` | 앵커·태그 앱, 진단 스케치, 백업 앱과 부트로더·파티션·ELF·MAP의 경로 및 SHA-256 |
| `BUILD_PRODUCTS.csv` · `FLASH_IMAGES.csv` | 빌드·플래시 영수증의 해시와 ZIP 안에서 같은 바이트를 찾을 경로 |
| `PACKAGE_MANIFEST.json` · `SHA256SUMS.txt` | 원본 위치와 ZIP 내 경로, 파일 크기, 분류, 파일별 SHA-256 |
| `DUPLICATE_BUILD_PRODUCTS.csv` | 같은 SHA-256을 가진 중복 빌드 산출물의 원래 경로와 ZIP 내 보존 경로 |
| `local/` | 로컬 작업본의 디렉터리 구조를 보존한 소스, vendor, 빌드, 시험·배포·디버깅 증거 |
| `pi/` | Pi에 남은 태그의 배포 전·후 이미지와 R5 USB·업로드 기록, 원격 SHA-256 목록 |
| `inputs/` | 최초 전달 ZIP, R2·R4 입력 ZIP, 태그 핀맵 |
| `protected/` | 교체 전 앵커 4 MiB 전체 플래시의 DPAPI 사본과 복호화 대조 기록 |

실기 기록의 `.rfb8`는 원시 16비트 CIR I/Q와 RF 프레임, DW1000 시각 정보를 포함한다. `.uart`는 파일이 존재하는 실행의 USB 바이트 스트림이다. `.jsonl`은 디코딩된 USB 메시지다. `measurement_scope/`는 START부터 STOP까지의 분석 입력이며 원본 캡처를 별도로 보존했다. CSV와 NPZ는 파생 분석 결과다. `SYNTHETIC_ONLY`, `replay`, `adversarial` 아래의 입력과 결과는 호스트 시험에 해당한다.

## 단계별 작업과 결과

### 준비 · 원본 백업, 감사, 빌드

- Mode 8 원본부터 R2–R5까지 입력 ZIP, 감사 보고서, 반례 데이터, 호스트 재현 도구와 타깃 빌드 영수증을 보존했다. P2 프레임 충돌, 잘못된 세션·중복 거래, CIR 슬롯과 수신 식별자 불일치, FCS 송신 버퍼 경계, 낡은 BIN 재사용, 파티션 입력 누락, Windows CP949 시험 오류의 기록이 포함된다. [초기 감사](local/UWB_MODE8_AUDIT_20261111/AUDIT_KO.md) · [R2 감사](local/UWB_MODE8_R2_AUDIT_20261111/AUDIT_KO.md) · [R4 감사](local/UWB_MODE8_R4_AUDIT_20261111/AUDIT_KO.md) · [R5 재감사](local/UWB_MODE8_R5_20261111/UWB_DUAL_DW1000_MODE8_R5_20261111/docs/R5_REAUDIT_KO.md)
- ESP32 Core 3.3.7과 `esp32:esp32:esp32doit-devkit-v1`에서 빌드한 역할별 이미지 및 사전 검사·플래시 검증을 포함했다. R5 소스의 장치 문자열이 일부 빌드에서 `D8_R4_SOURCE_20261111`로 유지됐으므로, 동일 문자열의 정확한 이미지 식별에는 플래시 SHA-256을 사용한다. [R5 타깃 검증](local/UWB_MODE8_R5_TARGET_20261111/target_verification.log) · [초기 배포](local/UWB_MODE8_R5_DEPLOY_20261111/deploy_receipt.json)
- 교체 전 앵커 앱·키 헤더는 기존 보호본을 그대로 담았다. 전체 플래시 사본은 이 작업에서 별도로 DPAPI `CurrentUser`로 보호하고 원본 4 MiB의 SHA-256과 복호화 결과가 일치하는지 확인했다. 복호화에는 이 PC의 해당 Windows 사용자 환경이 필요하다. [기존 앵커 보존 기록](local/UWB_ANCHOR_FIRMWARE_BASELINE_20261111/README.md) · [전체 플래시 보호 확인](protected/protected_flash_receipt.json)

### Stage 1 · 장치 식별, 핀맵, 초기화

- 처음 앵커는 USB SELFTEST를 통과했고 교체 태그는 실패했다. 직접 SPI 진단에서 A·B에 같은 비정상 ID `0xBC950360`이 반복된 기록과 A/B DEV_ID·CS 진단 스케치 및 로그가 있다. 태그 핀맵은 공유 SCK18/MISO19/MOSI23, A CS21/IRQ22/RST17, B CS25/IRQ26/RST27이다. CS 수정본은 COM4에서 배포하고 Pi 연결 후 A·B 모두 `0xDECA0130`, SELFTEST 통과를 확인했다. [수정 전 DEV_ID](local/UWB_MODE8_R5_TAG_CS_FIX_DEPLOY_20261111/before_flash_raw_dev_ids.json) · [Pi 재연결 후 SELFTEST](local/UWB_MODE8_R5_TAG_CS_FIX_DEPLOY_20261111/after_move_pi_usb.json)
- 앵커 R6에서 초기화 첫 실패를 USB로 출력했다. `TX_FCTRL` 기대값 `0x252000`, 실측 `0x25a00c`에서 TR 비트가 달랐다. 해당 비트만 읽기 검증에서 제외하고 전송 속도 오설정은 계속 거부하는 회귀시험을 적용했다. [초기화 실패 상세](local/UWB_MODE8_R6_ANCHOR_INIT_DIAG_20261111/evidence/COM4/usb_smoke.json) · [앵커 코드 감사](local/UWB_MODE8_R5_ANCHOR_PLL_STABILITY_20261111/evidence/anchor_firmware_code_audit_20261111/REPORT_KO.md)

### Stage 2 · 양쪽 POLL·P1·P2 수신과 CIR

- 초기 첫 POLL에서 앵커 `CLKPLL_LL`이 기록됐다. PLL 준비 검사, RF 설정 읽기 확인, 고장 시 저속 SPI 및 진단 경로를 정비했다. TR 수정 뒤 첫 POLL은 예정·실제 시각이 일치했고 CLKPLL_LL은 관측되지 않았다. 같은 시험에서 태그의 `SESSION_DEADLINE`은 남았다. 초기 PLL 비트를 처음 세운 하드웨어 조건은 확정되지 않았다. [최초 쌍 시험](local/UWB_MODE8_R5_PAIR_RF_20261111/stage2_first/capture_receipt.json) · [TR 수정 뒤 첫 POLL](local/UWB_MODE8_R6_ANCHOR_TR_FIX_20261111/evidence/stage2_first_poll/first_poll_verdict.json)
- 태그 RX 진단·복구·SPI 접근 구간·읽기 확인을 거치며 전원 재연결, 안테나 지연 레지스터 `65535` 읽힘, 부분 수신, CIR 오류를 기록했다. CIR 10 MHz 시험은 0/20, 20 MHz 최종 및 재시험은 각각 20/20 완결이었다. [20 MHz 최종](local/UWB_MODE8_TAG_STAGE2_RX_READBACK_20261111/evidence/stage2_20_cir20mhz_final/capture_receipt.json) · [20 MHz 재시험](local/UWB_MODE8_TAG_STAGE2_RX_READBACK_20261111/evidence/stage2_20_cir20mhz_retry_20261111_184229_280/capture_receipt.json)

### Stage 3 · A·B RESPONSE 송수신

- 전원 재연결 뒤 `txn=16`의 앵커 A 응답 누락과 `txn=19`의 태그 A 응답 예약 여유 부족을 기록했다. RESPONSE 타이밍과 앵커 수신 확인을 수정했다. 중간 수정본은 앵커 19/20, 태그 20/20이었고 읽기 확인을 반영한 재시험에서 A·B RESPONSE가 각각 태그 송신 20/20, 앵커 수신 20/20이었다. [실패 거래](local/UWB_MODE8_TAG_STAGE2_RX_READBACK_20261111/evidence/stage3_20_after_anchor_power_20261111_184742_135/verdict.json) · [최종 송수신 검증](local/UWB_MODE8_STAGE3_RESPONSE_FIX_20261111/evidence/stage3_20_rx_readback_20261111_190903_269/verdict.json)

### Stage 4 · A·B DS-TWR와 원시 시각 검산

- 기본 지연에서 음수 거리 때문에 A 15/20, B 0/20이 유효했다. 약 0.8 m 배치의 기준 거래에서 태그 A 16279틱, B 15886틱을 임시로 정하고 새 20거래에서 A·B 각각 20/20을 검산했다. 앵커는 16436틱이었다. 이 값은 당시 배치의 시험 설정이다. [기본값 검산](local/UWB_MODE8_STAGE4_DS_TWR_20261111/evidence/stage4_20_default_20261111_191657_832/stage4_distance_verdict.json) · [임시 설정](local/UWB_MODE8_STAGE4_DS_TWR_20261111/evidence/candidate_tag_delay.json) · [새 20거래](local/UWB_MODE8_STAGE4_DS_TWR_20261111/evidence/stage4_20_trial_delay_20261111_191836_249/stage4_distance_verdict.json)

### Stage 5 · P1·P2 공통 지연축과 전력 그리드

- 이전 P2 계산에서 초반 교환의 왕복 시각을 재사용해, 두 구간의 유효 상대 클록비가 달라졌을 때 P2 음수 거리로 이어지는 것을 원시 시각으로 확인했다. 상대 클록비 차이를 만든 하드웨어 메커니즘은 미확정이다. P2 전용 POLL2와 새 A·B 응답, 해당 교환의 실제 P2 시각을 사용하도록 수정했다. 650 ms, 20거래에서 P1·P2 모두 20/20 공통 지연축과 전력 그리드를 검산했다. [원시 시각 진단](local/UWB_MODE8_STAGE5_P2_RANGING_20261111/evidence/p2_baseline_diagnosis.json) · [최종 20거래](local/UWB_MODE8_STAGE5_P2_POLLFIX_20261111/evidence/stage5_execution.json)

### Stage 6 · 연속 수집, 주기, USB 전송

- 300 ms 초기 시험은 P1 75/100, P2 74/100이었다. 1 ms 대기가 약 0.8 ms인 수신 상태 확인 구간을 건너뛰는 경우를 호스트에서 재현했다. 마감 근처 25 µs 확인, 하드웨어 RX 시각 기반 마지막 확인, 필요시 같은 거래의 새 RF 교환을 추가했다. [초기 300 ms 검산](local/UWB_MODE8_STAGE6_CONTINUOUS_IOFIX_20261111/evidence/stage6_300ms_100_20261111_202141_012/stage6_verdict.json)
- 200 ms·2 Mbps 초기 시험은 RFB8 수신 앵커 40/100, 태그 0/100이었다. RF를 끈 고정 데이터 진단에서도 직렬 경로에서 바이트 누락이 재현됐다. 조각 CRC·패리티 복구, 태그 무손실 I/Q 패킹, 버퍼링을 적용했다. 최종 100거래 시험 두 조건 모두 P1·P2 100/100, RFB8 누락·CRC·주기 초과 0이었다. [초기 2 Mbps](local/UWB_MODE8_STAGE6_CONTINUOUS_IOFIX_20261111/evidence/stage6_200ms_2mbps_100_20261111_202458_020/capture_receipt.json) · [최종 100거래 두 조건](local/UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_PACKED_20261111/evidence/stage6_fast_execution.json)
- **200 ms·2 Mbps, 1,000거래 세션 3895992639는 미통과**다. 생성은 P1 999/1,000, P2 998/1,000이고 양쪽 기록 검산은 각각 998/1,000이다. 태그 B가 `txn=29`의 P1과 `txn=634`의 P2를 놓쳤다. 앵커 `txn=944`는 누락 데이터 조각 9개에 유효 패리티 6개여서 복원에 실패했다. RFB8 수신 CRC 및 펌웨어 주기 초과는 양쪽 0건이었다. USB 조각 CRC는 앵커 2,184건, 태그 5,348건이었다. [1,000거래 검산과 해시](local/UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_PACKED_20261111/evidence/stage6_200ms_2mbps_1000_20261111_220602_883/stage6_1000_verdict.json)

## 복원과 해석 범위

- 가장 최근 실기 조합은 앵커 `D8_R6_ANCHOR_STAGE6_RX_FEC_RFRETRY_20261111`, 태그 `D8_R5_TAG_STAGE6_RX_FEC_PACKED_20261111`이다. 실행 기록의 빌드·플래시 SHA-256으로 대조할 수 있다. [조합 및 실기 영수증](local/UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_PACKED_20261111/evidence/stage6_fast_execution.json)
- 현장에서 생성·보존된 로컬 및 Pi 파일을 수집했다. 기존 영수증 안의 절대 경로는 작성 당시 위치를 보존한다. ZIP에서 같은 파일을 찾을 때는 `PACKAGE_MANIFEST.json`의 `source_path`와 `path`를 대조한다. 초기 USB 시험은 당시 생성된 스트림·JSON·로그 범위만 남아 있다.
- 재생성 가능한 컴파일 오브젝트와 캐시를 제외했다. 동일 바이트의 중복 빌드 산출물은 매핑을 남겼다. 실패 원시 데이터 및 분석 중간본도 보존했다. 실기 거리와 지연축은 정적 벤치 시험의 추정치다.

