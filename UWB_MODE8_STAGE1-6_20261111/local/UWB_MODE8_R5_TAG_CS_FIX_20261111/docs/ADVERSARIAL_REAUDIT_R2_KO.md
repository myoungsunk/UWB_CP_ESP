# UWB Mode 8 R2 — 전체 재감사 및 적대적 검증 보고서

기준 날짜: 2026-11-11 (KST)  
수정 릴리스: **`D8_R2_REAUDITED_20261111`**  
프로토콜: Air `FCB8` v1, RFB8 v1, **Config ID 계약 개정 R2** (동일 v1 포맷이나 이전 R1 Mode 8과 Config CRC 불일치)  
**최종 판정: `HOST_REAUDIT_PASS / TARGET_BUILD_BLOCKED / HARDWARE_NOT_TESTED`**

> 이번 재감사는 제공된 감사 결과를 기준으로 구현자 역할에서 적대적 리뷰 및 재실행을 수행한 것입니다. 제3자 독립 감사 인증 또는 RF 현장 승인으로 표시하지 않습니다.

## 1. 고정 입력 및 출처

- 전 릴리스 ZIP: `UWB_DUAL_DW1000_MODE8_SOURCE_HOST_QA_20261111.zip`, SHA-256 `4a5f27ff6cc4f3bca5c1b7c27f5ff53e1acb80cbed36a5646ed027fd9864f11c`.
- 전달 감사 ZIP: `UWB_MODE8_AUDIT_20261111.zip`, SHA-256 `1487772db1f16e93955994d692e35d93c67443d8099f8dc216abcbf628cb9926`.
- 기존 R1L80 원본: `evidence/BASELINE_R1L80_UNMODIFIED.zip`. 이 기준 원본과 vendor 라이브러리는 수정하지 않았습니다.
- 외부 감사 본문 및 원본 반례: `evidence/adversarial/ORIGINAL_AUDIT_KO.md`, `ORIGINAL_COUNTEREXAMPLES.json`.
- 수정 전후 재현 결과: `evidence/adversarial/AFTER_FIX_REPLAY_REPORT.json`.

## 2. 보고된 두 결함의 재현·수정·검증

| ID / 심각도 | 수정 전 반례 | 최종 수정 | 확인한 결과 |
|---|---|---|---|
| **M8-01 / 높음** | P2-B의 실제 RF 프레임과 공유 P2 메타데이터가 불일치하고 `metadata_conflict=true`여도 분석기가 `axis_estimate_available=true`, 거리 약 5.1104 m를 출력 | `tools/analyze.py`에서 **각 slot의 수신 RF CRC·동일성·부팅/세션/거래/설정/프로브 타입**, P1/P2 앵커 블록, 실제 TX, 로컬 여섯 시각을 교차 검사. 충돌 시 물리 지연축·공통 전력 그리드 사용 금지 | 동일 반례에서 **P2-B 물리축 무효**, 거리 `null`, P2 공통 그리드 비어 있음. FP 상대축과 원시 데이터 유지 |
| **M8-02 / 높음** | 요청 세션 77에서 다른 세션 76, boot 999, txn `[0,0]`, 시퀀스 갭을 주입해도 수집기 `pass=true` 반환 | `tools/run_pair.py` + `tools/capture_audit.py`에서 **실제 요청 session·HELLO boot/peer boot·config·role·stage·period·거래 0..N-1 유일성/순서·양쪽 실제 RF payload 동일성·개별 송수신 시각·CIR/DS 유효성·스트림 손실·리더 오류**를 최종 패스에 연동 | 같은 반례에서 종료 코드 **2**, `pass=false`; 중복·세션 불일치·시퀀스 누락을 각각 기록 |

두 반례의 **보정 전 값과 보정 후 값은 JSON 증거 파일에 모두 기록**했습니다. 기존 `reproduce_findings.py`는 이전 릴리스에서 결함이 존재했음을 입증하는 원본으로 보존하고, 새 릴리스의 검증은 `tests/test_adversarial.py` 및 `AFTER_FIX_REPLAY_REPORT.json`을 사용합니다.

## 3. 전체 코드 적대적 검토에서 추가로 수정한 결함

