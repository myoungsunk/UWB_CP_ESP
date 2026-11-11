# Mode 8 앵커 펌웨어 결함 감사

- 질문 ID `UWB-MODE8-SOURCE-HOST-QA-20261111`. 2026-11-11 16:51 KST. AUDITOR 자기 검토.
- 정책 `RESEARCH-GOVERNANCE-v1`. COMMON SHA-256 `a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb`, AUDITOR SHA-256 `e1bdc2f231c71cdc27201c688e041b078576a6adf6c0290acc16ce4d8218291a`.
- COM4에 검증된 앵커 앱 SHA-256 `6ba6517b3504e1349f3c668636ec1d94b09f37ed47982edce66b54c492a73336`. 현재 소스 매니페스트를 재계산해 빌드 기록의 `b22502a268b1b5d3d660a45c3669ef8b81e0e4d8682bcd8029b0c3d5c1bc9340`과 대조했다. ZIP 기반 소스 패키지다. [플래시 검증 기록](../flash/flash_receipt.json)
- **판정 REDIRECT.** 아래 결함이 열려 있다. 검증 범위는 소스, 과거 실기록, 가상 SPI·USB 입력으로 실행한 호스트 재현이다. 실제 DW1000에서 최초 `CLKPLL_LL`을 유발한 조건은 확정할 수 없다. 빌드·플래시·실물 RF 재시험은 미실행이다.

## 결함 및 재현

| ID | 결함 | 근거와 영향 | 재검증 기준 |
|---|---|---|---|
| A1 | START와 실제 송신에 서로 다른 PLL 판단 기준 적용 | `verifyIdleClock()`은 `RF_STATUS`의 CPLLLOCK/CPLLLOW/CPLLHIGH까지 읽는다. `send()`는 송신 직전 `SYS_STATUS`만 읽는다. 가상 SPI에서 정상 초기화 후 POLL 버퍼 설정 시 `RF_STATUS=0x03`, `SYS_STATUS=0x00800002`을 공급하자 TXSTRT 호출 3회, Stage 2 `complete=true`였다. 이 결과는 펌웨어의 송신 허가와 기록 판정을 재현한다. | 동일 반례에서 TXSTRT 호출 0회, 거래 미완료 및 원인 기록. |
| A2 | 실제 RF 설정을 검증하지 않고 초기화·SELFTEST 통과 | 소프트웨어 설정은 채널 5, 가상 칩의 `CHAN_CTRL`은 채널 2로 설정했다. 설정 후 이 레지스터를 읽지 않았고 `initialize()`와 `SELFTEST` 모두 통과했다. 앱의 `config_id`는 설정 의도에서 계산되므로 RF 칩이 적용한 값을 증명하지 못한다. 실제 보드가 채널 2였다는 근거는 없다. | 설정 쓰기 실패를 주입하면 불일치 검출 및 START 거부. |
| A3 | PLL 오류 뒤에도 고속 SPI로 정지 명령 | `SYS_STATUS=0x02800002`을 주입하면 송신은 차단되지만 `trip()`은 현재의 FAST 설정으로 `forceTRxOff()`를 호출한다. PLL이 실제 미잠금이면 DW1000 SPI 허용치는 3MHz다. `PLLSTATUS`도 저속 조회보다 고속 조회를 먼저 한다. 실제 송신 정지 성공 여부는 이 시험으로 알 수 없다. | 가상 SPI에서 오류 경로의 정지 명령이 저속으로 발행되는지 확인하고, 실기에서 레지스터 및 정지 상태 확인. |
| A4 | 오류 상세와 준비 상태가 거래·진단에 일관되게 반영되지 않음 | 최초 실물 Stage 2에서 `TX.status=0x02800002`, 거래 `errors=2`(`ErrPoll`)와 별도 `HARDWARE_FAULT_LATCHED`가 기록됐다. 재현에서도 거래에 `ErrHardware` 비트가 없었다. 초기화가 통과한 뒤 오류를 주입하면 `PLLSTATUS`는 실시간 RF 상태 `0x03`과 `ready=true`를 함께 출력한다. `ready`는 초기화 당시 값이다. 전체 수집기는 별도 오류 이벤트와 미완료 거래도 검사한다. | 하드웨어 오류 분류가 거래에 반영되고 초기화 결과 및 현재 상태가 구별돼야 한다. |
| A5 | USB 명령의 잘못된 입력 처리 | `SET STAGE 7`은 응답 없이 반환한다. `START 12 1<NUL>ignored<LF>`는 NUL에서 잘려 세션을 활성화했다. 가상 USB에 바이트를 전달해 실제 서비스 함수에서 재현했다. | 잘못된 SET은 오류 응답. NUL 포함 명령 전체 거부 및 START 상태 불변. |

[앵커 하드웨어 연결 코드](../../shared/esp32_port.hpp) · [앵커 거래 코드](../../shared/engine.hpp) · [USB 명령 코드](../../shared/app.hpp) · [최초 Stage 2 앵커 기록](../../../UWB_MODE8_R5_PAIR_RF_20261111/stage2_first/anchor.jsonl) · [Qorvo DW1000 데이터시트 표 2](https://store.qorvo.com/datasheets/qorvo/dwm1000datasheet.pdf)

이전 조사에서 확인한 잠금 확인 전 고속 SPI 전환과 PMSC 쓰기 순서 차이도 수정되지 않은 상태다. A1의 반례는 송신 직전 보호가 뚫리는 경우를 보여 준다. 실제 첫 Stage 2는 이미 오류 비트가 켜진 상태를 감지해 송신 요청 전에 중단했다. [이전 앵커 초기화 조사](../anchor_firmware_investigation_20261111.md)

## 실행 증거

- `wsl -d Ubuntu-22.04 -- bash evidence/anchor_firmware_code_audit_20261111/run.sh` 종료 0. GCC 11.4 및 ASan·UBSan에서 반례 재현, PLL 정책·앵커 구문 검사 통과, 합성 assertion 11,413개 통과. [재현 소스](repro.cpp) · [실행 스크립트](run.sh) · [호스트 로그](host_repro.log)
- `py -3 -B -X utf8 -m unittest discover -s tests -p 'test_*.py'` 종료 0, 기존 67개 통과. [Python 로그](python_tests.log)
- SHA-256 `repro.cpp` `feedf8c76b7f8ee7b6d6bd56d37b9a82a24fdcbc1f1c8a82cda660ab290ef122`, `run.sh` `f2f674c0952b2e769168c9968bcc89597e7a2cce4f3f2fecd4a5129bb9f5b83b`, `host_repro.log` `7e1287b9e0ab2b140a080dbec6ddc4254d2756ed3a079ad372395c9b84b2bb27`, `python_tests.log` `66e98aab4c8d5a272577035175940873ca9c0905cf4c913dba8cca340870c79b`, 이전 실물 `anchor.jsonl` `3963afc835f4e6118cd705a55d498bac30c4e9934cd94e1e7a7e3f8f870874a3`.
