# 빌드·업로드·측정 명령

## 0. 현재 제공 상태

ESP32 타깃 컴파일은 실행하지 못했습니다. 컴파일러 미설치 및 공식 파일 다운로드 실패 로그가 `evidence/`에 있습니다. `.bin`/`.elf`가 없으며, `build/host`에서 생성될 수 있는 객체를 보드에 기록하면 안 됩니다.

원본에서 고정한 환경은 Arduino ESP32 Core **3.3.7**, FQBN **esp32:esp32:esp32doit-devkit-v1**입니다. Arduino CLI를 설치하고 PATH에 넣은 PC에서 아래 명령을 실행합니다. 이 패키지는 키가 필요 없는 USB 측정용 분리 펌웨어이며 기존 Wi-Fi/OTA 기능은 포함하지 않습니다.

공식 설치·명령 문서:
- https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
- https://arduino.github.io/arduino-cli/1.3/installation/
- https://arduino.github.io/arduino-cli/1.3/commands/arduino-cli_compile/
- https://arduino.github.io/arduino-cli/1.3/commands/arduino-cli_upload/

## 1. 두 역할 타깃 빌드

압축을 푼 최상위 폴더에서 실행합니다. Python 3.10 이상을 사용합니다.

```powershell
arduino-cli version
python tools/build.py --install
```

이미 정확한 코어가 설치돼 있다면:

```powershell
python tools/build.py
```

래퍼는 `build.ps1`, `build.sh`입니다. `--role anchor` 또는 `--role tag`로 하나만 빌드할 수도 있습니다. 두 역할의 실제 빌드 명령은 다음과 같은 구조입니다.

```text
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 --warnings all --library vendor/anchor/DW1000Ng --build-path build/anchor --output-dir dist/anchor firmware/ANCHOR_DUAL_DS_CIR
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 --warnings all --library vendor/tag/DW1000Ng --build-path build/tag --output-dir dist/tag firmware/TAG_DUAL_DS_CIR
```

**성공한 경우에만** `dist/<role>/`에 실제 app BIN 및 도구가 생성한 bootloader/partition BIN, ELF, MAP가 저장됩니다. 역할별 `compile.log` 및 `target_build_receipt.json`에 종료 코드와 실제 산출물 SHA-256을 기록합니다. `target_build: true`라도 하드웨어 검증이 완료된 것은 아닙니다.

실패하면 로그를 보존하고 성공 상태로 처리하지 않습니다. 현재 제공된 receipt는 실패 상태이며, 스크립트 실행 후 실제 결과로 갱신됩니다.

## 2. 플래시

아래 COM 포트는 예시입니다. 실제 앵커/태그 포트를 구분해야 합니다. 원래 이미지의 백업/복구 수단을 확보하고, 처음에는 운영 보드 대신 별도 테스트 보드에 적용하는 편이 안전합니다. 원본 ZIP만으로는 누락된 등록 키 때문에 기존 운영 이미지를 동일하게 재생성할 수 없습니다.

```powershell
python tools/upload.py --role anchor --port COM5
python tools/upload.py --role tag --port COM6
```

업로드 도구는 성공한 타깃 빌드 receipt와 각 산출물 해시를 검사합니다. 전역 erase 명령은 실행하지 않습니다. 그래도 새 애플리케이션/부트·파티션 관련 영역은 기록될 수 있으므로, 기존 운영 기능이나 자격증명이 계속 사용된다고 가정하지 마십시오. 업로드 종료 코드는 독립적인 플래시 readback 검증과 다릅니다.

## 3. P1–P2 첫 실행

```powershell
python -m pip install -r requirements.txt
python tools/run_pair.py --anchor COM5 --tag COM6 --stage 2 --count 20 --out captures/p2_first
```

실행기는 두 보드에 STOP → HELLO → SELFTEST를 수행하고 하드웨어 상태가 정상일 때만 시작합니다. STAGE 2는 POLL/P1/P2 동시 수신·CIR 수집이며 RESPONSE 송신 및 거리 유효화는 하지 않습니다. 출력은 항상 RFB8이므로 일반 텍스트 시리얼 모니터에서 바이너리가 깨져 보이는 것은 정상입니다. 기존 RFB5 수집기로 읽으면 안 됩니다.

상위 단계:

