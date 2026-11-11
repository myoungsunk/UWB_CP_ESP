# Mode 8 R6 앵커 펌웨어 적대적 재감사

질문 ID `UWB-MODE8-SOURCE-HOST-QA-20261111` · 2026-11-11T17:12:07+09:00 · 구현 후 호스트 적대적 자체 재감사

**호스트 판정 PASS.** 앵커 PLL 초기화 경로와 A1–A5의 재현 조건을 확인했다. 기존 R5 소스·빌드·플래시 증거는 원래 패키지에 보관하고, 수정과 검증은 이 R6 패키지에서 진행했다. [R5 감사](../../UWB_MODE8_R5_ANCHOR_PLL_STABILITY_20261111/evidence/anchor_firmware_code_audit_20261111/REPORT_KO.md) · [R5 반례 로그](../../UWB_MODE8_R5_ANCHOR_PLL_STABILITY_20261111/evidence/anchor_firmware_code_audit_20261111/host_repro.log)

| 항목 | 수정 | 회귀시험 결과 |
|---|---|---|
| PLL 초기화 | 2 MHz에서 DEV_ID를 확인하고 필요한 경우 MISO 1비트 보정을 선택한다. PMSC의 하위·상위 바이트를 순서대로 쓰고, XTI에서 자동 시퀀싱을 멈춘 상태로 LDO 보정, XTAL trim, PLL 보정, LDE 로드를 적용한 뒤 AUTO로 돌아간다. 현재 잠금이 정상인 경우에만 부팅 중의 CLKPLL_LL을 한 번 해제하고 3초간 상태를 검사한다. | 드라이버 레지스터 순서, 기본·보정 읽기, 주요 초기화 쓰기 유실, 잘못된 DEV_ID, 약 2.3초 후 잠금 이상, 제한된 재시도, 잠금 상실 검출기 비활성화와 잘못된 상태 읽기를 확인했다. |
| A1 송신 허가 | POLL과 각 Probe에서 RF_STATUS·SYS_STATUS·잠금 상실 검출기를 송신 시작 전과 완료 판정 시 읽는다. | R5에서 Stage 2가 완료됐던 RF_STATUS 0x03 / SYS_STATUS 0x00800002 조합에서 TXSTRT 0회, 거래 실패. P1 직전 및 P2 타임스탬프 읽기 중의 오류도 차단했다. |
| A2 RF 설정 | 초기화·START·SELFTEST에서 SYS_CFG, CHAN_CTRL, TX_FCTRL, FS_PLLCFG, FS_PLLTUNE을 다시 읽는다. | CHAN_CTRL 채널 2와 요청 채널 5 불일치, 나머지 네 레지스터의 변조를 거부했다. 실제 vendor 설정 함수가 기대 레지스터 값을 쓰는지도 모의 SPI에서 확인했다. |
| A3 오류 시 SPI | 앵커의 초기화·일반 동작·정지·진단에 2 MHz를 사용한다. | PLL 오류 시 TRXOFF를 저속에서 한 번 발행한다. 고속 진단 읽기는 없으며 `fast_sampled=false`가 기록된다. 실제 무선 정지 확인은 실물 시험에 남아 있다. |
| A4 거래·진단 | 거래에 송신 단계 오류와 ErrHardware를 같이 기록하고, 하드웨어 오류 후 후속 Probe를 중단한다. PLLSTATUS는 startup_ready와 현재 ready를 표시하고 오류 발생 시 RF_STATUS와 EC_CTRL을 보존한다. | 최초 POLL의 기존 SYS_STATUS 0x02800002를 주입하면 `errors=3`, TXSTRT 0회였다. 실시간 RF_STATUS가 회복돼도 오류가 래치된 동안 ready=false가 유지됐다. 일반 P1 타임스탬프 불일치에서는 P2가 계속 가능했다. |
| A5 USB 명령 | 범위 밖 STAGE·VERIFY_CIR에 오류 응답을 보낸다. NUL·라인 내부 CR·기타 비허용 바이트를 포함한 명령 전체를 거부한다. | NUL을 포함한 START가 세션을 열지 않았다. 범위 오류, 라인 길이 초과, 비ASCII, 분할 CRLF와 정상 후속 명령을 확인했다. |

[앵커 하드웨어 연결 코드](../shared/esp32_port.hpp) · [앵커 드라이버](../vendor/anchor/DW1000Ng/src/DW1000Ng.cpp) · [거래 상태기계](../shared/engine.hpp) · [USB 명령 코드](../shared/app.hpp)

## 실행 증거

