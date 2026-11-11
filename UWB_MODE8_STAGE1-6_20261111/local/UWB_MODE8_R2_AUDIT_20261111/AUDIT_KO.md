# Mode 8 R2 소스 재감사

2026-11-11 KST · `UWB-MODE8-SOURCE-HOST-QA-20261111` · **REDIRECT — 감사 미통과, ESP32 빌드 보류**

제출 ZIP `UWB_DUAL_DW1000_MODE8_R2_ADVERSARIAL_REAUDITED_20261111.zip`의 SHA-256은 `f2c8a34b993ca71d59eccf95e5c80edbe9430f0ee294967d337f4651f76876e4`다. ZIP 142개 항목의 CRC, 배포 목록 140개 파일의 SHA-256, 공유 소스 동기화를 확인했다. R1과 비교한 vendor 50개 파일과 동봉 원본 ZIP의 해시가 일치했다. 압축 원본과 패키지 소스를 수정하지 않았다.

## 기존 결함 5건

| 기존 ID | R2 재검증 결과 |
| --- | --- |
| M8-01 | **해소.** 기존 P2 프레임 충돌에서 B 거리와 물리 지연축이 무효이며, P2 공통축도 생성되지 않았다. 원시 IQ와 FP 상대축은 보존됐다. [재현 코드](replay/host_repro.py#L53) |
| M8-02 | **해소.** 세션 76·중복 거래·시퀀스 누락을 주입한 기존 사례에서 수집기가 오류 사유를 기록하고 `pass=false`, 종료 코드 2를 반환했다. [회귀시험](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/tests/test_adversarial.py#L289) |
| M8-03 | **미해소, 높음.** A의 CIR 읽기 직후 무결성 오류가 래치돼도 엔진은 먼저 `ErrCir`로 처리한다. 포트 반환 계약을 모사한 시험에서 B의 CIR 읽기와 B의 P2 수신 준비가 각각 한 차례 계속됐다. 하드웨어 오류를 감지한 지점에서 두 모듈의 거래를 중단하고, 일반 CIR 실패의 부분 기록을 유지하는 경로를 확인해야 한다. [포트](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/shared/esp32_port.hpp#L56) · [엔진](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/shared/engine.hpp#L49) · [재현](replay/engine_repro.log) |
| M8-04 | **해소.** B POLL 누락 시 `ErrPoll=1`, `complete=0`을 확인했다. [완료 조건](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/shared/core.hpp#L147) · [재현](replay/engine_repro.log) |
| M8-05 | **미해소, 중간.** 다른 태그 부팅 ID와 설정을 가진 앵커 기록을 입력하자, P2 거리는 무효였으나 P1의 `both_endpoints_calibrated`가 `true`였다. 양단 보정 표시에 사용하는 앵커 기록에도 거래·부팅 ID·설정 일치 검사가 필요하다. [분석 코드](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/tools/analyze.py#L216) · [재현](replay/host_repro.json) |

## 추가 발견

### R2-01 · 높음 · 정상 Stage 2를 수집기가 거부

Stage 2에는 태그 응답이 없다. 앵커는 태그 부팅 ID를 알 수 없어 거래 기록과 P1/P2에 `peer_boot=0`을 남긴다. 펌웨어의 앵커 상태기계가 만든 완료 기록과 RF 바이트가 일치하는 합성 태그 완료 기록을 감사기에 넣자, 태그 부팅 ID 불일치로 세 차례 오류가 발생했다. Stage 2의 부팅 ID 검사에 이 프로토콜 규칙을 적용하고, Stage 3 이상에서 양단 일치 검사를 유지해야 한다. [앵커 상태기계](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/shared/engine.hpp#L99) · [수집 검사](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/tools/capture_audit.py#L80) · [재현](replay/host_repro.json)

### R2-02 · 높음 · 분석기가 모순된 기록에서도 물리 지연축을 생성

- 양쪽 P2 수신 슬롯을 미수신으로 바꾸고 공용 P2 메타데이터를 남긴 합성 기록에서, 분석기가 A/B P1 거리 약 5.0007 m와 5.1100 m 및 P1 공통축을 생성했다. P1 송신 확인에는 실제로 일치하는 P2 수신 슬롯이 하나 이상 필요하다. [수신 확인 로직](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/tools/analyze.py#L136) · [재현](replay/host_repro.py#L77)
- B의 로컬 RESPONSE 기록을 A의 식별자로 바꾸자 B P1은 무효였으나 B P2는 약 5.1104 m로 유효 처리됐다. P2 독립 거리 계산도 로컬 RESPONSE의 모듈·유형 일치를 검사해야 한다. [P2 거리 계산](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/tools/analyze.py#L198) · [재현](replay/host_repro.py#L87)

### R2-03 · 중간 · 최종 STOP 송신 실패 후에도 성공 보고

정상 합성 Stage 5 거래를 받은 뒤 양쪽 직렬 포트의 마지막 `STOP` 전송만 실패시켰다. 수집기는 종료 코드 0과 `pass=true`를 반환했고 오류를 남기지 않았다. 최종 STOP 송신 오류를 기록하고, 수집 데이터의 유효성과 장치 종료 확인 결과를 각각 표시해야 한다. [수집 종료 코드](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/tools/run_pair.py#L231) · [재현](replay/host_repro.py#L101)

## R2 보완 사항과 검증 범위

- P1 타임스탬프 교차 확인, 하드웨어·메타데이터·지연 오류의 거리 유효화 차단, CIR 내부 CRC 거부를 확인했다. 오류 직후의 수신 동작과 분석기의 기록 불일치 처리에는 위 결함이 남는다.
- P2 독립 거리의 앵커 부팅 ID·실제 TX 시각 대조와 회귀시험을 확인했다. 850 kbps 설정에서 Smart TX Power가 꺼졌으며, 기본 설정 ID는 R1 `181770294`에서 R2 `3839621402`로 바뀌었다.
- 최종 수집 감사가 리더 종료 후 스트림 누락·오류를 확인한다. 마지막 STOP 전송 오류는 위 재현처럼 감사 결과에 반영되지 않는다.
- Python 42개 시험과 GCC C++ 11,342개 assertion을 재실행했다. AddressSanitizer와 UndefinedBehaviorSanitizer의 오류 보고는 없었다. 두 역할 소스의 호스트 구문 검사도 통과했다. 추가 반례는 C++ 포트 모형, 합성 기록, 모의 직렬 포트로 재현했다. [Python 로그](replay/python-tests.log) · [GCC 실행 로그](replay/gcc-core-test.log) · [추가 재현 결과](replay/host_repro.json)
- [제출된 ESP32 빌드 상태](UWB_DUAL_DW1000_MODE8_REAUDITED_20261111/dist/target_build_receipt.json)는 `target_build=false`이며 `dist`에는 빌드 상태 파일만 있다. 이번 재감사에서 ESP32 컴파일, 업로드, 실물 RF 시험은 수행하지 않았다.

## 실행 기록

압축을 푼 패키지 폴더 기준이다. PowerShell에서 아래 Python 명령은 모두 종료 코드 0으로 끝났다. 마지막 명령의 종료 코드는 재현 스크립트의 assertion 결과이며, 그 안에서 개별 수집기의 종료 코드를 별도로 확인했다.

```powershell
py -3 -B tools/verify_package.py
py -3 -B tools/sync_shared.py --check
py -3 -B -m unittest discover -s tests -p 'test_*.py' -v
py -3 -B ../replay/host_repro.py
```

Python 추가 재현에 쓰인 `stage2_anchor.json`은 WSL Ubuntu 22.04에서 아래와 같이 생성했다. 각 명령의 종료 코드는 0이었다.

```sh
g++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -Ishared tests/test_core.cpp -o ../replay/test_core
../replay/test_core ../replay
g++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -Ishared -Itests ../replay/engine_repro.cpp -o ../replay/engine_repro
../replay/engine_repro ../replay/stage2_anchor.json
```

| 결과 파일 | SHA-256 |
| --- | --- |
| `engine_repro.log` | `1940eb0f99402eb9f31d07a097420934376eac3fa274928c65402def27eb9fe4` |
| `host_repro.json` | `4b2cd9d3c33cfc7078680a7b9a67a1fbd0dcd4c86205a5ec828ac44274675a23` |