| ID / 심각도 | 발견한 위험 | 조치 및 검사 |
|---|---|---|
| **M8-03 / 높음** | `math_valid`와 공유 P1 프레임만 믿어, 로컬 RX/TX 타임스탬프 또는 P1/P2의 실제 RF 수신 프레임이 서로 다른데도 물리축 확정 가능 | 독립 DS 계산 전 `six_ts` 6개를 **실제 프레임의 TX/RX 정보, 태그 RX 및 TX readback**과 대조. 잘못된 근거의 물리축 거부 |
| **M8-04 / 높음** | firmware 내부에서 하드웨어·시간 초과 오류가 있어도 독립 DS 계산 결과가 유효로 남을 위험 | `shared/core.hpp`에서 `ErrHardware`, `ErrLate`, `ErrMetadata`를 최종 거리 확정 금지 조건으로 적용. `complete`도 무오류 조건에 종속 |
| **M8-05 / 높음** | RFB8 외부 CRC가 정상인 상태에서 raw CIR을 조작하고 슬롯 CRC만 이전 값으로 두어도 수집 완료로 판정될 가능성 | `tools/run_pair.py::cir_payload_integrity()`가 **태그 CIR 4개 각각의 slot/길이/CRC/유효 데이터** 확인. 누락 CIR은 zero padding과 미유효 표기가 일치해야 함 |
| **M8-06 / 중간** | 앵커 파일을 `(session,txn,boot)`만으로 조인할 때 같은 거래를 다른 boot로 중복할 수 있고, 앵커 로그 내부 시각·프레임 부족으로 P2 거리 허용 가능 | 분석기 중복 거래 거부, 앵커 실제 POLL/P1/P2 TX 시각·응답 RX 시각·3개 RF 프레임과 태그 기록을 교차 검사. 반례로 검증 |
| **M8-07 / 중간** | 850 kbps에서 `smartPower=true` 설정은 Smart TX Power 적용 전제와 불일치 | 앵커·태그 공통 구성에서 **`smartPower=false`**. Config CRC에 계약 판번호 2를 넣어 이전 Mode 8의 혼합을 방지. 단, **실제 TX_POWER 레지스터와 방사 출력 실측은 미검증** |
| **M8-08 / 중간** | 수집 종료 전 `pass` 확정, 한쪽 직렬 수집 지연, reader fault/손실을 놓칠 위험 | 양쪽 지정 건수 수집·소거 구간 이후 **STOP 및 리더 종료 시점**에 완전성 판정. `fault_events`, `sequence_gap_events`, `crc/schema/trailing/skipped` 및 reader error를 모두 확인. 감사 예외 발생 시 fail-closed |

검토한 구성요소: 공통 C++ 프로토콜·엔진·RF 포팅·기록기·RF guard, 앵커/태그 스케치·DW1000Ng 벤더 헤더, RFB8 디코더, 두 보드 수집 실행기, CIR 분석기, 빌드·업로드·해시 도구, 호스트 테스트 및 배포 메타데이터. 100% 결함 부재를 보증하는 형식 검증은 아닙니다.

## 4. 실행한 검증 (실측 RF와 혼동 금지)

| 항목 | 확인 상태 |
|---|---|
| 원본 감사의 두 반례 재실행 및 수정 결과 확인 | **PASS**, 기존의 잘못된 승인 결과가 더는 나오지 않음 |
| GCC/C++11 프로토콜 및 상태기계, AddressSanitizer·UndefinedBehaviorSanitizer | **11,342 assertions PASS**, 해당 테스트 실행에서 진단 없음 |
| Clang 별도 C++11 빌드·상태기계 실행 | **11,342 assertions PASS** |
| GCC/Clang 앵커·태그 상위 소스 | **구문 컴파일 통과**, 실제 vendor 헤더 및 SDK *스텁* 사용 |
| Python | **42개 테스트 PASS**, 정상 수집/악성 수집 시뮬레이션 포함 |
| RFB8 합성 사례 앵커·태그 디코딩 및 물리 시간축 분석 | **PASS**, 두 입력 decoder CRC/schema/gap/trailing 0 |
| 실제 ESP32 Core 3.3.7 타깃 컴파일/링크 | **BLOCKED**: 실행 환경에 `arduino-cli` 없음; BIN/ELF/MAP 없음 |
| 두 DW1000 동시 RX, UART 부하, LOS 실측 거리 및 51 ms RF 시간 | **NOT RUN** |

증거 위치: `evidence/host/host_qa_receipt.json`, `evidence/host/core_test.log`, `evidence/host/clang_core_test.log`, `evidence/host/python_tools_tests.log`, `evidence/host/SYNTHETIC_ONLY_analysis/`, `evidence/target_build_attempt_R2.log`.

## 5. 최종 판정과 인수 제한

**소스·PC 호스트 회귀시험 수준에서는 ACCEPT**, 단 **타깃 빌드와 하드웨어 인수는 HOLD**입니다. 이전 감사의 REDIRECT 사유 두 개는 합성 반례에 대해 수정 확인했습니다. 이를 **실물 장치에서의 정상 동작 입증**이라고 표현해서는 안 됩니다.

`D8_R2_REAUDITED_20261111`은 **USB 직렬 측정용 분리 펌웨어**입니다. 기존 등록 키, Wi-Fi/TCP/OTA, Mode 3–7, RFB5 기능과 통합돼 있지 않습니다. 배포 ZIP의 `dist/target_build_receipt.json`에는 `target_build=false`가 기록됐고, `.bin/.elf`는 넣지 않았습니다.

승인 보류 조건: ① 실제 SDK 전체 링크 및 빌드 산출물 SHA ② 두 DW1000 전원·SPI 및 RXENAB 무결성 ③ 동시에 수신한 CIR의 retention 시험 ④ RF 송신 7/14/19/51 ms의 실제 deadline ⑤ 각 모듈별 TX/RX 안테나 지연 보정·독립 DS 거리 정확도 ⑥ 850 kbps에서 출력 설정 및 합법적인 RF 출력 확인 ⑦ 장기 RFB8 무손실 검증. 모두 별도 수행해야 합니다.

현재 CRC·세션 ID는 **우발적인 혼합과 전송 오류**를 검출하기 위한 것이며, 악의적으로 RF/USB 전체 데이터를 재계산해 위조하는 공격에 대한 암호학적 인증은 제공하지 않습니다.