```powershell
python tools/run_pair.py --anchor COM5 --tag COM6 --stage 3 --count 20 --out captures/p3_responses
python tools/run_pair.py --anchor COM5 --tag COM6 --stage 4 --count 20 --out captures/p4_ds
python tools/run_pair.py --anchor COM5 --tag COM6 --stage 5 --count 20 --out captures/p5_cir
python tools/analyze.py captures/p5_cir/tag.rfb8 --anchor captures/p5_cir/anchor.rfb8 --out captures/p5_cir/derived
```

**R2에서는 `capture_receipt.json`의 `pass=true`가 요청 세션·boot·상대 boot·거래 번호 0..N-1·중복 없음·양쪽 RF 프레임 일치·CIR 슬롯 CRC·RFB8 시퀀스 갭 및 리더 오류 0이라는 조건을 추가로 충족해야 합니다.** 같은 거래의 P1/P2 데이터 충돌이 있는 경우 물리 시간축을 출력하지 않습니다. `capture_receipt.json`의 pass는 패킷·수집 완전성 판정입니다. 거리 정확도, LOS/NLOS 판별 또는 물리 지연축의 참값 일치를 의미하지 않습니다. 실패 시 부분 데이터와 오류를 보존합니다. STOP은 실행 중인 거래가 반환한 뒤 처리됩니다. 즉시 전기적 차단 기능이 아닙니다.

## 4. 개별 명령

ASCII 한 줄 명령을 입력하고 RFB8 응답을 읽습니다. 동일 UART에 두 프로그램을 동시에 연결하지 마십시오.

```text
HELLO
SELFTEST
SET STAGE 2
SET PERIOD_US 300000
SET CIR_HZ 20000000
SET VERIFY_CIR 1
START 12345 20
STOP
```

START의 첫 숫자는 양쪽에 동일한 session ID, 둘째는 앵커 기준 거래 개수입니다. 태그를 먼저 START하고 앵커를 시작합니다. STAGE 1에서는 SELFTEST만 수행하며 START는 거부됩니다. 자동 재시작/무한 재설정은 하지 않습니다. 하드웨어 오류가 래치되면 원인을 확인하고 STOP 후 RESET을 명시적으로 실행합니다.

`ANT <radio> <tx_delay> <rx_delay> <cal_id> <range_sigma_m>`로 별도 보정 결과를 등록할 수 있습니다. 앵커 radio는 0, 태그는 0=A/1=B입니다. 기본 16436/16436은 원본 설정을 가져온 값일 뿐 이 배선에서 검증된 보정값이 아닙니다. 실제 보정 전에는 `cal_id=0`을 유지하십시오. 값은 RAM에만 적용되며 재부팅 후 기본값으로 돌아갑니다.

## 5. P6 주기 변경

기본 921600 baud에서는 300 ms를 유지합니다. RFB8 최대 메타데이터 7,000 B, CIR 15,872 B, 8N1 및 20% 전송 여유를 기준으로 검증하는 정책입니다. 2 Mbps에서는 200 ms, 이후 150 ms가 설정상 허용됩니다. **100 ms는 전체 CIR 출력 예산을 초과하므로 거부됩니다.** 이 수치는 정책상 전송량 검산이며 실제 UART/USB 안정성을 보장하지 않습니다.

```powershell
python tools/run_pair.py --anchor COM5 --tag COM6 --stage 6 --baud 2000000 --period-us 200000 --count 100 --out captures/p6_200ms
```

실행기는 연결 시 보드가 기본 921600 baud라고 가정합니다. 이전 실행에서 2 Mbps로 바꿨다면 재부팅하여 초기 속도로 돌아온 뒤 다시 시작하거나, 별도 시리얼 도구로 속도를 맞춰야 합니다.

## 6. 호스트 검증 재현

GCC/Clang은 ESP32 크로스 컴파일러가 아니라 PC용 컴파일러입니다.

```text
python tools/host_qa.py
python -m unittest discover -s tests -p "test_*.py" -v
python tools/sync_shared.py --check
```

`tests/stubs/`는 구문 검사에만 사용됩니다. 타깃 빌드의 include 경로에 넣으면 안 됩니다. 실제 하드웨어 링크를 확인하지 않은 가짜 Arduino SDK로 사용해서는 안 됩니다.

**주의:** `run_pair.py`는 현재 수집 무결성 감사를 위해 `--count`를 최대 100,000으로 제한하며, 로그 크기 및 PC 메모리의 여유를 충분히 확보해야 합니다. `dist/target_build_receipt.json`이 `target_build=false`이면 업로드 스크립트가 플래시를 거부합니다.
