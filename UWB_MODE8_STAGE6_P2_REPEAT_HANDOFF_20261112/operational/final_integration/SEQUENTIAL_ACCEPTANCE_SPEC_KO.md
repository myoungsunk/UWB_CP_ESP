# G0–G12 순차 인수

기존 [검증 사양서](../MODE8_SEQUENTIAL_VERIFICATION_SPEC_KO.md)의 기준을 적용한다. 외부 계측은 제외한다. 보드 자체 진단, 유선 거래, 실제 운영 애플리케이션을 사용한다.

| Gate | 실행과 증거 |
|---|---|
| G0–G4 | 소스 매니페스트 대조, Python/ASan/UBSan, 정확한 Core 3.3.7로 앵커·태그 컴파일 및 해시 기록 |
| G5 | 이미지 역할 및 플래시 읽기 대조, HELLO·SELFTEST, A/B DEV_ID 및 PLL 보정 기록 |
| G6 | Stage 6·200 ms·2 Mbps 유선 RF 32거래, 양쪽 원본·P1/P2 축·전력·STOP/DRAIN 확인 |
| G7 | Pi 태그·앵커 운영 앱 32거래, Wi-Fi OFF 상태와 LittleFS 원본 GET·검산·ACKFILE_OK 기록 |
| G8 | 10지점 × 32거래, 양쪽 RF 원본 320/320, 지점별 ACKFILE_OK 10/10, 지연 분포와 실패 로그 기록 |
| G9 | 보류. 재개 시 승인된 테스트 데이터와 전원 차단 절차로 tmp/commit 복구를 확인 |
| G10 | 배포용 이미지 서명·공개키·롤백 파티션 준비 후 정상 OTA와 오류·첫 부팅 실패 주입 |
| G11 | 200 ms·2 Mbps 유선 1시간 통과 후 24시간. 미복구 누락 0, RFB8 CRC 0, 종료·복원 확인 |
| G12 | 장비 정지 상태에서 RF·odom·IMU와 USB 시각 마커 동시 수집. Pi ROS2 시스템 시계 확인 후 A/B 모델과 유효 구간 검증 |

유선 명령은 `python operational/host/ops_wired_continuous.py --help`에서 포트와 새 출력 폴더를 지정한다. G12 운영 세션에는 `python operational/host/ops_session.py … --usb-clock-markers`를 사용한다. A/B 모델 검증은 `python operational/host/ops_usb_clock.py CAPTURE/sync_markers.jsonl --boot BOOT --session SESSION`으로 실행한다. 매핑은 마커로 관측한 시간 범위와 계산된 USB 오차를 갖는다. 보드 플래시 및 실제 주행은 별도로 승인된 실행에 속한다.
