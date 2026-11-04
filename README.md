# Make Baseball Team Game Project

**プロ野球チームをつくろう！3** (PlayStation 2, SEGA 2005, SLPM-65853)의 PC 네이티브 포트입니다.
게임의 EE 코드를 C++로 정적 재컴파일(static recompilation)하고, 그래픽은 PCSX2의 GS를 써서 고해상도로 그립니다.

> **게임은 포함되어 있지 않습니다.** 직접 가지고 있는 디스크로 만든 이미지(`.iso`, SLPM-65853)가 있어야 하며,
> 실행할 때 디스크의 실행 파일(`SLPM_658.53`)을 SHA-256으로 대조해 정품 디스크에서만 동작합니다.
> 게임과 그 안의 모든 데이터·코드의 권리는 **SEGA**에 있습니다. 이 저장소는 SEGA와 관계없는 비영리 팬 프로젝트이며,
> 게임의 코드나 데이터를 담고 있지 않습니다(변환된 게임 코드와 VU1 캡처는 각자의 ISO에서 로컬로 만들어집니다).

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

- 첫 전체 빌드는 약 35~50분 걸립니다(생성 코드 2.5만 파일). 이후에는 바뀐 파일만 다시 빌드합니다.
- 3D 속도용 VU1 코드: `PS2X_VU1_CAPTURE=build\vu1_capture`로 경기를 돌린 뒤 `py -3 tools\vu1_generate.py`, 다시 빌드.
- 설정: `build\port\bin\config.ini` (내부 해상도 1~8배, 화면비, 필터 등).
- 경기 배속(게임 기능): 경기 중 R2 올림, L2 내림.

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
