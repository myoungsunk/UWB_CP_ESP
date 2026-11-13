# MODE8-ACCEPTANCE-001 — 순차적 검증 사양서

대상 Git 브랜치 \`codex/mode8-operational-integration-20261113\`.
**각 단계는 전 단계 증거가 있고 차단 조건이 없을 때만 진행합니다.** 각 시험은 firmware BIN SHA, Git SHA, boot/session/config, 설치 핀맵, 실행 원본 (\`.uart/.rfb8/anchor_archive.bin\`)과 receipt를 남깁니다.
**기준**: Stage6 200ms·2Mbps·Dual-DW1000, P1/P2 독립 DS-TWR, A/B CIR. Mode3–7과 절대 거리 보정은 승인 대상 아님.

| Gate | 유형 및 시험 | 검증 방법 | 필수 합격/차단 조건 |
|---|---|---|---|
| G0 소스 | 모듈/스케치 사본, Profile CRC/role, 프로토콜 동일성 | Python 검사 + 소스 SHA | 불일치 0, 미검증 BIN 사용 금지 |
| G1 오프라인 | PROFILE SAVE/LOAD/CLEAR, 잘못된 CRC/role, 설정 변경/ACK 경쟁 | ESP32 함수 Mock·portable 테스트 | 손상 시 921600 진단, 자동 RF START 없음, 이전 세션 혼합 0 |
| G2 호스트 | route 계획 1~10000pts/point1~32, 중복/음수/초과/경로 주입 | mock 서브프로세스, crash/restart | 잘못된 계획 거부, 세션 중복 0, 미완료 START 재실행 0 |
| G3 저장 승인 | 파일 CRC 정상이나 radio/boot/nonce 불일치, GET 절단, ACK/PURGE replay | 앵커 Mock + Pi unittest | 검증 전 삭제 0, 유효 증거 불변성, CRC mismatch 반드시 HOLD |
| G4 빌드 | 앵커/태그 Core3.3.7, exact board FQBN | GitHub Actions target build, size, source hashes | 2역할 BIN/ELF 생성, 실제 빌드 영수증, 컴파일 오류 0 |
| G5 장치 | 태그 A/B CS/IRQ/RSTn, RF 스위치 제거, 전원 | DEV_ID 0xDECA0130, SYS_TIME, RXENAB, SPI 20MHz | 수신 복구 시 오동작·CS 중첩·brownout 없는 장치 |
| G6 RF | 정지 32거래 x ≥10 runs | 같은 Probe 양쪽 CIR/DS, on-air frame CRC, RF 시각 직접 비교 | 각 실행 P1/P2 공통축 요구수 충족, period ±4ms, 누락/재시도 원인 분리 |
| G7 운영 | 단일 point TCP start→Wi-Fi OFF→RF→commit→ON→GET/ACKFILE/PURGE | AP, Pi, Wi-Fi 스니핑, USB 원시 로그 | RF 시간 동안 Wi-Fi OFF, 양단 ID/원본 CRC 검증, 임의 삭제 0 |
| G8 다중 point | 32+32+20, 10×32, route resume | Pi route journal, 전원/USB/TCP 차단 | 각 point 원본 해시 재생, 불완전 점 자동 재측정 없음, RF 공백 별도 보고 |
| G9 정전 | Armed/Measuring/Draining/Flash-temp/rename/ACK/OTA 6개 절단점 | 하드웨어 릴레이 전원차단, 복원 | 미커밋 RAM 손실은 명시 HOLD, 기존 원본 덮어쓰기 0, 손상 파일 오승인 0 |
| G10 OTA | 잘못된 역할 이미지, wrong sha, U_SPIFFS 요청, 중단 업데이트, rollback | 60–600초 정비창, 역할별 파티션/부트 확인 | 근거 없는 OTA 안전/롤백 성공 주장 금지, 기존 파일 불변 |
| G11 장기 | 1h/24h 반복 운영, 인증/PLL/heap/stack, USB 조각 결손 | 고장 주입·스코프·heap watermark | 데이터 누락이 원인별로 기록, 복구 후 원본 byte 동일, heap 저수위 문턱 지킴 |
| G12 이동 | 0.2m/s 로봇 및 회전, FP·NLoS | 로봇 GT/odom/IMU 실제 시간축 비교 | 반복 P2는 반복 시각 사용, 통신 손실·물리 거리 보정 별도 보고 |

## 적대적 반례 카탈로그

| ID | 입력/실패 주입 | 기대 결과 |
|---|---|---|
| AX01 | 프로파일 role 바꾸고 CRC 재계산 | 다른 하드웨어 역할에서 로드 거부 |
| AX02 | 프로파일 CRC 손상, 절단 NVS | 진단 초기값, RF 자동 시작 금지 |
| AX03 | Tag 과거 HELLO(active=true), 다른 boot/session/config | START 확정 거부 |
| AX04 | 최초 POLL 누락·P1/P2 충돌·A/B 혼합 반복 | 공통 물리축 무효, 원시 IQ 보존 |
| AX05 | 5개 tag record 모두 ACK 지연·호스트 2초 정지 | Backpressure/실패를 정확히 기록, 이전 세션 기록 해제 금지 |
| AX06 | GET 수신 95% 뒤 TCP 단절 | \`.partial\` 보존, ACKFILE 금지 |
| AX07 | 승인 없이 PURGE; 다른 파일 CRC·boot 재사용 | 미승인 원본 삭제 거부 |
| AX08 | route point START 응답 분실 → 프로세스 재시작 | 같은 session 자동 재측정 금지 |
| AX09 | PURGE 응답 분실 → 재시작 | 저장된 PURGE_INTENT 상태와 원격 상태만으로 복구 |
| AX10 | OPS START 시 WIFI_OFF 실패, 예약 START USB STOP | RF 송신 0, RAM 예약 해제 |
| AX11 | Flash 쓰기 직전/rename 도중 전원 차단 | 성공하지 않은 기록을 완료로 오인하지 않음 |
| AX12 | OTA에 잘못된 역할·서명·CRC | 지원되는 검증이 없으면 FAIL-CLOSED, 정상 이미지 유지 |
| AX13 | RF 중 Wi-Fi/EAP/OTA/flash 변동 | RF 소유권 침범 0, 200ms deadline 기록 |
| AX14 | 경로 파일 조작/중복 point ID/33개/Mode7 | 클라이언트가 RF 요청 전 거부 |

## Gate 인수 영수증 스키마

\`\`\`json
{
 "git_sha":"<exact sha>",
 "anchor_bin_sha256":"<exact actual flashed image>",
 "tag_bin_sha256":"<exact actual flashed image>",
 "tests":[{"id":"G7","attempted":true,"pass":false,
           "stop_reason":"<observed defect or unfulfilled prerequisite>",
           "evidence":["<path to immutable raw log>"]}],
 "hardware_field_approved":false,
 "absolute_accuracy_validated":false
}
\`\`\`

누락, 오류 후 복구, 정상 최초 수신을 따로 집계하고 **최초 RF 성공률**과 **복구 후 데이터 보존율**을 구분합니다. 모든 시험에 실측 전원/네트워크/보드 정보가 없으면 해당 Gate는 NOT_RUN이며 합격으로 기입하지 않습니다.

## 실제 실행 명령(운영 후보)

\`\`\`bash
python -m unittest discover -s operational/tests -p 'test_*.py' -v
python operational/host/ops_route.py --plan operational/route_example.json --out captures/route01 --anchor-ip <local-anchor-ip> --tag-port /dev/serial/by-id/<tag> --manual-confirm
\`\`\`

**중단 조건**: CRC 정상으로 위장한 가짜 물리축 통과, 다른 세션 삭제, boot/role 어긋남, CLKPLL 오류 후 RF 강행, 자동 포맷, 원본 덮어쓰기, OTA 기록 파손.

## G8 추가 적대적 검증 — 재개 시점 원본 변조

- **AX15**: point가 `REQUESTED` 또는 `VERIFIED`인 상태에서 `operational_receipt.json`의 paired_audit는 PASS로 남기고 `tag.rfb8` 바이트를 변조한다. `ops_route.py`의 `verify_point()`는 반드시 원시 RFB8/앵커 아카이브를 재생해 실패하고, 후속 PURGE 및 새 RF START가 0회이어야 한다.
- **AX16**: 최초 검증 후 `anchor_archive.bin`, `tag.rfb8` 또는 영수증을 교체한다. journal에 기록된 SHA-256과 새로 계산한 해시가 하나라도 다르면 `RouteHold`로 중단한다.
- **AX17**: `PURGE_INTENT` 이후 재시작하면서 저장된 증거가 삭제되거나 손상됐다. 원격 앵커가 파일 없음만 반환해도 로컬 원본 재감사 없이 `PURGED`로 확정하지 않는다.

## G0–G12 원본 증거 자동 확인 CLI

`operational/host/ops_acceptance_gate.py`는 단계별 증거 파일의 SHA-256,
Git SHA, 앵커·태그 실제 flashed BIN SHA와 단계의 선행조건을 검사합니다.
실물 장치의 계측 사실 자체를 인증하지 않으므로 **검증된 증거가 모두 있어도
`hardware_field_approved=false`**로 기록합니다. 별도 담당자의 실기 검사·인수 서명은
기존 검증표에 따라 수행해야 합니다.

```bash
python operational/host/ops_acceptance_gate.py \
  --manifest captures/acceptance_manifest.json \
  --evidence-root captures \
  --expected-git-sha <40-character-actual-source-sha> \
  --out captures/acceptance_gate_result.json
```

manifest `tests` 배열은 G0..G12 중 진행된 각 단계에
`{"id":"G7","attempted":true,"pass":true,"evidence":[{"path":"raw/g7.uart","sha256":"<real 64-hex hash>"}]}`
형식을 사용합니다. 존재하지 않는 로그·다른 파일 해시·선행단계 실패 후 PASS 선언은
`FIELD_HOLD` 처리합니다. 개별 `pass` 표기는 실제 시험 원본을 대신하지 않습니다.
