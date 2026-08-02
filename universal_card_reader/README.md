# Universal Card Reader

Flipper Zero FAP-i — `application.fam` təsviri: **Reads any NFC (13.56MHz) or LF RFID (125kHz) card.**

- **Versiya:** 1.4 (`application.fam`də `fap_version`)
- **Kateqoriya:** Tools
- **Yığma stack:** 12 KB
- **Müəllif:** farhadsoft

## Nə edir

Bir tətbiqdə iki diapazonu növbəli skan edir:

- **NFC — 13.56 MHz:** ISO14443-3A/3B/4A, ISO15693-3, FeliCa, St25tb,
  Mifare Ultralight/NTAG, Mifare Classic.
- **LF RFID — 125 kHz:** firmware-in LF RFID işçi axını ilə avto-aşkarlanan
  protokollar (məs. EM4100, HID Prox, Indala və s.).

Flipper eyni vaxtda hər iki radiodu işlədə bilmədiyi üçün app
növbəli fazalarla işləyir: NFC fazası (~1200 ms) → LF fazası (~1600 ms) →
dövr. Kart tapılanda aktiv faza dayanır və oxuma aparılır.

Bütün radio start/stop çağırışları GUI axınında edir; işçi axınları və
faza taymeri yalnız `view_dispatcher_send_custom_event()` göndərir.

## Göstərilən məlumat

NFC kart üçün nəticə ekranında (mövcud olduqca):

- Band (13.56 MHz)
- Tip (skanerin qaytardığı ən dəqiq protokol adı)
- Protokol zənciri (məs. `ISO14443-3A > ISO14443-4A > EMV`)
- UID
- ISO14443-3A: ATQA, SAK
- ISO14443-4A: ATS (TL/T0/TA1/TB1/TC1) və tarixi baytlar
- ISO15693-3: istehsalçı kodu, DSFID, AFI, IC ref, blok sayı və blok
  məzmunu (oxunanlar)
- FeliCa: IDm, PMm, oxunan/blok sayı
- Mifare Ultralight/NTAG: tip, oxunan/səhifə sayı, səhifə hex dökümü,
  NDEF URI/Text qeydləri
- Mifare Classic: tip (Mini/1K/4K), oxunan sektor sayı, sektor xəritəsi,
  oxunan blokların hex dökümü
- EMV / bank kartı: AID(s), tətbiq etiketi, PAN, son istifadə tarixi,
  kart sahibinin adı, service code, issuer country, card sequence,
  Track2 məlumatı, əməliyyat jurnalı (kart verərsə)

LF RFID üçün:

- Band (125 kHz)
- Tip
- ID (hex)

EMV məlumatları kartın özü verdiyi qədərdir; kart vermədiyi sahə
`not disclosed` / `not available over contactless` olaraq göstərilir.

## Ödəniş kartı (EMV) davranışı

Kontaktsız bank kartları (Visa, Mastercard, AmEx, Discover, JCB, UnionPay
və s.) üçün app ISO14443-4A səviyyəsində aşağıdakı **yalnız oxuma**
əmrlərini işlədir:

1. SELECT PPSE (`2PAY.SYS.DDF01`)
2. SELECT AID (PPSE uğursuz olarsa tanınmış AID-lərlə fallback)
3. GET PROCESSING OPTIONS (PDOL-dan qurulmuş standart terminal dəyərləri ilə)
4. READ RECORD (AFL üzrə)
5. Əməliyyat jurnalı üçün GET DATA `9F4F` və READ RECORD

Yazma, yeniləmə və ya PIN/cripto əməliyyatları aparılmır. CVV/CVC2, PIN
və kartın özəl açarları secure elementdən çıxarılmır.

**Yadda saxlama:** EMV kartları app-ın öz data qovluğuna
(`/ext/apps_data/universal_card_reader/EMV_<UID>.emv`) yazılır və PAN,
son istifadə tarixi, kart sahibi, AID-lər, Track2 və jurnal kimi oxunan
bütün maliyyə sahələrini saxlayır.

**Emulyasiya:** EMV kartı üçün Emulate seçiləndə app ISO14443-4A
nəqliyyat səviyyəsində emulyasiya başladır (yaxalanmış UID/ATS ilə).
Tətbiq səviyyəsində EMV terminal emulyasiyası **yoxdur**; yəni PAN və
jurnal başqa bir oxuyucuya ötürülmür.

> **Qeyd:** Nəticə ekranının sonunda `[Policy] Bank card: emulation disabled;
> save stores UID/ATS only.` sətri görünə bilər. Bu bildiriş köhnəlib:
> hazırkı kodda EMV məlumatları `.emv` faylında saxlanılır və Emulate
> ISO14443-4A səviyyəsində işləyir.

## Kartları yadda saxlamaq və yükləmək (Save / Load)

Bütün yadda saxlanan kartlar (`.nfc`, `.emv`, `.rfid`) tək bir qovluqda —
`/ext/apps_data/universal_card_reader/` altında — app-ın öz data
qovluğunda saxlanılır, bayaqkı paylaşılan `/ext/nfc` və `/ext/lfrfid`
qovluqlarından ayrı. Həmin köhnə qovluqlardakı fayllar toxunulmaz qalır —
köçürülmür, silinmir.

