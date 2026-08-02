# flipper-tools

Flipper Zero üçün RFID/NFC alətləri toplusu.

Hazırda iş sahəsində bir FAP var: **[universal_card_reader](universal_card_reader/)** —
tək bir tətbiqdə həm 13.56 MHz NFC, həm də 125 kHz LF RFID kartları
oxuyur, saxlayır, yenidən yükləyir (Load) və emulyasiya edir.

## Repo strukturu

```
flipper-tools/
├── universal_card_reader/   # Universal Card Reader FAP-i
│   ├── application.fam      # manifest
│   ├── universal_card_reader.c
│   ├── reader_nfc.c / .h    # NFC skan/poll/emulyasiya
│   ├── reader_lf.c / .h     # LF RFID işçi axını
│   ├── card_info.c / .h     # ekranda göstərilən hesabat
│   ├── emv.c / .h           # kontaktsız ödəniş kartı EMV oxuma
│   ├── reader_ui.c / .h     # cihaz UI-si
│   ├── icon.png             # 10x10 menyu ikonu
│   └── README.md            # tətbiq səviyyəli təlimat
├── doc/
│   └── emv-read-diagnosis.md  # EMV oxuma diaqnozu (tarixi)
├── cap.py                   # serial CLI log yazma köməkçisi
├── logs/                    # cap.py çıxışları (gitignore-dadır)
├── CLAUDE.md                # inkişaf qaydaları
└── README.md                # bu fayl
```

## Qurma ön şərtləri

- Flipper Zero (rəsmi firmware 1.x və ya eyni API səviyyəsində fork)
- `ufbt` — `pip install --upgrade ufbt`

## Qurma və işə salma

FAP-i app qovluğundan yığ:

```sh
cd universal_card_reader
ufbt        # -> dist/universal_card_reader.fap
ufbt launch # yığ, yüklə və bağlı cihazda işə sal
```

`ufbt launch` USB portu tutur; əgər qFlipper açıqdırsa, əvvəlcə onu bağla.
Cihazda: **Apps → Tools → Universal Card Reader**.

Ətraflı qurma, dəstəklənən protokollar, EMV davranışı və düymə
naviqasiyası üçün bax: [universal_card_reader/README.md](universal_card_reader/README.md).
İnkişaf qaydaları (log tutma, firmware fork uyğunluğu, hardware
ardıcıllığı) üçün bax: [CLAUDE.md](CLAUDE.md).

## Log/debug workflow

`cap.py` ilə cihazın serial CLI çıxışını tut:

```sh
python cap.py --port COM3 --cmd "log info" --deadline 15.0
```

Transkriptlər avtomatik `logs/` qovluğuna yazılır (məs. `logs/cap_*.log`).
Ətraflı CLI əmrləri və log təhlükəsizliyi qaydaları üçün bax `CLAUDE.md`.

## Etik/qanuni qeyd

Yalnız öz kartlarınızı və ya sınamaq üçün açıq icazəniz olan kartları
oxuyun. Digər şəxslərin və ya təşkilatların icazəsiz oxunması/qeyd
edilməsi çox ölkədə qanun pozuntusudur.
