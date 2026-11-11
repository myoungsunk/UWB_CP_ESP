# 앵커 펌웨어 수정 전 보존본

2026년 11월 11일에 앵커 펌웨어 소스와 앱 복구본을 보존했다. 기준은 `UWB_LAZY_RELEASE_20261107_134517`의 ESP32 앵커 앱이다. 2026년 11월 7일 13:54 KST에 `0x10000`에 기록한 앱을 플래시와 대조해 성공한 기록이 있다. 보존 앱, 당시 배포 기록 및 원본 ZIP 속 앱의 SHA-256이 모두 일치한다. 현재 보드의 플래시는 이번 작업에서 재대조하지 않았다.

| 항목 | 내용 |
| --- | --- |
| 앱 SHA-256 | `9d2c082a72c5808dd179b1dce7a8a75320029c5104186a22cb2e594a573739b9` |
| 앱 크기 | 1,283,328바이트 |
| 빌드 환경 | Arduino ESP32 core `3.3.7`, 보드 `esp32:esp32:esp32doit-devkit-v1` |
| 펌웨어 소스 | `firmware/ANCHOR_RF_PLATFORM_V1/` |
| DW1000Ng | `vendor/DW1000Ng/`에 당시 빌드 명령이 가리킨 라이브러리 복사본 보존 |
| 보호된 복구본 | `recovery/`에 앱 바이너리와 빌드용 프로비저닝 키 헤더 보존 |
| 검증 기록 | `evidence/`의 기존 기록과 `MANIFEST.json`의 파일별 SHA-256 |

`recovery/`의 파일은 이 PC의 현재 Windows 사용자 계정에 묶여 있다. Windows DPAPI `CurrentUser`로 두 파일을 보호하고, 같은 계정에서 복호화한 바이트의 SHA-256이 원본과 일치하는지 확인했다. 앱에는 프로비저닝 키가 내장돼 있다.

기존 앱과 원본 ZIP의 위치는 `MANIFEST.json`의 `original_app`과 `original_archive`에 기록했다. 앱 복구가 필요할 때는 원본 앱의 SHA-256을 확인하고, 확인된 앵커의 파티션 배치를 대조한 뒤 앱 영역만 대상으로 삼는다. 재빌드할 때는 앵커 소스, DW1000Ng 및 기존 키 헤더를 사용한다. 기존 키 헤더의 상대 경로는 원본 패키지의 `source/ANCHOR_RF_PLATFORM_V1/uwb_provision_key.hpp`다.

11월 10일의 `UWB_WIFI_TAG_ANCHOR_COMMAND_20261110`은 오프라인 빌드 검증 기록이 있는 후속 후보이며, 이번 보존본은 11월 7일에 플래시 대조까지 마친 앱을 기준으로 한다. 보존 범위는 앵커 펌웨어와 빌드 입력이다.
