# Mode 8 R5 호스트 적대적 재감사

**2026-11-11 · 재현 결함 3건과 CP949 시험 오류 수정 · 호스트 재감사 통과**

질문 ID는 `UWB-MODE8-SOURCE-HOST-QA-20261111`이다. 입력은 R4 ZIP `UWB_DUAL_DW1000_MODE8_R4_SOURCE_HOST_REAUDIT_20261111.zip`이며 SHA-256은 `2a4342497ab9a693338ea74d7d7abc7ae239c1d0ffb90e19187eb8de0a488a93`이다. R4 원본과 이전 감사 기록을 보존하고 R5 사본에서 작업했다. 같은 작업자가 수정과 재감사를 수행했다.

| 확인 항목 | 회귀시험과 재감사 결과 |
| --- | --- |
| R4-A1 · 한쪽 POLL 누락 | R4 감사의 C++ 직렬화 기록 두 건을 그대로 회귀시험에 넣었다. A가 놓치면 B의 P1 **5.112298 m**, B가 놓치면 A의 P1 **4.998371 m**가 보존됐다. 미수신 쪽 거리는 `null`이고, 원시 IQ와 FP 상대축은 보존됐다. POLL을 미수신으로 표시하고 RF 프레임을 남긴 모순 입력은 양쪽 물리축을 거부했다. |
| R4-A2 · 앵커 TX 불일치 | 수집기와 분석기가 같은 함수로 POLL/P1/P2의 송신 슬롯, 모듈, 타입과 예약·실제·프레임 시각을 확인한다. POLL 예약 시각, P1 유형, P2 모듈을 각각 변조하면 P1 거리와 상대축은 유지되고 P2 거리는 둘 다 `null`, P2 공통 그리드는 **0점**이었다. `--assume-static`을 지정한 경우에도 TX 모순은 같은 결과였다. 정상 앵커 기록의 P2 공통 그리드는 **993점**이었다. |
| R4-A3 · 기존 BIN 재사용 | 버전 2 빌드 기록에는 `shared/`, 역할별 스케치, 지정 vendor 라이브러리 파일을 포함한다. 스케치의 `partitions.csv`·`bootloader.bin` 추가, 변경, 제거 후 기존 빌드 기록을 거부했다. 동결 R3 이미지와 ESP32 Core 3.3.7의 실제 파티션 생성기로 R4 반례를 재현했다. 파티션 산출물 SHA가 달라졌고, 이전 이미지 자체의 SHA가 맞는 상태에서도 R5가 업로드 코드 **2**를 반환했다. 모의 업로더 호출은 **0회**였다. |
| Windows CP949 | FCS 회귀시험에서 vendor 소스와 어댑터 헤더를 UTF-8로 명시해 읽는다. Windows Python 3.13.15의 CP949 환경에서 UTF-8 모드를 끄고 전체 Python **67개 시험**을 통과했다. 동결 R4의 동일 시험은 `UnicodeDecodeError`를 재현했다. |

부분 기록 분석은 유효한 채널을 보존한다. 전체 수집 성공에는 양쪽 정상 기록과 종료 확인이 필요하다. 앵커·태그 C++와 vendor 파일은 R4와 바이트 단위로 일치하며 장치 기록 식별자 `D8_R4_SOURCE_20261111`를 사용한다.

## 실행 기록

- Windows Python 67개, GCC ASan·UBSan 합성 assertion 11,413개, 앵커와 태그 C++ 호스트 구문 검사 모두 통과. [Python](../evidence/r5/python_cp949.log) · [GCC](../evidence/r5/gcc_host.log) · [구문 검사](../evidence/r5/gcc_syntax.log)
- 동결 R4에서 회귀시험이 결함을 실제로 검출하는지 확인했다. R5 분석 명령에서 RFB8 읽기, 앵커 기록 결합, CSV와 NPZ 출력까지 재실행했다. [R4 대조 로그](../evidence/r5/against_r4.log) · [회귀시험](../tests/test_r5_regression.py) · [RFB8 분석 결과](../evidence/r5/analysis_cli.json)
- 이전 이미지 차단과 배포본의 업로드 차단을 확인했다. 제공된 빌드 기록의 `target_build`는 `false`다. [파티션 재현](../evidence/r5/legacy_partition.json) · [배포본 사전 검사](../evidence/r5/as_delivered_preflight.json)

ESP32 타깃 빌드, 플래시, 보드 USB 및 실물 RF 시험은 미실행이다. 거리는 합성 입력에서의 계산 결과다.

적용한 로컬 연구 지침의 SHA-256은 COMMON `a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb`, IMPLEMENTER `2380fb1fe7302870a6c2d825cc2e65a73e91138677e095b4e51db43ab9ea90da`, AUDITOR `e1bdc2f231c71cdc27201c688e041b078576a6adf6c0290acc16ce4d8218291a`다.
