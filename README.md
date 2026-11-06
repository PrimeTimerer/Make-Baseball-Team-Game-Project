# Make Baseball Team Game Project

**プロ野球チームをつくろう！3** (PlayStation 2, SEGA 2005, SLPM-65853)의 PC 네이티브 포트입니다.
게임의 EE 코드를 C++로 정적 재컴파일(static recompilation)하고, 그래픽은 PCSX2의 GS를 써서 고해상도로 그립니다.

> **게임은 포함되어 있지 않습니다.** 직접 가지고 있는 디스크로 만든 이미지(`.iso`, SLPM-65853)가 있어야 하며,
> 실행할 때 디스크의 실행 파일(`SLPM_658.53`)을 SHA-256으로 대조해 정품 디스크에서만 동작합니다.
> 게임과 그 안의 모든 데이터·코드의 권리는 **SEGA**에 있습니다. 이 저장소는 SEGA와 관계없는 비영리 팬 프로젝트이며,
> 게임의 코드나 데이터를 담고 있지 않습니다(변환된 게임 코드와 VU1 마이크로코드는 각자의 ISO에서 로컬로 만들어집니다).

## 현재 상태

- 부팅 → 시스템 데이터(메모리카드) → SEGA 로고 → 타이틀 → 메뉴 → 운영(2D) 화면
- 3D 경기: VU1 마이크로코드를 미리 컴파일해 실시간 속도(60 fields/s)
- 메모리카드: 게임 자체의 MCMAN/MCSERV + `Mcd001.ps2`(PCSX2 형식). 폴더형 세이브 가져오기 지원
- 오프닝 무비(OPMV)는 아직 건너뜀

## 빌드 (Windows)

필요한 것: Visual Studio 2022(C++), CMake, Ninja, Python 3, 그리고 본인의 디스크 이미지.

```bat
copy <디스크 이미지>.iso game\
py -3 build.py configure -DURBAN_BUILD_RECOMP=ON -DURBAN_BUILD_TOOLS=ON
py -3 build.py generate --iso game\<디스크 이미지>.iso
py -3 build.py build
build\port\bin\yktk3.exe
```

- 첫 전체 빌드는 1시간 정도 걸립니다(생성 코드 2.5만 파일 + VU1 코드). 이후에는 바뀐 파일만 다시 빌드합니다.
- 3D 속도용 VU1 코드: `generate`가 저장소의 조립 목록(`recomp/vu1_images.txt`: ELF 안의 위치만 있고 게임 바이트는 없음)으로
  본인 ELF에서 VU1 마이크로코드 이미지 466개를 다시 조립해 해시로 확인한 뒤 미리 컴파일합니다. 캡처는 필요 없습니다.
  새 장면을 캡처했다면(`PS2X_VU1_CAPTURE=build\vu1_capture`) `py -3 tools\vu1_generate.py --record`로 목록에 더합니다.
- 설정: `build\port\bin\config.ini` (내부 해상도 1~8배, 화면비, 필터 등).
- 런처: 빌드하면 `build\port\bin\YKTK3 Launcher.exe`도 만들어집니다(`tools\launcher`, WPF; 윈도우에 들어 있는 .NET
  Framework 4로 컴파일, 추가 설치 없음).
  - 홈: 플레이, 안전 설정으로 실행, 디스크 자동 확인, PC 사양(그래픽카드·VRAM·모니터)에 맞춘 추천 설정, 플레이 기록, 표지 이미지
  - 화면: `config.ini`의 해상도·창·필터·샤프닝·색·HD 텍스처 설정 전부
  - 컨트롤러: 연결된 패드(게임과 같은 번호), 1P·2P 패드 지정, 눌러서 버튼 지정, 데드존·감도·진동 (`controls.ini`, PCSX2 형식)
  - 메모리카드: 세이브 목록(일본어 제목), 실행 전 자동 백업과 복원, PCSX2 카드 가져오기, 폴더 세이브로 새 카드 만들기
  - 치트: `mods` 폴더의 `.pnach`를 켜고 끄기(게임 중에도 바로 적용), `.pnach`/`.zip` 추가
  - 로그: 실행마다 `logs`에 기록, 오류로 끝나면 원인을 쉬운 말로 안내하고 안전 설정으로 다시 실행
  - 도구: 바탕화면·Steam 바로가기(`--play`: 창 없이 바로 시작), GitHub 새 버전 확인과 설치(git pull + 다시 빌드)
