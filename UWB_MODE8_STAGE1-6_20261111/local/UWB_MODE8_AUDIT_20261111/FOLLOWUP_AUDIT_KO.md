# Mode 8 추가 감사

**2026-11-11 KST · 판정 REDIRECT 유지**

[기존 감사](AUDIT_KO.md)와 같은 질문, 정책, 입력 ZIP SHA-256 `4a5f27ff6cc4f3bca5c1b7c27f5ff53e1acb80cbed36a5646ed027fd9864f11c`를 사용했다. 태그 상태기계의 두 경로와 P1 보정 정보의 결합을 합성 입력으로 확인했다. 제공 소스에는 변경을 가하지 않았다.

## 추가로 재현한 결함

### M8-03 · 높음 · 하드웨어 결함을 기록한 뒤 상대 모듈의 동작을 계속함

`copyAccumulator()`의 마지막 `guard()`는 장치 ID, 안테나 지연, 시계 검사 실패를 래치한다. `readCir()`는 이 상태를 일반 CIR 읽기 실패와 동일한 `false`로 반환한다. `waitPair()`는 `ErrCir`만 기록한 뒤 남은 CIR을 읽는다. 다음 P2 준비에서도 첫 라디오의 arm이 실패한 뒤 상대 라디오의 arm을 시도한다. [장치 검사 및 CIR 읽기](UWB_DUAL_DW1000_MODE8_20261111/shared/esp32_port.hpp#L36) · [CIR 이후 상태 전이](UWB_DUAL_DW1000_MODE8_20261111/shared/engine.hpp#L25)

제공된 호스트 라디오 모델에서 P1의 A CIR 읽기 직후 무결성 오류를 주입했다. 오류 래치 이후 B의 CIR 읽기 1회와 B의 P2 RX 준비 1회가 호출됐다. 이 결과는 펌웨어 제어 흐름에 관한 것이며 실물에서의 RF 송수신을 측정한 결과는 없다. 일반 CIR 불량의 부분 데이터 보존을 유지하면서 하드웨어 래치 때는 양쪽 수신을 정지하고 후속 읽기와 arm이 실행되지 않는지 검증해야 한다. [재현 로그](replay/followup_engine.log)

### M8-04 · 높음 · Stage 2에서 한쪽 POLL 누락을 완료로 집계함

`runTag()`는 B POLL이 없으면 `ErrPoll`을 기록한다. `finalizeRanges()`는 Stage 2의 `complete`를 메타데이터 충돌 여부와 네 CIR 슬롯의 유효성으로 계산하고 POLL 수신을 검사하지 않는다. 호스트 모델에서 B POLL을 누락시키고 양쪽 P1/P2를 수신시킨 결과 `B_poll_received=0`, `ErrPoll=1`, `complete=1`이 동시에 나왔다. 이 레코드는 이전 감사의 M8-02를 수정해 세션을 엄격히 검사하더라도 완료로 집계될 수 있다. Stage 2의 완료 기준에 A/B POLL 수신을 반영하고, POLL 누락 시 원시 P1/P2는 보존되는지 확인해야 한다. [POLL 누락 처리](UWB_DUAL_DW1000_MODE8_20261111/shared/engine.hpp#L70) · [완료 조건](UWB_DUAL_DW1000_MODE8_20261111/shared/core.hpp#L141) · [재현 로그](replay/followup_engine.log)

### M8-05 · 중간 · 다른 구성의 앵커 기록에서 P1 보정 상태를 가져옴

앵커 로그의 조회 키는 session, txn, 앵커 boot다. P2 거리 계산은 이어서 config와 상대 boot도 대조하지만, P1의 `both_endpoints_calibrated`는 조회된 앵커의 보정 ID를 그대로 사용한다. 실제 분석 명령에 config와 상대 boot가 모두 다른 앵커 레코드를 제공하자 A/P1에 `both_endpoints_calibrated=True`가 출력됐다. 이 반례에서 P1 거리의 수치에는 영향이 없었으며, 보정 상태를 기준으로 결과를 선별할 때 오분류가 생긴다. 양쪽 식별자가 일치할 때만 앵커 보정 정보를 사용하고, 결합할 수 없는 로그에서도 P1 거리와 보정 상태를 각각 올바르게 표시하는지 검증해야 한다. [앵커 로그 결합](UWB_DUAL_DW1000_MODE8_20261111/tools/analyze.py#L154) · [보정 표시](UWB_DUAL_DW1000_MODE8_20261111/tools/analyze.py#L101) · [재현 출력](replay/followup_calibration.json)

## 재현 범위

- Ubuntu 22.04, GCC 11.4, AddressSanitizer·UndefinedBehaviorSanitizer로 `g++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -Ishared -Itests ../replay/followup_repro.cpp -o ../replay/followup_repro` 실행. 생성된 호스트 실행 파일의 종료 코드 `0`은 두 상태기계 반례의 재현을 뜻한다. [재현 코드](replay/followup_repro.cpp)
- Windows Python에서 `py -3 -B ../replay/followup_calibration.py` 실행. 종료 코드 `0`은 P1 보정 표시 반례의 재현을 뜻한다. [재현 코드](replay/followup_calibration.py)
- 기존 보고서의 850 kbps Smart TX Power 설정과 기준 ZIP 차이도 남아 있다. ESP32 타깃 빌드, 보드 접근과 실물 RF 검증은 미실행이다.
