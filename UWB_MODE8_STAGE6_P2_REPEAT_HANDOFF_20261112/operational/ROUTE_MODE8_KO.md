# Mode 8 다중 point 운영 — 안전한 정지 구간 연결

이 구현은 **각 point 최대 32거래의 검증된 무선 측정**을 연결해 긴 경로 데이터를 관리하는 Pi 호스트 스케줄러입니다. **연속 5Hz RF를 끊김 없이 1,000거래 측정하는 코드가 아닙니다.** Wi-Fi 재연결·원본 파일 확인·명시적 PURGE 사이에는 측정 공백이 있습니다.

## 실행
처음에는 \`--manual-confirm\`으로 매 지점의 실제 로봇 위치를 확인하십시오. 스크립트는 **로봇을 구동하지 않습니다**.

\`\`\`bash
export MODE8_AUTH_KEY_HEX='<USB로 프로비저닝한 64자리 키>'
python operational/host/ops_route.py --plan operational/route_example.json --out captures/route123 --anchor-ip 192.168.1.20 --tag-port /dev/serial/by-id/your-device --manual-confirm
\`\`\`

프로그램이 중단된 경우 동일한 plan/out을 사용하고 \`--resume\`을 추가합니다. RF START 요청이 이미 제출됐지만 유효한 앵커/태그 원본이 없는 point는 **자동으로 재측정하지 않고 HOLD**합니다. 운영자가 상태·원본·부팅 ID를 확인한 뒤 별도 세션으로 재측정 여부를 결정해야 합니다.

\`--tag-initial-baud 2000000\`은 태그가 영속 운영 프로파일로 부팅했을 때 사용합니다. 정상 점 종료 후 기존 \`ops_session.py\`는 USB baud를 921600으로 돌리므로 다음 point에서는 필요에 따라 시작 baud를 다시 지정해야 합니다. 재부팅 후 baud가 달라지는 상황은 자동 탐지 없이 오인하지 않도록 의도적으로 명시합니다.

## 완료 기준
한 point의 성공 증거: 유효한 \`operational_receipt.json\` 상태 \`SAVED_BOTH_ENDPOINTS\`, 원본 \`tag.rfb8\`·\`anchor_archive.bin\`, 앵커 boot/config/session/count, P1·P2 양단 감사 모두 성공, SHA-256 포함 호스트 route 저널. 승인된 파일만 PURGE하고 그 명령의 실행 의도를 **미리 fsync**합니다.

**외부 저장/비휘발성 기록장치 없이 32거래 내부에 전원 차단이 발생하면 앵커 RAM 데이터가 손실될 수 있습니다.** 이를 무손실로 바꾸려면 별도 저장 하드웨어 또는 RF와 간섭 없는 검증된 비휘발성 기록 구조가 필요합니다. 손실 데이터는 추정이나 재측정으로 원본인 것처럼 보충하지 않습니다.

## 인수 시험
1. 잘못된 mode/stage/주기, 중복 point ID, 33거래 요청 거부.
2. START 응답 중간 단절 후 journal \`REQUESTED\` 유지 및 RF 자동 재시작 금지.
3. ACKFILE 응답만 유실된 경우 기존 원본과 CRC 재검산 후 복구.
4. PURGE 명령 응답 유실 후 \`PURGE_INTENT\`에서 원격 상태로 복원.
5. 원격 boot/session/count/ACK 상태가 다를 때 다음 point 금지.
6. 실제 로봇 이동·중지·정지 데이터 안정성을 별도 검증하고 Wi-Fi 복귀 시간을 기록.

**삭제 권한**: route 및 단일 point 종료에서 서로 다른 `MODE8_AUTH_KEY_HEX`와 `MODE8_APPROVAL_KEY_HEX`가 필요합니다. 승인 키는 USB의 `OPS DELETEKEY`로만 등록하고, 일반 HMAC 로그인 키만으로는 ACKFILE/PURGE가 실패해야 합니다.
