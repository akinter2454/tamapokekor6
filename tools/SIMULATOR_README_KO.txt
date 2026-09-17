TamaPoke PC UI Simulator v1
기준 펌웨어: TamaPoke KO v3.55

실행 방법
1. tools/TamaPoke-PC-Simulator.html 파일을 더블클릭합니다.
2. Chrome / Edge 등의 브라우저에서 바로 실행됩니다.
3. 별도 서버, Python, Netlify, GitHub Pages가 필요하지 않습니다.

주요 기능
- 466x466 원형 화면 미리보기
- 메인 / 육성아이템 / 기술머신 / 시계 / 체육관 / 방어 훈련 화면
- 포켓몬 이름, 레벨, 타입, 낮/밤, 날씨 변경
- 타입별 절차형 배경 및 날씨 효과 미리보기
- 이름 라벨 Y 위치 및 이름/레벨 텍스트 위치 실시간 조정
- 조정된 HEADER_NAME_Y_NUDGE / HEADER_TEXT_LIFT 값을 바로 확인 및 복사
- 좌표 그리드와 터치 좌표 확인
- 방어 게이지 PERFECT / GOOD / BLOCK / MISS 모의 테스트
- 현재 화면 PNG 저장

주의
- 이 파일은 UI 개발용 시뮬레이터입니다.
- 실제 ESP32-S3의 처리 속도, AMOLED 색감, CST9217 터치, ES8311 오디오,
  NVS 저장, LAN 통신을 완전히 에뮬레이션하지는 않습니다.
- 특히 한글 글꼴은 PC의 맑은 고딕/Noto Sans KR 계열을 사용하므로
  기기의 QuanPixel CJK 픽셀 폰트와 글자 모양/폭이 완전히 같지는 않습니다.
- 최종 버전은 실제 기기에서 1회 확인하는 것을 권장합니다.

추천 작업 흐름
1. 시뮬레이터에서 UI 위치/색상/배경을 먼저 조정합니다.
2. 원하는 숫자를 오른쪽 '현재 펌웨어에 넣을 값'에서 확인합니다.
3. 그 값만 TamaPoke.ino에 반영합니다.
4. 큰 수정이 끝난 시점에만 GitHub Actions 빌드 및 실제 기기 플래시를 합니다.


[v3.55 기본 이름 라벨 값]
HEADER_NAME_Y_NUDGE = 5
HEADER_TEXT_LIFT = -2
라벨 높이 = 29
이름 글자 크기 = 24px (펌웨어 uiSetTextSize(3))
