TamaPoke 한국어판 v3.63.6 Reserved Species Guard + Training Reward Boost + Stability

1. ZIP을 풀어 GitHub 저장소 루트에 덮어씁니다.
2. 기존 tools/catalog_lock.json이 있다면 유지합니다.
3. GitHub Pages Source는 GitHub Actions를 사용합니다.
4. Actions에서 `TamaPoke PMDCatalog` workflow를 실행합니다.
5. build/deploy 성공을 확인합니다.
6. Pages 설치기에서 v3.63.6 펌웨어를 설치하고 기기를 재부팅합니다.

현재 버전 핵심
- 꿰뚫는화염(National Dex #1020)과 날뛰는우레(#1021)를 현재 게임 등장/획득 대상에서 제외
- 두 종의 National Dex 번호와 내부 base ID(1038/1039)는 그대로 예약하여 향후 재활성화 가능
- 두 종은 DEX_ENABLED=0으로 알/랜덤 등장/보스 후보/배틀 참가 대상에서 제외
- 이전 SD카드에 남은 p1038/ps1038/p1039/ps1039 파일은 WebSerial 삭제 목록에 포함
- 정상 훈련 완료 시 해당 개체열매 5개 확정, 30% 확률로 총 9개
- 훈련 완료 시 별도 30% 확률로 샤이니열매 1개
- 기존 반짝부적의 다음 알 Shiny 확률 증가 기능 유지
- 기술폭 확장, 저장/배틀/오디오/화면/SD 안정화 유지
- 과거 업데이트 노트와 SHA-256 파일은 배포 ZIP에 포함하지 않음
