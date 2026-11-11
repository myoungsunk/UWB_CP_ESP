# Mode 8 R4 사용 적합성 감사

**2026-11-11 · REDIRECT · 현재 R4의 배포 기준 채택 보류**

질문 ID는 `UWB-MODE8-SOURCE-HOST-QA-20261111`이다. 감사 입력은 `UWB_DUAL_DW1000_MODE8_R4_SOURCE_HOST_REAUDIT_20261111.zip`, SHA-256 `2a4342497ab9a693338ea74d7d7abc7ae239c1d0ffb90e19187eb8de0a488a93`다. ZIP 164개 항목 CRC와 배포 목록 162개 파일 SHA-256을 검증했다. 두 스케치의 공통 헤더와 `shared`가 일치했고, R3 대비 vendor 50개 파일도 일치했다. 이번 작업은 제출된 R4의 소스·호스트 감사이며, 제출 소스와 원본 ZIP을 수정하지 않았다.

## 제보된 네 결함

| 항목 | 직접 확인한 결과 |
| --- | --- |
| TX 경계 초과 | **해소.** 양쪽 vendor의 `setTransmitData`는 프레임 체크 사용 시 96에 2를 더한 길이로 TX 버퍼를 쓴다. R4 어댑터는 98바이트를 0으로 초기화하고, 96바이트 프레임을 인코딩해 전달한다. [어댑터](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/shared/esp32_port.hpp#L150) · [앵커 vendor](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/vendor/anchor/DW1000Ng/src/DW1000Ng.cpp#L2257) · [태그 vendor](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/vendor/tag/DW1000Ng/src/DW1000Ng.cpp#L2237) |
| CIR 슬롯 식별 | **제보 사례 해소.** 슬롯 번호·수신 모듈·Probe 번호를 각각 바꾸면 수집과 해당 슬롯의 분석 유효 판정이 거부됐다. [시험](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tests/test_r4_regression.py#L49) |
| RX 식별 | **제보 사례 해소.** 태그 POLL/Probe와 앵커 RESPONSE 수신기 라벨을 바꾼 시험이 통과했다. 정상 부분 기록의 거리 누락과 앵커 TX 검사 불일치는 아래와 같이 별도로 재현됐다. [시험](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tests/test_r4_regression.py#L59) |
| 구형 BIN 재사용 | **부분 해소.** 유효한 테스트용 빌드 기록에서 일반 `.hpp`를 바꾸면 업로드 사전 검사가 거부했다. 현재 빌드가 실제로 쓰는 `partitions.csv` 변경은 누락된다. [입력 열거](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tools/source_contract.py#L8) · [실행 결과](replay/audit_cases.json) |

## 채택 전 해결할 결함

### R4-A1 · 높음 · 한쪽 POLL 누락 시 정상 채널의 거리도 소실

R4 분석기의 거래 공통 `poll_ok`는 두 로컬 POLL에 모듈·타입·프레임 일치를 모두 요구한다. 미수신 슬롯은 펌웨어가 기본값으로 기록하므로 두 채널 모두 물리축 계산에서 빠진다. R4 C++ 모델에서 A의 POLL만 누락시키면 B의 P1이 약 **5.1123 m**, 반대 경우 A의 P1이 약 **4.9984 m**로 유효했다. 두 경우 모두 같은 입력을 R3 분석기에 넣으면 해당 거리가 보존되고 R4는 `null`을 출력했다. 실제로 수신했다고 기록된 POLL만 공통 무결성 검사 대상으로 삼고, 수신 여부와 거리 유효성은 모듈별로 확인해야 한다. [분석 조건](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tools/analyze.py#L146) · [C++ 재현](replay/partial_poll.cpp) · [결과](replay/audit_cases.json)

### R4-A2 · 높음 · 수집기가 거부한 앵커 TX 기록을 분석기는 수용

앵커의 POLL 예약 시각, P1 TX 유형, P2 TX 모듈 번호를 각각 변경했다. 수집 감사가 세 사례 모두에서 해당 TX를 거부했으나, 분석은 매번 A/B P2 약 **5.0011 m / 5.1104 m**와 **993점** 공통 물리 지연축을 출력했다. 분석의 `anchor_matches()`에도 수집기와 같은 TX 슬롯 유형·모듈·예약/실제 시각 일치 조건을 적용해야 한다. [분석 조건](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tools/analyze.py#L65) · [수집 조건](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tools/capture_audit.py#L185) · [결과](replay/audit_cases.json)

### R4-A3 · 높음 · 파티션 입력 변경 후에도 기존 이미지가 업로드 검사 통과

설치된 ESP32 Core 3.3.7의 빌드 절차는 스케치 폴더의 `partitions.csv`를 우선 복사해 파티션 바이너리를 생성한다. R4 빌드 입력 해시는 C/C++ 계열 확장자에 한정된다. 동결된 R3 소스·바이너리를 임시 폴더에 복사하고 R4 형식의 테스트용 빌드 기록을 만들어 확인했다. 앵커 스케치의 파티션 CSV를 바꾸자 Core의 실제 생성기가 새 파티션 바이너리를 만들었지만, 입력 해시는 그대로였다. 그 상태에서 R4 사전 검사가 통과했고 업로드 코드가 이전 이미지로 업로더를 호출하려 했다. 해당 호출은 모의 처리했다. 파티션·사용자 부트로더 등 플래시 산출물을 바꾸는 입력을 해시 검사에 포함하거나 해당 오버라이드를 명시적으로 거부해야 한다. [입력 열거](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tools/source_contract.py#L8) · [업로드 검사](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/tools/upload.py#L27) · [재현 코드](replay/audit_cases.py) · [결과](replay/audit_cases.json)

## 재실행 및 범위

- Windows Python 3.13에서 UTF-8 모드로 62개 단위시험이 통과했다. 기본 CP949 모드에서는 vendor 소스를 기본 인코딩으로 읽는 FCS 시험 한 건이 `UnicodeDecodeError`로 종료됐다. 테스트에서 UTF-8을 명시하면 이 환경 의존성을 제거할 수 있다. [UTF-8 실행](replay/python-tests-utf8.log) · [기본 인코딩 실행](replay/python-tests.log)
- WSL Ubuntu 22.04의 GCC ASan·UBSan 실행에서 합성 assertion 11,413개가 통과했고, 두 역할의 호스트 구문 검사도 통과했다. [C++ 로그](replay/gcc-core.log) · [구문 검사](replay/gcc-syntax.log)
- 제출된 R4는 `target_build=false`이고 `dist`에 R4 BIN/ELF가 없다. 오프라인 사전 검사는 종료 코드 2를 반환했다. 이번 감사에서는 ESP32 타깃 컴파일, 실제 업로드, 실물 USB·RF 시험을 수행하지 않았다. [제출 빌드 기록](UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111/dist/target_build_receipt.json) · [사전 검사](replay/as_delivered_preflight.json)

적용 지침은 로컬 RESEARCH-GOVERNANCE-v1이다. SHA-256은 COMMON `a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb`, AUDITOR `e1bdc2f231c71cdc27201c688e041b078576a6adf6c0290acc16ce4d8218291a`다.