**Load** əməliyyatlar menyusunda (Save/Emulate/Rescan-dan sonra) və ya
skan ekranında birbaşa **OK** düyməsi ilə açılır: firmware-in öz fayl
seçici dialoqu yalnız bu qovluğu göstərir. Seçilən fayl nəticə ekranında
elə canlı oxunmuş kart kimi göstərilir; nəqliyyat məlumatı olan fayllar
(`.nfc`/`.rfid`) **Emulate** ilə işə salına bilər, təkcə EMV sahələri olan
`.emv` faylları isə (arxasında heç bir nəqliyyat məlumatı olmadığı üçün)
emulyasiya üçün bloklanır.

## Məhdudiyyətlər

- Mifare Classic sektorları yalnız nəqliyyat açarı `FF FF FF FF FF FF`
  ilə oxunur; digər açar tələb edən sektorlar oxunmayacaq.
- ISO14443-4B və SLIX emulyasiyası dəstəklənmir; bu kartlar nəqliyyat
  protokoluna qayıdaraq oxunur.
- UHF / 2.45 GHz Flipper-in daxili avadanlığı ilə dəstəklənmir.
- EMV emulyasiyası yalnız ISO14443-4A nəqliyyat səviyyəsindədir.
- **Yüklənmiş (fayldan açılmış) Mifare Classic kartını Emulate etmək bəzən
  app-i asıla bilər** — bu, firmware səviyyəsində tanınan, uzun müddətdir
  davam edən bir problemdir (bax: rəsmi firmware issue #2577, Unleashed
  issue #257), bu app-in kodundan qaynaqlanmır. Asılma zamanı `loader
  close` işləmir; bərpa üçün cihazı yenidən başlatmaq (`power reboot` CLI
  əmri və ya fiziki reset) lazımdır. Canlı oxunmuş kartı birbaşa
  emulyasiya etmək bu problemi göstərməyib.

## Qurma

App qovluğundan:

```sh
cd universal_card_reader
ufbt
```

Nəticə: `dist/universal_card_reader.fap`.

Bağlı cihaza yükləmək və işə salmaq:

```sh
ufbt launch
```

> `ufbt launch` USB portu tutur. Əgər qFlipper açıqdırsa, əvvəlcə onu
> bağlayın.

Alternativ olaraq `dist/universal_card_reader.fap` faylını SD kartın
`apps/Tools/` altına qoyub cihazda **Apps → Tools → Universal Card
Reader** seçin.

Menyu ikonu `icon.png` faylıdır; lazım gələrsə `make_icon.py` ilə
10×10 1-bit PNG yenidən yaradıla bilər.

## İstifadə

1. App-i işə salın. Ekran dərhal skan etməyə başlayır və aktiv bandı
   göstərir.
2. Kartı Flipper-in arxasına tutun (həm NFC, həm LF antenası oradadır).
3. Kart oxunanda nəticə ekranı açılır; məzmun çoxdursa **Yuxarı** /
   **Aşağı** ilə sürüşdürün.
4. Nəticə ekranında **Geri** düyməsi əməliyyatlar menyusunu açır:
   **Save**, **Emulate**, **Rescan**, **Load**, **Exit**.
5. Əvvəllər yadda saxlanmış kartı açmaq üçün **Load** seçin (və ya skan
   ekranından birbaşa **OK** basın) və siyahıdan faylı seçin.
6. Skan ekranında **Geri** app-dən çıxar.

## Fayl strukturu

| Fayl | Təyinat |
|---|---|
| `universal_card_reader.c` | App-in əsas həyat dövrü, save/emulate, fazalar |
| `reader_app.h` | Strukturlar, enumlar, sabitlər |
| `reader_nfc.c/h` | NFC skaner/poller, protokol həll etmə, emulyasiya |
| `reader_lf.c/h` | LF RFID işçi axını, emulyasiya |
| `card_info.c/h` | NFC/LF nəticələrinin ekran üçün formatlanması |
| `emv.c/h` | Yalnız oxuma EMV APDU zənciri, `.emv` save/load |
| `reader_ui.c/h` | Cihaz UI-si (scan/reading/emulating ekranları) |
| `application.fam` | FAP manifesti |
| `icon.png` / `make_icon.py` | Menyu ikonu |

## Təhlükəsizlik və etik

Yalnız öz kartlarınızı və ya sınamaq üçün açıq icazəniz olan kartları
oxuyun, saxlayın və emulyasiya edin. İcazəsiz ödəniş kartı məlumatlarının
oxunması/qeyd edilməsi çox yurisdiksiyada cinayət sayılır. Emulyasiya
fiziki kartın surətinə sahib olmaq kimi qiymətləndirilir; ona uyğun
qayğı ilə yanaşın.

## Firmware fork uyğunluğu

App `NfcProtocolNum` / `NfcProtocolInvalid` kimi fork-arası stabilliyi
olmayan sentinel dəyərlərdən istifadə etmir. Protokol münasibətləri
`nfc_protocol_has_parent()` ilə firmware tərəfindən qiymətləndirilir.
Rəsmi firmware 1.x və Momentum `mntm-dev` (API 87.1) üzərində yoxlanıb.

APPCHK yalnız API major/minor müqayisə edir; fork-lar eyni API versiyasını
saxlayıb enum dəyərlərini dəyişə bilər. App bunun qarşısını almaq üçün
yalnız rəsmi və Momentum-da eyni qalan protokol ID-lərini (0–11) poll və
emulyasiya üçün istifadə edir.
