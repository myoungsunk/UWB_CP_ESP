# Mode 8 소스·호스트 검증 감사

**판정 REDIRECT · 감사 완료 · 타깃 컴파일 보류**

질문 `UWB-MODE8-SOURCE-HOST-QA-20261111`에서 확인한 범위는 첨부 ZIP의 앵커·태그 Mode 8 소스, 역할별 DW1000Ng, 호스트 수집·분석 및 빌드 진입점이다. 이 세션은 제공본 구현에 참여하지 않았으며 2026-11-11 KST에 AUDITOR로 검토했다. 별도 리뷰 에이전트는 사용하지 않았다. ESP32 타깃 컴파일, 업로드, 장치 접근과 RF 시험은 수행하지 않았다.

- 감사 대상 ZIP SHA-256 `4a5f27ff6cc4f3bca5c1b7c27f5ff53e1acb80cbed36a5646ed027fd9864f11c`. Git 이력 대신 이 해시 및 ZIP 내부 파일 해시로 소스와 실행 입력을 식별했다.
- 연구 정책 `RESEARCH-GOVERNANCE-v1`. `COMMON_CONTRACT.md` SHA-256 `a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb`. `ROLE_AUDITOR.md` SHA-256 `e1bdc2f231c71cdc27201c688e041b078576a6adf6c0290acc16ce4d8218291a`.

## 컴파일을 보류한 이유

### M8-01 · 높음 · 충돌한 P2 프레임으로 물리 지연축 생성

