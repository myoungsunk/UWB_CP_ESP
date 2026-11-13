# Mode 8 Dual-DW1000 운영 계층 통합 — 개발 후보

**기준** 2026-11-13 Stage 6 P1 반복 및 종료 정리 재감사 소스. 기존 저장소 경로를 사용하며 운영 앵커와 태그는 Stage 6 RF 계약 14를 사용한다.

**적용 범위**: ESP32 앵커의 Wi-Fi/TCP 운영 계층, 단일 정지 point의 RF-offline 수집·로컬 증거 보존·TCP 회수, Raspberry Pi의 태그 USB 수집기. P1·P2 반복, 원본 CIR, 기존 태그 핀맵 및 대응하는 Pi 수집기를 함께 사용한다.

**인수 상태**: 실기 검증용 플래시는 앵커·태그 ESP32 Core 3.3.7 타깃 빌드와 산출물 검산 후에 진행한다. 운영 인수에는 순차 실기 검증이 필요하다. 이 문서는 구현·시험 계약이지 실물 통과 영수증이 아니다.

## 1. 구성

- \`firmware/anchor_p2_repeat/shared/operational_anchor.hpp\`: 유휴 TCP 서버(29310), HMAC-SHA256 인증, Wi-Fi OFF 측정, 32거래 고정 크기 RAM 증거 수집, 측정 종료 후 LittleFS 검산·TCP 파일 회수
- \`firmware/{anchor_p2_repeat,tag_p2_repeat}/shared/app.hpp\`: STOP/ACK 기록 정리 게이트. 앵커는 운영형에서 UART RFB8 대신 RAM archive를 저장하는 한정된 경로 제공
- \`operational/host/ops_archive.py\`: 파일/행/air frame CRC, 길이, 세션·부팅·거래 순서 검증
- \`operational/host/ops_client.py\`: 원격 챌린지 HMAC, GET 임시 파일/fsync/CRC 검증. 단독 GET의 ACK 금지. 태그·앵커 양단 검증 후 \`ops_session.py\`만 ACKFILE 전송
- \`operational/host/ops_session.py\`: Pi 태그 원시 RFB8/CRC/ACK 수집, TCP 앵커 START→Wi-Fi OFF, 재연결→앵커 파일 회수/양단 ID 검증
- \`.github/workflows/mode8-operational.yml\`: Python 적대적 테스트, 기존 portable Mode 8 회귀, 앵커·태그 실제 ESP32 Core 3.3.7 빌드

### 명시적인 제약

- **기존 Mode 3–7과 기존 R03 route/point 저장 스키마를 복원한 것이 아니다.** Mode 8의 새로운 단일 point 세션이며, 운용상 기존 기능과 교차 사용할 수 없다.
- **한 세션 최대 32거래**: RAM 증거 보관 후 Wi-Fi 재연결·LittleFS 기록. 현재 방식은 1,000거래 5 Hz 연속 측정용이 아니다. tag-only USB 측정은 기존 시험용 스택 사용
- 앵커 보관 파일은 **Mode8 anchor compact evidence** 형식이며 RFB8 파일과 다르다. 태그 전체 CIR은 Pi \`tag.rfb8\`에 보존한다
- 측정 직전 반드시 태그를 Pi에서 먼저 START해야 한다. 앵커 TCP START는 인증된 호스트에서 시작하고 250ms 뒤 로컬 Stage 6 엔진을 실행한다
- Wi-Fi는 RF 측정 중 **OFF**. 측정이 종료되고 저장된 뒤 재연결한다. 이 구성에서는 측정 중 TCP 명령을 보내는 운용은 지원하지 않는다
- 앵커/태그의 기본 안테나 지연값은 미보정. \`math_valid\`는 거리 정확도·NLoS 또는 위상 동기를 보증하지 않는다
- OTA 정비 요청은 장치에서 이미지 역할과 SHA-256을 검증할 수 있을 때까지 닫힌 상태를 유지한다. USB는 `IMAGE_ATTESTATION_REQUIRED`, 인증된 TCP는 `OTA_IMAGE_ATTESTATION_REQUIRED`를 반환한다. 첫 부팅 검사는 기존 동작을 유지한다. 실제 rollback 인수는 G10 대상이다.
- TCP는 HMAC challenge 인증만 적용. **TLS가 아니므로 동일 LAN의 도청·트래픽 분석을 막지 못한다.** 외부 인터넷에 TCP29310/UDP3232를 노출하지 않는다
- Wi-Fi 자격증명은 NVS Preferences에 저장하므로, 플래시 암호화가 없는 보드에서 추출 가능성을 고려해야 한다

### Mode 8 영속 운영 프로파일 (신규)

앵커·태그는 최초 설정 시 USB에서 `SET STAGE 6`, `SET PERIOD_US 200000`, `BAUD 2000000` 이후 `PROFILE SAVE`를 실행해야 합니다. 다음 부팅부터 CRC와 역할을 확인한 프로파일만 복원합니다. 손상·미설정이면 기존 안전 진단 기본값(stage5 / 300ms / 921600baud)을 사용하고 **자동 측정 시작은 하지 않습니다**. `PROFILE STATUS`로 상태, `PROFILE CLEAR`로 저장 설정만 삭제합니다. 프로파일은 안테나 지연 보정 값을 저장하거나 보정 완료로 표시하지 않습니다.

Pi `ops_session.py`에서 태그가 프로파일로 2Mbps 부팅한다면 `--tag-initial-baud 2000000`을 사용하십시오. 미설정 태그에는 기본 921600을 유지합니다.

## 2. 처음 실행 준비

ESP32 DevKit V1 및 현장 배선 확인 후, 복원 가능한 기존 펌웨어 이미지를 보존한다.

1. ESP32 타깃 빌드와 SHA 영수증을 확인한다. 통과 전 플래시 금지
2. 앵커는 USB Serial로 \`STOP\`, \`SET STAGE 6\`, \`BAUD 2000000\`, \`SET PERIOD_US 200000\` 수행
3. USB 콘솔에서만 WPA 개인망 자격증명과 별개의 접속·파일 승인 HMAC 키를 등록. 비밀값을 공개 저장소·로그에 남기지 않는다
4. USB \`OPS NET ON\` 이후 앵커 29310/TCP의 challenge-HMAC 상태 조회를 수행
5. 태그 Pi USB 연결 \`ops_session.py\` 실행. 태그는 Stage 6·2Mbps·200ms로 설정하고 먼저 시작한다. 앵커는 TCP START 확인 직후 Wi-Fi를 끄고 측정한다
6. 끝나면 앵커 \`m8_pending.bin\`이 검산된 뒤 Wi-Fi 재연결. Pi는 \`anchor_archive.bin\`을 받고 원시 \`tag.rfb8\`와 세션·부팅·설정을 교차 대조 후 ACKFILE 전송
7. HOST가 실제 분석·백업을 마친 후 명시적 \`PURGE\`; 파일 삭제는 자동 실행하지 않는다

**삭제 승인 권한 분리**: USB에서 `OPS DELETEKEY <64-HEX>`를 별도로 지정하고 Pi의 `MODE8_APPROVAL_KEY_HEX` 환경변수를 설정합니다. 이 키는 `OPS AUTH`와 달라야 합니다. 파일 승인/삭제는 접속별 nonce + boot + session + CRC + ACK/PURGE 동작에 대한 별도 HMAC를 요구합니다. 외부 LAN에는 포트를 공개하지 마십시오. 분리 키가 없는 단독 GET은 보관 파일을 내려받을 수 있지만 삭제 권한은 없습니다.

**USB OPS 명령**: \`OPS STATUS\`, \`OPS SSID <UTF8-HEX>\`, \`OPS PASS <UTF8-HEX>\`, \`OPS AUTH <64-HEX-KEY>\`, \`OPS OTAHASH <64-HEX-SHA256>\`, \`OPS NET ON\`, \`OPS NET OFF\`, \`OPS OTA 60\`.

\`OPS SSID/PASS/AUTH/OTAHASH\` 값은 출력·ACK에 재표시하지 않는다. HMAC 키는 암호학적 난수로 32바이트 생성하고 호스트 환경변수 \`MODE8_AUTH_KEY_HEX\`에 설정한다.

**호스트 사용 예**:

\`\`\`bash
pip install pyserial numpy
export MODE8_AUTH_KEY_HEX='<USB에서 등록한 접속용 64자리 hex 키>'
export MODE8_APPROVAL_KEY_HEX='<USB에서 등록한 별도 파일 승인용 64자리 hex 키>'
python operational/host/ops_client.py --host <anchor-ip> status
python operational/host/ops_session.py --anchor-ip <anchor-ip> --tag-port /dev/serial/by-id/<tag-port> --session 12345 --count 20 --out captures/point_12345
python operational/host/ops_client.py --host <anchor-ip> purge \
  --archive captures/point_12345/anchor_archive.bin \
  --tag captures/point_12345/tag.rfb8 \
  --receipt captures/point_12345/operational_receipt.json
\`\`\`

작업 경로는 패키지 \`operational/\`의 상위 디렉터리를 기준으로 한다. Windows도 같은 CLI를 사용하며 앵커 TCP 접속 호스트와 Pi 태그 시리얼 호스트가 같아야 한다.

### 복구 및 오류 정책

- 임시 파일을 검산해 완전하면 확정한다. 손상 시 `RECOVERY_REQUIRED`를 표시한다. USB `OPS RECOVER`는 검산을 재시도하고, `OPS QUARANTINE`은 손상된 임시 파일을 별도 경로에 보존한다. 덮어쓰기와 자동 포맷은 금지한다.
- 기존 v1 파일은 GET과 수동 검증이 가능하다. 새 v2 파일은 최초·반복 P1/P2 TX와 프레임을 보관한다. 이전 세션 단독 삭제 승인은 폐기한다.
- TCP 시작 응답은 예약 수락을 뜻한다. RF 시작 직전 Wi-Fi OFF를 다시 확인한다. 응답이 끊기면 동일 부팅 ID와 세션의 결과로 시작 여부를 확인한다.
- 현재 브랜치의 대상 이미지는 새 CI 또는 로컬 빌드 산출물이다. 저장소의 기존 dist 바이너리는 이전 소스에서 생성됐다.
- USB·TCP 측정 기록이 손상되면 동일 원본 record를 재회수하며 RF 재측정으로 치환하지 않는다
- 앵커는 32개 이하 구간 동안 RAM에 compact 정보를 보존한 뒤 종료 시 플래시 커밋한다. 측정 중 전원 차단으로 RAM 증거가 사라지는 위험이 있다. 완전 무손실 전원 차단 대응에는 외부 비휘발성 저장·저장 트랜잭션 재설계가 필요하다
- 이전 버전의 파일 승인만 가진 앵커 파일은 그대로 보존되며, 삭제하려면 양단 원본을 회수하고 새 승인 형식으로 다시 검증해야 한다.
- 단독 \`ops_client.py get\`은 파일만 내려받으며 삭제 승인하지 않는다. 과거 \`--ack\` 옵션과 Python \`get_archive(ack=True)\`는 거부한다. ACKFILE은 신뢰된 호스트의 검증된 \`ops_session.py\` 경로에서만 자동 호출해야 한다. HMAC 키를 가진 다른 클라이언트의 직접 명령은 서버가 구분할 수 없으므로 폐쇄 LAN과 키 보호를 전제로 한다.
- Pi는 요청 ID가 일치하는 STOPPED·DRAINED와 baud 복귀 응답이 저장된 원시 RFB8에 포함됐는지 확인한다. 양단 부팅 ID, 실제 송수신 시각, P1·P2 선택, CIR 무결성, 공통축·전력 그리드와 주기를 검산한 뒤 GET 연결에서 파일 CRC와 세션을 ACKFILE에 보낸다. 삭제 승인은 부팅 ID·세션·CRC에 묶는다. USB PURGE는 거부한다. 앵커가 재부팅된 이전 부팅의 파일은 GET으로 보존하고 새 삭제 승인은 보류한다. 단독 PURGE와 route는 삭제 직전 두 원본을 다시 확인한다.

## 최신 적대적 감사

- [96d8430 기준 적대적 재감사·수정·인수 보고](ADVERSARIAL_REAUDIT_96D8430_20261113_KO.md): 독립 검증 범위를 넘는 주장은 제외하고 코드·CI·실기 인수를 분리한다.

## 3. 인수 테스트

\`VALIDATION_CHECKLIST_KO.md\`의 A(소스) → B(적대적) → C(타깃 빌드) → D(현장 하드웨어) → E(운영 계층) 순서로 검증한다. 실제 컴파일·실기 결과는 CI run ID와 별도 UART/RFB8 원본 해시로 기록한다.

### 유선 1,000거래 우회 운용 및 Wi-Fi 부팅 정책

원격 Wi-Fi OFF 앵커는 한 point당 최대 32거래이며, 다음 구간 시작 전 저장·Wi-Fi 재연결 공백이 있습니다. 두 보드(앵커와 태그)를 **같은 USB 호스트에 직접 연결할 수 있다면** `ops_wired_continuous.py`를 사용한 기존 Stage6 200ms·2Mbps 장기 연속 캡처 경로가 별도로 있습니다.

앵커에 Wi-Fi 자격증명이 남은 상태로 USB 수집기를 재연결하면 보드가 재부팅되며 Wi-Fi가 자동으로 켜질 수 있습니다. 이를 막기 위해 **앵커 USB 정비 포트에서 `OPS NET AUTOSTART 0`을 먼저 수행**하십시오. 이 설정은 NVS에 보존되고 재부팅 이후에도 Wi-Fi 자동 재연결을 막습니다. 다시 원격 운영으로 복귀할 때는 USB에서 `OPS NET AUTOSTART 1`을 실행합니다. 기본값은 기존처럼 자동 재연결 허용입니다. 이는 실제 Wi-Fi RF 간섭/전원 안정성의 물리 검증을 대체하지 않습니다.

```bash
# 첫 명령은 실행 계획만 출력하고 RF를 송신하지 않음
python operational/host/ops_wired_continuous.py --anchor COM4 --tag COM7 --count 1000 --out captures/wired_1000
# 두 보드 전원/USB/앵커 Wi-Fi OFF를 실물에서 확인한 후에만 실행
python operational/host/ops_wired_continuous.py --anchor COM4 --tag COM7 --count 1000 --out captures/wired_1000 --execute --wifi-off-confirmed
```

`PROFILE SAVE`로 2Mbps 부팅이 설정돼 있다면 `--anchor-initial-baud 2000000` 또는 `--tag-initial-baud 2000000`을 추가해야 합니다. `ops_wired_continuous.py`의 기본 921600을 무조건 신뢰하지 마십시오. RF START 실패 시 자동으로 재측정하지 않고 원본·진단 로그를 보존합니다.
