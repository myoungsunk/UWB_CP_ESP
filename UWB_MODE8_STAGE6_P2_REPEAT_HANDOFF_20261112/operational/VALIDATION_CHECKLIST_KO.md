# Mode 8 운영 통합 검증 계약

**판정 단위**: host unit / ESP32 target compile / 실제 anchor-tag / WLAN+Pi / 실사용 운용. 서로 대체하지 않는다. 기록에는 Git SHA, firmware BIN SHA, boot/session/config, 보드·배선·파티션, 각 실행의 \`rfb8\`, \`uart\`, \`archive\`, 결과 receipt를 남긴다.

## A. 코드 및 데이터 계약 (자동)

| ID | 시험 방법 | 통과 기준 |
|---|---|---|
| A01 | 기준 \`ce8f986\` 소스와 신규 브랜치 비교 | 양쪽 Stage 6 RF 계약 14와 대응 Pi 도구 및 보존된 핀맵 확인 |
| A02 | \`ops_archive.py\` 정상·변조·중복·길이·부팅/세션 불일치 | 비트 변조 후 바깥 CRC만 재계산해도 fail-closed |
| A03 | TCP challenge/HMAC 테스트, 잘못된 인증 | 비인증 명령이 실행되지 않음 |
| A04 | TCP GET 중 단절 | 최종 파일 승격·ACKFILE 전송 금지, partial 분리 |
| A05 | 앵커·태그의 \`shared/*.hpp\`와 sketch 복사본 대조 | 무결성 PASS |
| A06 | \`REC_ACK/NACK\` 지연·중복·다른 세션·부팅 ID | 잘못된 record release 0 |
| A07 | \`STOP→SET\`, \`STOP→START\` 사이 writer·ACK 남김 | \`RECORD_DRAIN_PENDING\` 또는 명시적인 보류, 구형 기록 오염 0 |
| A08 | archive 최대 32개와 초과 요청·이전 archive 존재 | 초과·덮어쓰기 거부 |
| A09 | 기존 P2_REPEAT·RXRFSL·SPI 코드 회귀 | 정상/오류 판정 동일 |
| A10 | 호스트 ASan·UBSan, C++ portable 및 Python | 실행한 시험 전부 통과 |
| A11 | ESP32 Core **3.3.7**, FQBN 고정 빌드 | anchor/tag BIN/ELF/MAP 생성, source SHA–image SHA 연결 |
| A12 | Build failure·source mismatch 상태에서 플래시 도구 | 업로드 차단, 사용 가능하다고 표시하지 않음 |

## B. 장치 및 전원

| ID | 시험 | 합격 기준 |
|---|---|---|
| B01 | Pin map 및 회로 측정 | tag A(21/22/17), B(25/26/27), SPI18/19/23, 스위치 GPIO4/5 미사용 |
| B02 | DEV_ID/SYS_TIME/RXENAB 및 ANT 읽기 | 수신 후에도 두 모듈의 DEV_ID=0xDECA0130, 시간 증가, TX/RX delay 정상 |
| B03 | 단일/이중 RX, 992 complex CIR 반복읽기 | 다음 RX 시작 전에 양쪽 스냅샷 보존, CRC 일치 |
| B04 | 32거래 운영형에서 최대 free heap 및 최대 스택·태스크 상태 | 여유 부족 시 측정 금지, WDT/brownout 없음 |
| B05 | 100회 이상 부팅→SELFTEST→Wi-Fi→OFF→측정→ON | PLL 시작 오류 원인·발생률과 복구 상태 기록, 무한 재시작 없음 |
| B06 | 정전·브라운아웃 순간을 준비/측정/commit/OTA로 주입 | 미완료 기록을 완료로 오승인하지 않음 |

## C. RF 및 타이밍

| ID | 시험 | 합격 기준 |
|---|---|---|
| C01 | Stage6 200ms 정적 1,000거래, 기존 USB 기준선 | 독립 관측·CIR·P1/P2 completeness 및 시간 주기 대비 |
| C02 | 32거래 운영형에서 Wi-Fi OFF | 실제 RF 구간 Wi-Fi OFF, POLL cadence 오차, TX scheduling 달성 |
| C03 | P1 RESPONSE 또는 Probe 누락 | 기존 재시도/무효화 동일, attempt 혼용 없음 |
| C04 | P2 최초 누락+repeat 성공 | A/B가 같은 반복 Probe, 원본 최초 실패 저장 |
| C05 | P2 최초·repeat 모두 누락 | 과거 타임스탬프 혼합 없이 fail-closed |
| C06 | 200→195→190 ms 설정 시험 | 실제 기간·큐·주기 초과·TX deadline을 각각 측정; default 변경 전 승인 |
| C07 | 0.5/1/3/5 m 여러 거리·yaw·편파 교차 | 독립 DS-TWR bias/RMSE, 안테나 보정, FP 오차, NLoS 분리 |
| C08 | 로봇 0.2m/s 이동·회전 | 실제 선택된 TX/RX 시각으로 odom/IMU 연결, 반복 P2 시각 표시 |

## D. TCP/USB/저장/OTA 운영 기능

| ID | 주입 상황 | 통과 기준 |
|---|---|---|
| D01 | 암호 인증 성공/실패·이전 nonce replay | 인증 실패 단절, 재연결 새 nonce |
| D02 | 잘린 TCP 명령·긴 명령·여러 명령 한 write | 명령 1회 처리/초과 거부, RF 소유권 불변 |
| D03 | 태그 시작 전에 anchor START 시도 | 호스트 사전조율 실패 처리, 운영 성공으로 표시하지 않음 |
| D04 | 측정 중 Wi-Fi AP 재부팅/외부 TCP 명령 | RF 중 Wi-Fi OFF, 새 제어 명령 영향 없음 |
| D05 | tag USB 저장 중단, ACK 지연·분실 | 원본 보존/재전송 또는 명시적 실패, 새 RF 재측정 아님 |
| D06 | 앵커 파일 commit 중 재부팅 | 유효한 임시 파일만 확정, 손상 파일 보존 |
| D06a | 동일 세션의 새 파일, ACK 응답 유실 | 부팅 ID·CRC가 다른 파일 삭제 거부, 동일 파일 재승인 가능 |
| D06b | Wi-Fi OFF 실패 또는 RF 시작 직전 Wi-Fi 복귀 | RF START 차단, 실패 상태 표시 |
| D06c | CRC가 맞는 아카이브에서 TX 시각 또는 부팅 ID 변조 | Pi 양단 대조 실패, ACKFILE 차단 |
| D06d | 단독 GET에 --ack 또는 get_archive(ack=True) 사용 | 페어 감사 전 승인 경로 거부 |
| D06e | 이전 boot/session/config의 active HELLO 삽입 | 태그 START 오승인 0 |
| D07 | GET 파일 중간 단절과 2차 다운로드 | 원본 CRC·파일 SHA 동일, 받기 전 ACK 금지 |
| D08 | 저장되지 않은 파일의 ACKFILE/PURGE | 거부. 성공 다운로드 후 명시적 PURGE만 가능 |
| D09 | 보존 파일이 있는 상태 새 START/OTA | 거부 |
| D10 | OTA 창 60~600초, 비인증·측정 중 요청 | 거부/허용 조건 검증 |
| D11 | 파일시스템 OTA 및 이미지 오류·네트워크 단절 | 기존 데이터 보호, 실패 후 정상 실행 경로 검증 |
| D12 | OTA 뒤 부팅 실패·타 역할 이미지 | bootloader rollback/이미지 역할 검증은 **별도 설계 완료 전 미인수** |
| D13 | TCP 패킷 스니핑·로컬 네트워크 도청 | TLS 미도입 위험 명시. 외부 노출 금지 |
| D14 | 구형 R03 모드·UWB point/route 복원 요구 | 신규 Mode8 단일 point와 구형 스키마를 혼동하지 않음 |

## E. 판정 및 중단 조건

**실기 중지**: CS 충돌, 보드 공급전압 강하, PLL/DEV_ID 무결성 오류, 원시 RF 프레임 증거 충돌, 거리시간 계산 오류, archive CRC 실패, 기록이 원래 세션과 섞임, OTA 시 미회수 데이터 손상.

**인수 완료 조건**: A01–A12의 실제 로그, B의 전원·메모리, C의 고정·이동 중 RF 실측, D의 TCP·저장·OTA fault injection을 모두 수행하고, 실행 조건·실패 재현·후속 재감사 근거를 커밋과 연결한다. 현재 소스 구축과 GitHub CI 통과만으로 운영 승인하지 않는다.