- 경기 배속(게임 기능): 경기 중 R2 올림, L2 내림.
- 게임 속도: **F7** 200% 켜기/끄기, **F8** 터보(PC가 낼 수 있는 최대 속도) 켜기/끄기. 100%보다 빠른 동안은 소리가 꺼집니다.
  `config.ini` [Runtime] `PS2X_SPEED=200`(또는 `0` = 터보)이면 그 속도로 시작합니다(런처의 화면 → 시작 속도).
- 단축키: F1 단축키 목록(화면 왼쪽 위), F2 게임 메뉴(치트·선수 치트), F3 FPS, F4 치트 다시 읽기, F5 빠른 저장,
  F9 빠른 불러오기, F7 200%, F8 터보, F11 전체 화면. 개발용은 Ctrl과 함께 눌러야 해서 실수로 켜지지 않습니다:
  Ctrl+F6 성능 프로필 기록(`perfil_<n>.txt`), Ctrl+F7 프로필 중 표시, Ctrl+F8 디버그 덤프(VU1 기록·RAM),
  Ctrl+F1 디버그 패널(디버그 빌드). F6·F10·F12는 비워 두었습니다.
- 치트: `build\port\bin\mods`의 PCSX2 `.pnach`(런처의 치트 페이지에서 켜고 끔, 게임 중에도 바로 적용). 주소 처리는
  PCSX2 1.6.0과 같습니다. 값만 쓰는 데이터 치트는 그대로 작동하고, 게임 코드를 바꾸는 코드 치트는 `generate`가 게임과
  함께 C++로 번역합니다(`tools\cheat_code.py`): 치트가 바꾸는 곳마다 치트 버전을 함께 컴파일해 메모리에 있는 명령어에 맞는
  쪽을 실행하고, 치트가 빈 메모리에 넣는 루틴은 따로 컴파일합니다. 새 코드 치트를 넣으면 `generate`와 `build`를 다시
  하면 됩니다(바뀐 부분만). 코드 치트를 끄면 원래 명령어가 바로 돌아옵니다.
- 성능 기록: 게임이 도는 동안 `logs\perf-<시각>.txt`에 1초에 한 줄(게임 날짜, 실제 속도와 목표, 가장 긴 프레임, VU1이
  해석기/컴파일 코드로 돈 비율)을 씁니다. 느려지면 화면 사진(`logs\perf-<시각>\slow-N.bmp`)을 남기고, 컴파일되지 않아
  해석기로 돈 VU1 프로그램은 `logs\perf-<시각>\vu1`에 저장됩니다. `py -3 tools\vu1_generate.py --capture <그 vu1 폴더>
  --record` 뒤 `build`하면 그 3D 장면이 컴파일 코드로 돕니다. `PS2X_PERF_LOG=0`이면 끕니다.