태그는 각 슬롯의 실제 수신 프레임을 `probe_rx[slot].air`에 보존한다. 두 라디오의 프레임 내용이 다르면 `waitPair`가 `metadata_conflict`를 기록하고 `finalizeRanges`가 P1 거리 확정을 거부한다. 호스트의 P2 경로는 `frames[2]`의 공유 프레임만 사용한다. 슬롯별 수신 프레임과의 일치 및 `metadata_conflict`를 확인하지 않은 채, 그 슬롯의 RX 시각과 공유 프레임의 앵커 시각을 결합한다. [태그 상태기계](UWB_DUAL_DW1000_MODE8_20261111/shared/engine.hpp#L34) · [거리 유효화](UWB_DUAL_DW1000_MODE8_20261111/shared/core.hpp#L131) · [P2 분석](UWB_DUAL_DW1000_MODE8_20261111/tools/analyze.py#L82)

제공된 합성 기록에서 B의 P2 프레임에 실린 RESPONSE RX 시각을 256 DTU 변경하고 RF CRC를 다시 계산했다. `metadata_conflict=true`, B 수신 프레임과 공유 프레임 불일치 조건에서 분석기는 B에 `axis_estimate_available=true`와 약 5.1104 m를 출력했다. 따라서 오프라인 P2 물리 지연축과 그 축을 사용하는 공통 전력 그리드에 거래의 타임스탬프 정합성이 보장되지 않는다. 원시 CIR 보존과 FP 상대축 계산은 유지할 수 있다. [재현 출력](replay/counterexamples.json)

수정 후 P2 축을 허용하는 각 슬롯의 수신 프레임, 앵커 거래 및 실제 송신 시각을 서로 대조해야 한다. 재감사에서는 A/B 불일치, P1/P2 메타데이터 충돌, 정상 단일 수신, 양쪽 정상 수신을 확인한다. 실제 지연축 정확도에는 별도 하드웨어 근거가 필요하다.

### M8-02 · 높음 · 요청 세션과 무관한 데이터로 수집 통과

`Link._read()`는 수신한 종류 2 레코드의 개수와 `complete` 값만 누적한다. 실행기 최종 판정은 이 수치와 CRC·스키마 오류만 사용한다. 현재 실행의 session, 양쪽 boot, txn의 유일성과 대응, 순번 누락을 통과 조건에 연결하지 않았다. [레코드 집계](UWB_DUAL_DW1000_MODE8_20261111/tools/run_pair.py#L41) · [최종 판정](UWB_DUAL_DW1000_MODE8_20261111/tools/run_pair.py#L158)

실제 실행기와 RFB8 디코더에 메모리상의 직렬 입력을 연결했다. 요청 세션은 77이고 주입한 레코드는 양쪽 모두 세션 76, boot 999, txn `[0, 0]`이다. 양쪽에 순번 누락도 한 번씩 넣었다. 실행기가 종료 코드 `0` 및 `pass=true`를 반환했다. 따라서 `capture_receipt.json`의 `pass`로 해당 실행의 수집 완전성을 입증할 수 없다. [재현 출력](replay/counterexamples.json)

수정 후 현재 실행의 식별자가 일치하는 양쪽 레코드를 고유 거래 단위로 대조하고, 수집 누락 및 리더 오류를 최종 판정에 반영해야 한다. 재감사에서는 이전 세션, 재부팅, 중복, 한쪽 누락, 순번 누락과 정상 양쪽 수집을 확인한다.

## 확인한 범위와 남은 조건

- ZIP 130개 엔트리의 압축 CRC가 정상이고, 배포 목록에 있는 128개 파일의 크기와 SHA-256이 일치한다. DW1000Ng 50개 파일은 내장 기준 ZIP과 그 출처 목록의 해시에 일치한다. 역할별 스케치의 공통 헤더 복사본도 `sync_shared.py --check`를 통과했다. [패키지 검사](replay/package-integrity.json)
- GCC 11.4에서 AddressSanitizer·UndefinedBehaviorSanitizer를 켜고 호스트 C++ 합성 검사 11,309회를 재현했다. 앵커·태그는 실제 DW1000Ng 헤더와 호스트 선언 스텁으로 각각 구문 검사를 통과했다. Python 단위시험 17개를 재현했다. 제공본의 Clang 결과는 첨부 로그를 확인했다. [GCC 실행 결과](replay/gcc-core-test.log) · [Python 실행 결과](replay/python-tests.log)
- 내장 기준 ZIP은 로컬 `deliverables/UWB_TX_RX_FIRMWARE_20261110.zip`과 다른 스냅샷이다. 정규화한 공유 파일 78개 중 7개가 다르며, 그중 5개는 기존 태그 소스다. 내장본의 `MODE6_REFERENCE.json`과 로컬본의 `MODE7_REFERENCE.json`도 다르다. DW1000Ng 파일은 일치한다. 감사 판정은 첨부 ZIP의 독립 USB Mode 8 앱에 한정하며, 교체된 태그의 현재 설치 버전과의 계보 및 호환성은 미확인이다.
- 기본 RF 설정은 850 kbps에서 `smartPower=true`다. 역할별 라이브러리는 이 플래그에 따라 송신 전력 레지스터 값을 다르게 쓴다. 제조사 API 가이드 §5.14가 설명하는 Smart TX Power의 적용 데이터율은 6.8 Mbps이므로, 실물 시험 전에 의도한 수동 출력과 레지스터 설정을 확인해야 한다. 현재 보드의 출력에 대한 판정은 미실행이다. [RF 설정](UWB_DUAL_DW1000_MODE8_20261111/shared/esp32_port.hpp#L90) · [Decawave DW1000 API Guide §5.14](https://forum.qorvo.com/uploads/short-url/9wTyf4vAaBkReECig7wIppI9Ek9.pdf)
- 로컬 Arduino CLI 1.5.1과 ESP32 Core 3.3.7 설치를 확인했다. 감사 조건이 충족되면 기존 설치를 사용해 첨부본의 FQBN으로 두 역할을 빌드할 수 있다. 현재 `.bin`·`.elf`와 타깃 메모리 사용량은 미생성이다.

## 실행 기록

- Windows Python에서 `py -3 -B tools/verify_package.py`, `py -3 -B tools/sync_shared.py --check`, `py -3 -B -m unittest discover -s tests -p test_tools.py -v`를 실행했다. 각 최종 실행의 종료 코드는 `0`이다.
- Ubuntu 22.04에서 `g++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -Ishared -fsanitize=address,undefined tests/test_core.cpp -o ../replay/test_core`로 빌드하고 `../replay/test_core ../replay`를 실행했다. 구문 검사는 각 역할에 대해 `g++ -std=c++11 -DESP32 -DARDUINO -Wall -Wextra -Wpedantic -Werror -Itests/stubs -Ivendor/<role>/DW1000Ng/src -fsyntax-only firmware/<ROLE>_DUAL_DS_CIR/app.cpp`로 실행했다. 각 최종 실행의 종료 코드는 `0`이다. 첫 일괄 구문 검사 호출에서 역할 인자가 비어 두 역할을 명시한 명령으로 다시 실행했다.
- 감사 폴더에서 `py -3 -B reproduce_findings.py`를 실행했다. 종료 코드 `0`은 두 결함의 재현을 뜻한다. [재현 코드](reproduce_findings.py) · [재현 출력](replay/counterexamples.json)
- 원본 ZIP과 추출된 배포 파일을 보존했다. 재현 코드와 출력은 감사 폴더에 저장했다. 결함 수정과 재감사는 미수행이다.
