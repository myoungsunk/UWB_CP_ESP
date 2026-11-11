# Mode 8 R4 — 적대적 전체 코드 재감사 및 수정 보고

## 감사 판정

**R4_HOST_ADVERSARIAL_PASS / ESP32_TARGET_BUILD_NOT_RUN_FOR_R4 / REAL_RF_NOT_RUN.**

2026-11-11. R3 ZIP 원본을 동결 기준으로, C++ 엔진·실제 벤더 FCS API·ESP32 어댑터·수집기·CIR 분석기·업로드 보호 경로를 검토하고 수정했습니다. 수행자는 제3자 독립 감사 기관이 아니며 이번 PASS는 실행한 호스트 시험 범위에만 적용합니다.

## A. 발견한 결함과 구체적 재현

| ID | 원인 | 수정·검증 |
|---|---|---|
| R4-01 / 높음 | `DW1000Ng::setTransmitData(data,96)`는 프레임 체크 사용 시 내부에서 n+=2하여 SPI로 98바이트를 읽는다. R3 상위 코드는 `uint8_t data[96]`을 전달해 2바이트 경계 초과 읽기를 일으킬 수 있다 | `shared/esp32_port.hpp`의 송신 배열을 `AIR_BYTES+2`로 확장하고 0으로 초기화, 컴파일시간 크기 검사. 태그와 앵커 모두 동일 수정. 벤더 원본 보존 |
| R4-02 / 높음 | R3 수집기·분석기는 CIR의 `slot/radio/probe` 표기를 충분히 검사하지 않았다. B CIR을 A로 표시하면 정상 승인 | 슬롯의 `(0,A1),(1,B1),(2,A2),(3,B2)` 고정 계약을 수집기/최종 감사/분석기에서 교차 검증 |
| R4-03 / 높음 | R3 수신 슬롯 `radio/type`을 바꿔도 프레임 CRC와 공유 프레임이 동일하면 물리 지연축을 생성 | tag POLL/P1/P2 RX와 anchor RESPONSE RX, TX 슬롯의 `radio/type` 일치 검사 추가. 부정합 시 범위/물리축 거부·raw 보존 |
| R4-04 / 높음 | R3 업로드 경로가 이전 BIN 해시는 검증하지만 **바뀐 소스와 BIN의 대응관계**를 검증하지 않았다 | 타깃 빌드 시 역할별 실제 컴파일 입력 해시 등록. 업로드 시 전체 역할의 FQBN·코어·원시 BIN/ELF/부트로더/파티션/merged 배치 및 변경되지 않은 소스 검사 후 진행 |

원본 R3 분석·수집 파일과 R4 수정 파일을 동일 합성 거래에 적용한 독립 파일 실행 기록은 `evidence/r4/adversarial_before_after.json`입니다.

| 주입 조건 | R3 capture/axis | R4 capture/axis |
|---|---|---|
| 정상 거래 | PASS/PASS | PASS/PASS |
| CIR B의 `radio`를 A로 변경 | **PASS/PASS (오류)** | REJECT/REJECT |
| CIR B2 `probe`를 1로 변경 | **PASS/PASS (오류)** | REJECT/REJECT |
| B2 수신기 `radio`를 A로 변경 | **PASS/PASS (오류)** | REJECT/REJECT |
| B2 수신기 `type`을 P1로 변경 | **PASS/PASS (오류)** | REJECT/REJECT |

## B. 새 적대적 시험

`tests/test_r4_regression.py`에는 CIR 슬롯 교환, 수신기 타입·모듈 식별 변조, Poll RX 식별 변조, 앵커 TX/RESPONSE 라벨 변조, 송신 FCS 버퍼 API 정적 계약, 변경된 소스에서의 구형 이미지 업로드 차단, 소스 해시 변화 시험이 포함됩니다.

- 원래 R3 테스트 54개 + R4 신규 8개 = **Python 62개** 정상 통과.
- GCC·Clang 각각 **11,413개 C++ 합성 assertion** 통과; 호스트 ASan/UBSan 실행에서 오류 보고 없음.
- 두 역할의 **상위 스케치 구문 검사 통과** (실제 벤더 헤더 + 호스트 Arduino/FreeRTOS 스텁; ESP32 타깃 링크와 구분).
- 출처: `evidence/host/host_qa_receipt.json`, `evidence/host/core_test.log`, `evidence/host/clang_core_test.log`, `evidence/host/python_tools_tests.log`.

## C. 오프라인 대상 이미지 확인

R3 ZIP에 포함된 앵커·태그 BIN/ELF/bootloader/partitions/merged는 기존 타깃 컴파일 receipt SHA와 바이트 크기가 일치했습니다. merged 내 0x1000(bootloader), 0x8000(partition), 0x10000(application) 배치도 일치했습니다. `evidence/r4/R3_original_images_offline_static_verification.json`.

**이 결과는 R3 이미지를 R4로 바꾸지 않으며 R3 안전성 통과가 아닙니다.** R4용 새 컴파일러/크로스 링크 실행이 필요합니다. 이 환경에는 Arduino CLI·Xtensa 빌드 도구가 없어서 R4 .bin/.elf를 만들 수 없었습니다.

## D. 잔여 위험 / 보류 게이트

1. R4 재빌드 및 해시-소스 결합된 두 역할 산출물 실제 생성.
2. 실물 DevKit 30핀·DW1000 A/B 3.3V·전원·CS·RSTn 독립성; 각 모듈 RXENAB에서 DEV_ID/시계 유지.
3. 동시에 수신한 두 채널 accumulator 보존, 순차 CIR 반복 읽기 정합, P1→P2 여유.
4. RESPONSE_A/B 공중 간섭·재수신 타임라인·DS 거리 정확도·안테나 지연 보정.
5. 수집·분석에서 세션 혼합, CIR CRC, 슬롯 식별, STOP 요청과 실제 RF 실패를 서로 구분.
6. 명목 51ms 슬롯과 실제 RF 완료시간·UART 갱신율을 별도로 실측.

**결론:** 소스·호스트 적대적 검사는 통과. R4 업로드 및 Mode 8 현장 검증 인수는 아직 불가. 이 판단을 `HARDWARE_PASS` 또는 제3자 독립 승인으로 변경하지 않습니다.
