# Mode 8 R3 적대적 재감사

**2026-11-11 · ACCEPT · 다섯 결함의 호스트 검증 범위**

질문 ID는 `UWB-MODE8-SOURCE-HOST-QA-20261111`이다. 입력 R2 ZIP의 SHA-256은 `f2c8a34b993ca71d59eccf95e5c80edbe9430f0ee294967d337f4651f76876e4`다. R2 원본과 기존 감사 증거를 보존하고, 별도 R3 작업본을 사용했다. 같은 작업자가 구현 후 감사 역할로 전환해 반례를 재검증했다.

| 결함 | R3의 변경과 재검증 결과 |
| --- | --- |
| M8-03 · CIR 무결성 오류 | 포트가 일반 CIR 불량과 하드웨어 무결성 오류를 구분한다. A→B/B→A 읽기 순서와 P1/P2 양쪽에서 첫 모듈 오류 후 다음 모듈을 읽거나 후속 수신을 시작하지 않았다. 먼저 복사한 바이트와 CRC는 진단용으로 보존했고, 해당 슬롯과 거래의 거리는 무효 처리했다. 일반 반복 읽기 불일치에서는 두 모듈 처리를 계속했다. [상태기계](../shared/engine.hpp) · [포트](../shared/esp32_port.hpp) · [시험](../tests/test_core.cpp) |
| M8-05 · 보정 표시 | 일치하는 앵커와 태그의 보정 기록에만 양단 보정 표시를 붙인다. 앵커 설정, 상대 부팅 ID, 세션, 거래 번호를 바꾸면 표시가 거짓이 됐다. 태그가 자체 확인한 P1 거리값은 보존됐다. [분석](../tools/analyze.py) · [파일 재현](../evidence/r3/adversarial_replay.json) |
| R2-01 · 정상 Stage 2 거부 | Stage 2의 앵커 기록과 두 Probe에 `peer_boot=0`을 요구하고, 태그 자체 부팅 ID와 상대 앵커 ID는 계속 검사한다. 앞서 거부된 C++ 앵커 기록이 통과했다. 양단에서 동일한 P2 바이트로 바꿔 CRC를 다시 맞추더라도 조작된 태그 부팅 ID는 거부했다. 누락 POLL과 가짜 RESPONSE도 거부했다. [수집 감사](../tools/capture_audit.py) · [파일 재현](../evidence/r3/adversarial_replay.json) |
| R2-02 · 모순된 거리 증거 | 실제 P2 수신 슬롯을 하나 이상 요구하고, P1/P2의 로컬 RESPONSE 모듈·유형·송신 시각을 검사한다. P2 거리에는 앵커가 수신한 RESPONSE의 원본 바이트도 대조한다. P2 두 슬롯이 빠지면 거리와 공통 물리 지연축이 생성되지 않았다. B RESPONSE를 A로 바꾸면 B의 두 거리가 무효였고 A의 거리와 원시 CIR·FP 상대축은 보존됐다. [분석](../tools/analyze.py) · [파일 재현](../evidence/r3/adversarial_replay.json) |
| R2-03 · STOP 실패 후 성공 | 수집기가 `STOP <request>`를 보내고 양쪽에서 해당 요청·역할·세션과 일치하는 `STOPPED`를 기다린다. 송신 예외, 짧은 쓰기, 오래된 응답, 잘못된 역할 또는 세션은 전체 종료 코드 2가 됐다. 올바른 응답 직전에 도착한 중복 레코드도 수집 판정에서 거부됐다. [수집기](../tools/run_pair.py) · [Python 시험](../evidence/r3/python-tests.log) |

## 실행 근거

- WSL Ubuntu 22.04의 GCC로 C++ 합성 assertion 11,413개를 실행했다. ASan·UBSan 오류 보고가 없었다. 두 역할은 실제 vendor 헤더와 호스트 선언 스텁으로 구문 검사를 통과했다. [명령과 종료 코드](../evidence/host/host_qa_receipt.json) · [C++ 로그](../evidence/host/core_test.log)
- Windows에서 `py -3 -B -m unittest discover -s tests -p 'test_*.py' -v`로 54개 시험을 실행해 종료 코드 0을 확인했다. 정상 Stage 2·5와 기존 P2 충돌, 잘못된 세션·중복 거래, CIR CRC 오류를 포함한다. [Python 로그](../evidence/r3/python-tests.log)
- `py -3 -B evidence/r3/replay.py`가 종료 코드 0을 반환했다. 원래 Stage 2 반례의 앵커 파일을 재사용했고, 분석 CLI에서 실제 RFB8 파일 입출력과 CSV·NPZ를 확인했다. [재현 코드](../evidence/r3/replay.py) · [결과](../evidence/r3/adversarial_replay.json)
- 같은 Python 회귀시험 중 기존 반례 5개를 동결된 R2 호스트 코드에 연결하자 예상한 assertion 실패가 모두 재현됐다. CIR 무결성 오류의 R2 상태기계 실패는 당시 별도 C++ 재현 로그로 확인했다. [R2 대조](../evidence/r3/r2_negative_control.json) · [R2 엔진 로그](../evidence/r3/r2_engine_repro.log)
- 설정 계약 바이트를 3으로 올렸다. R3 기본 Config ID는 `2480720268`이며 R2 기본 ID `3839621402`와 다르다. 앵커·태그와 수집기를 R3 조합으로 사용한다. 일반 `STOP` 명령도 지원한다.

이 판정은 지정된 결함과 회귀 범위에 적용한다. `STOPPED`는 펌웨어가 정지 명령을 처리했다는 직렬 응답이다. 물리적 RF 정지, 거리 정확도, 실물 보드 동작은 검증 범위 밖이다. 타깃 컴파일 판정은 [빌드 결과](../dist/target_build_receipt.json)에 기록한다.

## 재감사 이후 ESP32 타깃 빌드

Arduino IDE에 포함된 Arduino CLI 1.5.1과 로컬 Arduino ESP32 Core 3.3.7에서 `tools/build.py --cli <Arduino CLI 경로>`로 두 역할을 빌드했다. FQBN은 `esp32:esp32:esp32doit-devkit-v1`이며 두 컴파일과 링크의 종료 코드는 모두 0이었다. 산출물 12개의 크기와 SHA-256을 다시 확인했다. [빌드 기록](../dist/target_build_receipt.json)

| 역할 | 프로그램 저장 공간 | 전역 변수 메모리 |
| --- | ---: | ---: |
| 앵커 | 388,248 B | 95,932 B |
| 태그 | 392,080 B | 92,900 B |

기존 양쪽 DW1000Ng의 `getRCphase()`에서 배열 범위 경고가 나왔다. Mode 8 호출부에서 참조가 없으며, `xtensa-esp32-elf-nm -C`로 확인한 두 최종 ELF에도 해당 함수와 `readRCphase()` 심볼이 없었다. R2와 비교해 vendor 50개 파일의 해시는 일치한다. [앵커 로그](../dist/anchor/compile.log) · [태그 로그](../dist/tag/compile.log) · [소스 해시 대조](../evidence/r3/source_delta.json)

## 적용 지침

로컬 RESEARCH-GOVERNANCE-v1을 적용했다. SHA-256은 COMMON `a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb`, IMPLEMENTER `2380fb1fe7302870a6c2d825cc2e65a73e91138677e095b4e51db43ab9ea90da`, AUDITOR `e1bdc2f231c71cdc27201c688e041b078576a6adf6c0290acc16ce4d8218291a`다.