- 선수 치트: 게임 중 F2 메뉴의 "선수 치트" 탭. 내 구단을 스스로 찾아(새 게임·불러오기 모두) 그 선수들에게만
  `player_cheat_items.txt`의 항목(kroon37 팀 S+를 능력치별로 나눈 27개: 체력·구위·제구력·변화구·교타·장타·주력·
  어깨·선구안·수비 적성 등)을 매 프레임 씁니다. 능력치 자리는 ISO 개조 자료(166바이트 선수 레코드)와 메모리를 맞춰
  확인했습니다. 항목마다 켜고 끌 수 있고,
  선수를 고정하면 선수 번호(기록 +0)로 따라가서 등번호나 로스터 자리가 바뀌어도 그 선수에게만 개별 항목을 씁니다.
  "선수 찾기 · 편집" 탭은 12개 구단 선수 표(검색·구단·위치 거르기, 머리줄 클릭 정렬, 체크·Ctrl·Shift로 여러 명)와
  선수 카드입니다. 카드에서 능력치마다 새 값(0~99, 255까지)이나 등급(S+ 99 … D 34)·상한·득의도를 정해 '적용'(한 번)
  또는 '고정'(매 프레임, S+ 항목보다 우선)을 합니다. 여러 명을 체크하면 같은 값을 한꺼번에, "단체 적용" 탭에서는
  단체 대상 전원에게 능력치 고정을 겁니다(개별 고정이 더 우선). 끄면 이번 실행에서 처음 쓰기 전 값으로 돌아갑니다.
  설정은 `player_cheats.ini`에 저장됩니다.
- 스카우트: F2 메뉴의 "스카우트" 탭. '하루 발굴 수'를 정하면 스카우트를 '발굴'로 보낸 날마다 그곳(국내 대회 지역·해외
  리그)의 후보를 그만큼 꼭 발굴합니다(게임의 발굴 판정 sub_00193570 / sub_00193DE0의 확률 굴림을 이김). 국내 신인 234명·
  해외 후보 180명 목록에서 '찜'한 선수는 스카우트 없이 바로 발굴되고(정보도 모두 공개), '독점'(찜·발굴한 선수 포함)은
  CPU 구단의 관심·평가를 계속 지워 쫓지 못하게 합니다. "DB에서 넣기"는 게임 DB의 아무 선수(현역·OB·아마추어·외국인)를
  국내(지역·구분)나 해외 리그 목록의 빈 칸에 발굴 상태로 넣고(빈 칸이 없으면 미발굴 후보 하나와 바꿈), 목록마다 '옮기기'로
  국내↔해외 사이를 옮깁니다. 항목은 게임의 빌더(sub_0017A030 / sub_0012D190)와 같은 규칙으로 만듭니다. 외국인 이름 옆
  '(모델: …)'은 `scout_models.tsv`에서 읽으며, 이 파일은 `py -3 tools\scout_models.py`가 gamezukushi.com의 외국인 선수
  일람에서 받아 만듭니다(그 사이트의 자료라 저장소에는 넣지 않음). 설정은 `scout_cheats.ini`. 이 포트의 선수 기록은 PCSX2보다 0x40 낮은 주소에 있어서 PCSX2의 고정 주소
  선수 치트는 어긋납니다. `mods`의 `팀 S+ - <구단> (새 게임 + 불러오기).pnach`는 구단 표를 따라가는 포인터 코드라 맞습니다.

## 크레딧

- [Urban Reign PC port](https://github.com/thiagordsfx/urban-reign-recomp) (thiagordsfx): 이 포트의 기반(런타임, PCSX2 GS
  연동, 빌드). 원래 README는 `docs/upstream/`에 있습니다.
- [PS2Recomp](https://github.com/ran-j/PS2Recomp)와 그 포크들, [PCSX2](https://github.com/PCSX2/pcsx2)의 GS,
  raylib, SDL3 외 `thirdparty/README.md`의 라이브러리들.

## 라이선스

GPL-3.0 (`LICENSE`). 게임 자체는 SEGA의 저작물이며 이 라이선스의 대상이 아닙니다.

---

*English:* a native PC port of *Pro Yakyuu Team wo Tsukurou! 3* (PS2, SLPM-65853) by static recompilation, based on the
Urban Reign PC port. **No game code or data is included**: you need your own disc image, checked by hash at start. The
game belongs to SEGA; this is a non-commercial fan project not affiliated with SEGA.