- `wsl -d Ubuntu-22.04 -- python3 -B -X utf8 tools/host_qa.py` 종료 0. GCC 11.4, ASan·UBSan에서 앵커 회귀시험과 실제 vendor 드라이버의 모의 SPI 시험을 통과했다. 기존 합성 assertion 11,413개, Python 단위시험 67개, 양 역할 C++ 구문 검사, 공용 헤더 동기화 검사도 통과했다. [호스트 실행 기록](host/host_qa_receipt.json) · [앵커 회귀 로그](host/anchor_regression.log) · [드라이버 회귀 로그](host/anchor_driver_regression.log)
- `py -3 -B -X utf8 -m unittest discover -s tests -p 'test_*.py'` 종료 0, Windows에서 67개 통과. [Windows 로그](windows_python.log)
- [앵커 회귀 소스](../tests/test_anchor_firmware.cpp) · [PLL 드라이버 회귀 소스](../tests/test_anchor_pll_driver.cpp)
- 기준 R5 앵커 소스 매니페스트 SHA-256 `b22502a268b1b5d3d660a45c3669ef8b81e0e4d8682bcd8029b0c3d5c1bc9340`. R5 반례 로그 SHA-256 `7e1287b9e0ab2b140a080dbec6ddc4254d2756ed3a079ad372395c9b84b2bb27`.

## ESP32 타깃 빌드 및 사전 검사

Arduino ESP32 Core 3.3.7, `esp32:esp32:esp32doit-devkit-v1`에서 앵커·태그 빌드가 모두 종료 0으로 완료됐다. 태그 빌드는 공용 USB 코드의 타깃 호환성을 확인하기 위해 함께 수행했다. `tools/offline_preflight.py` 종료 0으로 현재 소스와 빌드의 연결, 이미지 SHA-256, ESP32 및 Xtensa 헤더, 병합 이미지 오프셋을 양쪽 모두 확인했다. [타깃 빌드 기록](../dist/target_build_receipt.json) · [오프라인 사전 검사](offline_preflight.json)

| 역할 | 소스 매니페스트 SHA-256 | 애플리케이션 BIN SHA-256 |
|---|---|---|
| anchor | `0262ec1e59437fe519fe25b1f3f8761524a439386c6bbc7616358890d51cf556` | `7448a52755023c41d1d760e86f65c1a70c8d6addc9f9322723feea263c134f06` |
| tag | `6257315f8ada57ec639d79885c86520126e6918ee8ccf2aa2a29d3a53f0f8f45` | `662721eb983dd2cbc74265bab09c9954d91429a90de5c098d16d298a3b9d0dea` |

## 적용 범위

- 앵커는 HELLO와 거래 기록에 `D8_R6_ANCHOR_20261111`을 표시한다. 태그 역할 식별자, RFB8 형식과 RF 설정 계약 ID는 기존 값으로 유지했다. USB 입력 검사는 공용 코드에 적용된다.
- 벤더의 기존 getRCphase 배열 경계 경고는 앵커 컴파일 로그에 남아 있다. Xtensa 심볼 검사에서 getRCphase와 readRCphase는 Mode 8 앵커 ELF에 포함되지 않았다. [컴파일 로그](../dist/anchor/compile.log) · [ELF 심볼 확인](anchor_elf_symbol_check.log)
- 플래시·USB 실물 점검·Stage 2 RF 재시험은 미실행이다. 실제 첫 POLL의 CLKPLL_LL 재발 여부와 2 MHz에서의 실물 송수신 타이밍은 검증 대기 상태다.
- 참고 자료. [Qorvo DWM1000 데이터시트의 SPI 속도 제한](https://store.qorvo.com/datasheets/qorvo/dwm1000datasheet.pdf) · [DW1000 Errata SYSSTAT-1](https://www.qorvo.com/products/r/ra006714) · [Decawave 참조 드라이버](https://github.com/Decawave/dwm1001-examples/blob/b0142ced056296169c7a3f68461b67202aabbbba/deca_driver/deca_device.c#L2447-L2533)

## 증거 파일 SHA-256

- `host/anchor_regression.log` `94070f461e84180b1fd19a8f12e0c06debb5bba1469870c7df6dfbe638736518`
- `host/anchor_driver_regression.log` `7dce32f5174dcdd7d2d2d38faa570240bf639f9e028561885d2d58f06b8a0f3e`
- `host/host_qa_receipt.json` `cbfcfa15e46edfe3bc382a62abb1210a35dbb1f07b7b858ad57032ebbf7d86a2`
- `windows_python.log` `930899421054603697ad289667c146d53837d39e85f3284c660c127b9aabe07d`
- `offline_preflight.json` `28633855cc8335adb7c467f68ffa2f06fb2430cd982206f5eed848db3e99964b`
- `../dist/target_build_receipt.json` `ff081cf487760bbfa1666402facc6815088aaee6c833c3624970db45cb71d3b7`
