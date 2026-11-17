# VedatOnline — Development Plan (v0.1)

Tarih: 2026-11-15 · Durum: onay bekliyor

Motor kullanmadan, C++ ile yazılan, PostgreSQL destekli, klasik 3. şahıs MMORPG.
Tür olarak eski nesil Kore MMORPG'lerinden ilham alır; **isimler, ırklar, harita, görseller ve müzik tamamen özgün** olacak (mevcut bir oyunun kopyası değil).

## 1. İlk hedef (Milestone "İlk Adım")

1. Login ekranı (kullanıcı adı + şifre, hata mesajları)
2. Hesapta karakter yoksa: ırk seçimi + karakter yaratma ekranı
3. Karakterle dünyaya giriş, harita üzerinde gezme, diğer oyuncuları görme

Bitti sayılma ölçütü: iki ayrı client aynı sunucuya bağlanır, farklı hesaplarla girer, karakter yaratır, haritada birbirlerinin yürüdüğünü görür; sunucu yeniden başlatılınca karakter ve son konum DB'den geri gelir.

## 2. Teknoloji kararları

| Konu | Karar | Neden |
|---|---|---|
| Dil / derleyici | C++20, MSVC 14.5x (VS 2026) | Kurulu, D3D için doğal |
| Build | CMake 4 + Ninja + CMakePresets, vcpkg manifest | Hepsi VS ile kurulu geliyor |
| Grafik | **Direct3D 11**, HLSL (fxc ile build'de derlenir) | D3D12'ye göre çok daha hızlı ilerleme; ileride soyutlama katmanı üzerinden D3D12 eklenebilir |
| Pencere / input | Ham Win32 (WM_* + Raw Input) | Motor yok |
| Matematik | DirectXMath | SDK ile geliyor |
| UI | Kendi yazdığımız retained-mode UI (panel, buton, textbox, liste) + bitmap/SDF font atlası | Oyun içi görünüm tamamen bizim |
| Ağ | TCP, standalone Asio (Windows'ta IOCP) — client ve server ortak | Olgun, header-only |
| Kripto | libsodium: X25519 anahtar değişimi + XChaCha20-Poly1305 AEAD, şifreler Argon2id | Kendi kripto algoritmamızı yazmıyoruz |
| DB | PostgreSQL 17, libpqxx, düz SQL migration dosyaları | İstenen |
| Asset | Blender (headless, Python script) → glTF → kendi `assetc` aracımız → `.vmesh/.vanim/.vtex` | Runtime'da parse maliyeti yok |
| Görsel üretim | Higgsfield: konsept art, login arka planı, UI çerçeveleri, zemin dokuları, gerekirse 3D model taslağı | İstenen |
| Test | Catch2 (unit) + headless bot client (uçtan uca) | Agent çıktısını otomatik doğrulamak için şart |
| Ses | Bu milestone'da yok (sonra XAudio2) | Kapsam |

Küçük, tek başlıklı kütüphaneler serbest (cgltf, stb_image, stb_truetype); motor/framework (SDL, raylib, bgfx, ImGui oyun içi UI olarak) yok. ImGui sadece debug overlay için opsiyonel.

## 3. Mimari

```
VedatOnline/
  CMakeLists.txt, CMakePresets.json, vcpkg.json
  docs/                 plan, protokol spesifikasyonu, kod standartları
  shared/               client+server ortak statik kütüphane
    net/                frame, paket okuyucu/yazıcı, opcode'lar, kripto oturumu
    game/               ırk/sınıf tanımları, hareket kuralları, sabitler
    core/               log, config, zaman, assert
  server/               tek exe (login + world modülleri; ileride ayrılabilir)
    net/ auth/ world/ db/
  client/
    platform/           Win32 pencere, input, zamanlayıcı
    render/             D3D11 cihaz, shader, mesh, terrain, skinning, sprite/font
    ui/                 widget sistemi
    scenes/             Login, CharSelect, CharCreate, World
    net/                bağlantı + paket işleyiciler
  tools/
    assetc/             glTF/PNG → oyun formatları
    botclient/          headless test istemcisi
    blender/            model/animasyon üreten Python scriptleri
  db/migrations/        0001_init.sql ...
  assets_src/           kaynak (blend, gltf, png)   assets/  derlenmiş çıktı
  tests/
```

### 3.1 Ağ protokolü

- Çerçeve: `[u16 uzunluk][şifreli gövde][16 bayt MAC]`; gövde = `[u16 opcode][payload]`, little-endian, elle serileştirme (PacketWriter/Reader, sınır kontrollü).
- El sıkışma: client `HELLO(protokol sürümü, geçici X25519 pubkey)` → server `HELLO_ACK(geçici pubkey, statik anahtarla imza)`. Client, binary'ye gömülü sunucu açık anahtarıyla imzayı doğrular (MITM koruması). Her yön için ayrı oturum anahtarı, sayaç tabanlı nonce (replay koruması).
- Limitler: maks paket 8 KB, el sıkışma/login zaman aşımı, IP başına login deneme limiti, opcode başına durum kontrolü (login olmadan world paketi kabul edilmez).
- Şifre, şifreli kanal içinde gider; sunucuda Argon2id hash olarak saklanır.

Paketler (ilk milestone):
`HELLO/HELLO_ACK`, `LOGIN_REQ/RES`, `CHAR_LIST_REQ/RES`, `CHAR_CREATE_REQ/RES`, `CHAR_DELETE_REQ/RES`, `ENTER_WORLD_REQ/RES`, `MOVE_REQ`, `ENTITY_SPAWN`, `ENTITY_DESPAWN`, `ENTITY_MOVE`, `POSITION_CORRECT`, `PING/PONG`, `LOGOUT`, `ERROR`.

### 3.2 Sunucu

- Ağ thread'leri (Asio) + **tek thread'li dünya döngüsü** (20 Hz tick); aralarında kilitli kuyruk. Oyun mantığında veri yarışı olmaz.
- DB işlemleri ayrı worker havuzunda (bağlantı havuzu), sonuçlar dünya thread'ine callback ile döner; dünya döngüsü asla DB beklemez.
- Hareket sunucu otoriteli: client hedef/yön gönderir, sunucu hız ve arazi sınırlarını doğrular, sapmada `POSITION_CORRECT`.
- İlgi alanı (AOI): grid hücreleri; sadece yakın oyunculara spawn/move yayını.
- Aynı hesaba çift giriş: eski oturum düşürülür. Konum periyodik + çıkışta kaydedilir.

### 3.3 Veritabanı (ilk şema)

- `accounts(id, username UNIQUE, password_hash, created_at, last_login_at, banned)`
- `characters(id, account_id FK, name UNIQUE, race, class, face, hair, level, zone_id, x, y, z, yaw, created_at, deleted_at)` — hesap başına 3 slot
- `schema_migrations(version, applied_at)`
- İsim kuralları (3–16 karakter, harf/rakam, büyük-küçük duyarsız benzersiz) hem client hem server'da.

### 3.4 Client

- Sahne durum makinesi: `Login → CharSelect ⇄ CharCreate → Loading → World`.
- Render: forward renderer; heightmap terrain (4 katman splat doku), skybox, statik mesh, GPU skinning'li karakter, yönlü ışık + basit gölge (gölge opsiyonel), sis, 2D sprite/yazı batch'i.
- Kamera: karakter etrafında dönen 3. şahıs (sağ tık döndür, tekerlek zoom). Hareket: WASD + sol tıkla zemine yürüme.
- Diğer oyuncular: sunucu güncellemeleri arasında interpolasyon; kafa üstü isim etiketi.
- Türkçe karakter desteği (font atlasında ç, ğ, ı, İ, ö, ş, ü), pencere boyutlandırma, cihaz kaybı/alt-tab dayanımı.

### 3.5 İçerik (özgün, taslak isimler)

- 2 ırk: **Aydor** (insan krallığı) ve **Kargan** (iri yapılı dağ halkı). 4 sınıf arketipi: Savaşçı, Haydut, Büyücü, Şifacı (bu milestone'da sadece görsel/istatistik başlangıcı).
- 1 başlangıç haritası (~512×512 m): köy meydanı, tepeler, yol, ağaç/kaya/çit gibi 8–10 statik obje.
- Animasyon: idle, walk, run.

## 4. Milestone'lar

| # | İçerik | Doğrulama |
|---|---|---|
| **M0 Temel** | Repo, CMake/vcpkg, boş client+server exe, log/config, kod standartları, **protokol spesifikasyonu** (sözleşme önce) | Temiz makinede tek komutla build |
| **M1 Ağ + Kripto** | shared/net, el sıkışma, şifreli çerçeve, ping | Unit test + bot client echo |
| **M2 DB + Hesap** | Migration'lar, account/character repository, login, karakter CRUD, `admin` CLI (hesap aç) | Bot client: login → karakter yarat → listele |
| **M3 Client çekirdek** | Win32 pencere, D3D11, sprite/font, UI widget'ları | Ekran görüntüsü kontrolü |
| **M4 Login + Karakter ekranları** | Login, karakter seçim, ırk seçim, yaratma (3D önizleme) | Gerçek sunucuya karşı elle + ekran görüntüsü |
| **M5 Asset hattı** | Blender scriptleri, assetc, karakter/harita/obje/dokular, Higgsfield görselleri | assetc testleri, model önizleme |
| **M6 Dünya** | Terrain, karakter render+animasyon, kamera, hareket, AOI, diğer oyuncular | 2 client + 50 bot aynı haritada |
| **M7 Sağlamlaştırma** | Kopma/yeniden bağlanma, hata ekranları, bozuk paket fuzz testi, paketleme scripti | Test raporu |

M1–M2 (server tarafı), M3 (client çekirdek) ve M5 (asset) birbirine paralel yürür; M4 ve M6 bunların birleştiği yerdir.

## 5. Ekip modeli (müdür + ajanlar)

- **Müdür (Fable 5.2):** mimari, protokol sözleşmesi, iş paketlerinin yazımı, her teslimatın kontrolü, entegrasyon, rapor.
- **Ajanlar (Opus 5.5):** her biri tek iş paketi ve net dosya sahipliğiyle çalışır; iki ajan aynı dosyaya yazmaz.

| Ajan | Sahiplik |
|---|---|
| A — Ağ/Kripto | `shared/net`, `shared/core` |
| B — Sunucu/DB | `server/`, `db/` |
| C — Render/Platform | `client/platform`, `client/render` |
| D — UI/Sahneler | `client/ui`, `client/scenes`, `client/net` |
| E — Asset | `tools/blender`, `tools/assetc`, `assets_src` |
| F — Test/Araç | `tools/botclient`, `tests/` |

Her teslimatın kabul süreci:
1. Derleniyor mu (uyarılar hata sayılır, `/W4`), testler geçiyor mu — müdür kendisi çalıştırır, ajanın beyanına güvenmez.
2. Kod incelemesi: sözleşmeye uyum, sınır kontrolleri, thread güvenliği, kaynak sızıntısı.
3. Çalışma zamanı doğrulaması: bot client senaryosu veya client ekran görüntüsü.
4. Geçmezse aynı ajana somut düzeltme listesiyle geri döner. Geçerse git commit.
5. Her milestone sonunda sana kısa rapor: ne bitti, ne doğrulandı, ne eksik, sıradaki adım.

## 6. Eksik / risk kontrolü

Planın ilk hali üzerinden yapılan gözden geçirmede bulunan ve plana eklenen/karara bağlanan noktalar:

| Eksik veya risk | Çözüm |
|---|---|
| Hesap nasıl açılacak? (login ekranı var ama kayıt yok) | M2'de `admin` CLI ile hesap açma; sunucu config'inde `dev_auto_register` (geliştirmede bilinmeyen kullanıcı adı otomatik hesap olur). Oyun içi kayıt ekranı istenirse eklenir — **karar sende** |
| Kripto'da MITM | Sunucu statik anahtarı client'a gömülü, el sıkışma imzalı |
| Görsel çıktıyı ajan nasıl doğrulayacak | Client'a `--screenshot` ve `--autologin` debug bayrakları; müdür ekran görüntüsünü inceler |
| Sunucu ajanı client olmadan nasıl test edecek | Bot client M1'de yazılır, tüm sunucu işleri onunla doğrulanır |
| Paralel ajanların birbirini bozması | Sözleşme önce (protokol + shared header'lar müdür tarafından dondurulur), dosya sahipliği, git ile her kabul ayrı commit |
| Karakter modeli kalitesi (script ile üretilen model kaba olur) | İlk milestone'da stilize low-poly kabul; Higgsfield 3D üretimi + Blender'da rig denenir, olmazsa script modeli kalır. **En büyük görsel risk bu** |
| Türkçe karakterler ve klavye (ı/İ, isim benzersizliği) | UTF-8 her yerde, isimlerde izinli karakter listesi, DB'de `lower()` unique index |
| Bağlantı kopması / sunucu kapalı | Login'de anlaşılır hata, dünyada kopunca login'e dönüş (M7) |
| Kötü niyetli/bozuk paket | Uzunluk limitleri, durum makinesi, fuzz testi, rate limit |
| Hız hilesi | Sunucu otoriteli hareket doğrulaması |
| DB şema değişiklikleri | Numaralı migration dosyaları, sunucu açılışta uygular |
| Gizli bilgiler (DB şifresi, sunucu özel anahtarı) | `config/*.local.toml`, `.gitignore`'da; örnek dosya repoda |
| Lisanslar | Sadece permissive kütüphaneler (MIT/BSL/ISC/zlib); `THIRD_PARTY.md` |
| Fikri mülkiyet | Tüm isim/harita/görsel özgün; mevcut oyunlardan asset veya isim alınmaz |
| Bu milestone'da bilinçli olarak **yok** | Savaş, NPC/mob, envanter, sohbet, ses, çoklu bölge, launcher/patcher, anti-cheat, Linux sunucu build'i (kod taşınabilir yazılır ama test edilmez) |

## 7. Ortam durumu (2026-11-15 kontrolü)

| Araç | Durum |
|---|---|
| Visual Studio 2026 Enterprise + MSVC 14.52 | ✅ |
| Windows SDK 10.0.26100 (D3D11, fxc) | ✅ |
| CMake 4.3, Ninja, vcpkg (VS içinde) | ✅ |
| Git | ✅ (repo henüz `git init` edilmedi) |
| GPU: RTX 3080 Ti | ✅ |
| Docker Desktop | ⚠️ kurulu ama çalışmıyor |
| PostgreSQL | ❌ |
| Blender | ❌ |
| Python (sistem) | ❌ — gerekmiyor, Blender kendi Python'unu getirir |

Gerekenler: **Blender 4.x** kurulumu ve **PostgreSQL** (Docker Desktop'ı başlatmak yeterli; container'ı biz ayağa kaldırırız — ya da native PostgreSQL 17 kurulumu).

## 8. Açık kararlar

1. ~~PostgreSQL: Docker mı, native mi?~~ **Karar (2026-11-15):** kullanıcı hazır bir PostgreSQL için connection string verecek; sunucu bunu `config/server.local.toml` içindeki `db.connection_string` alanından (veya `VEDAT_DB_URL` ortam değişkeninden) okur. Repoya asla girmez.
2. Hesap açma: sadece admin CLI + dev otomatik kayıt mı, yoksa oyun içi kayıt ekranı da mı? (öneri: ilk etapta CLI + otomatik kayıt)
3. Görsel stil: stilize low-poly / el boyaması mı, gerçekçiye yakın mı? (öneri: stilize — üretim hattımızla tutarlı sonuç verir)

## 9. Durum (2026-11-15, M7 sonrası)

| # | Durum | Ne var | Nasıl doğrulandı |
|---|---|---|---|
| M0 | ✅ | CMake/vcpkg/Ninja, `build.ps1`, log/config, kod standartları, protokol sözleşmesi (`docs/PROTOCOL.md`) | `build.ps1 -Clean` uyarısız |
| M1 | ✅ | `shared/net`: imzalı X25519 el sıkışma, XChaCha20-Poly1305 çerçeve, `Connection` (M7'den beri birleştirilmiş, toplu yazmalı), bot client | Catch2 + `integration.*` |
| M2 | ✅ | PostgreSQL şema/migration, bağlantı havuzu (kopunca yeniden bağlanır), hesap/karakter servisi, `vo_admin` | `vedatonline_test` üzerinde birim ve `integration.db.*` testleri |
| M3 | ✅ | Win32 + D3D11, sprite/font (Türkçe), UI araç takımı, betikli otomasyon | ekran görüntüleri (`run-ui-flows.ps1`) |
| M4 | ✅ | Giriş, karakter seçme/yaratma (3D önizleme), tüm hata ekranları | UI akışları gerçek sunuculara karşı |
| M5 | ✅ | Blender scriptleri, `assetc`, `.vmesh/.vanim/.vterrain`, bölge 1 ve objeler | assetc ve içerik testleri |
| M6 | ✅ | Arazi, karakterler + animasyon, kamera, sunucu otoriteli hareket, AOI, diğer oyuncular | 200 bot yük testi (0 düzeltme, 0 kopma), dünya akışları |
| M7 | ✅ | Dünyadan çıkış yarışı (§5.4), tek `Connection`, protokol fuzz testi, slowloris/boşta/yarı açık bağlantılar, aşırı yük, zorla kapatma, dünyada çift giriş, dünyadayken DB kesintisi; iki gerçek istemci akışı; cihaz kaybında tam yeniden kurulum, küçültme/Alt+Tab, yüklenirken boyutlandırma, 4x MSAA, isim etiketlerinin obje/karakter arkasında gizlenmesi; `tools/package` paketleme | `ctest` (Debug + Release), `vo_fuzz_tests`, `run-ui-flows.ps1` (tüm akışlar), paketin repo dışından çalıştırılması |

"İlk Adım" ölçütü (iki istemci, farklı hesaplar, birbirini yürürken görme, yeniden başlatmada konumun DB'den gelmesi) `two_clients` akışı ve `integration.db.persist` ile doğrulanıyor.

Bilinçli olarak **yok** (bu milestone'un kapsamı dışında, §6'daki listeye ek olarak):

- Kopan bağlantıda otomatik yeniden bağlanma: istemci giriş ekranına döner ve sebebi gösterir; oturum yeniden açılmaz.
- Cihaz kaybında dünyada kalmak: istemci cihazı ve tüm GPU kaynaklarını yeniden kurar ama oturumu kapatıp girişe döner (dünya durumu GPU nesnelerine bağlı; yeniden girişte sunucu tam durumu tekrar gönderir).
- ~~Kamera objelerle çarpışmaz~~ → cila turu 1'de çözüldü (aşağıda).
- İsim etiketi gizleme kaba kutularla yapılır (ağaç gövdesi ile yaprak arası boşluklar da gizler); derinlik testi yok.
- Sunucu tek süreç; giriş/dünya ayrımı, yatay ölçek, Linux build'i yok. İstemci/sunucu imzalama, otomatik güncelleyici (launcher/patcher) yok.

## 10. Cila turu 1 (2026-11-15)

| Konu | Ne değişti | Nasıl doğrulandı |
|---|---|---|
| Görünümler | Yüz/saç seçicileri yerine tek "Görünüm n/N" seçici (◀ ▶; ırkın tek görünümü varken gizli). Kablo biçimi aynı: `face` = görünüm no, `hair` = 0. Görünüm n = `characters/<ırk>_<n>.vmesh` + kendi klipleri; yoksa n = 0 için eski `characters/<ırk>.vmesh`, diğerleri için görünüm 0. Görünümler açılışta dosyalardan keşfedilir (`client/world/CharacterAssets.h`); önizleme, yerel ve uzak oyuncular aynı tabloyu kullanır, klipler ırk + görünüm başına bağlanır. | sahte dosya kümeleriyle birim testleri, `looks` ve `world_preview` akışları (`expectpreviewmesh` / `expectlocalmesh` / `expectremotemesh`), ikinci görünüm geçici kopya dosyalarla denendi |
| Hikâye | `shared/game/Zones.h` (bölge 1 = Ortaocak): HUD, yükleme ekranı (bir satır hikâye), karakter kartları, giriş bandı "Ortaocak — Eşik'in tarafsız köyü". Giriş alt başlığı, ırk kartları, ırka göre sınıf satırları (Közbilimci, Körükçü, ...), ilk karakterde Közaran yemini `LORE.md`'den. | `polish` akışı 1280x720, 1024x640, 1024x640 @150% |
| Yürüme hızı | 2.0 → 1.4 m/s (koşu 5.5); oynatma hızı 0.6-1.6x ile sınırlı, geçişler 0.25 s | `polish` akışında ayak kayması ölçümü (`footprobe`), 200 bot yük testi |
| Kamera | Büyük objelere (ev, kule, kuyu, tezgâh, büyük kaya) karşı yönlü kutu çarpışması + ızgara; karakter ~1 m'den yakında titreşimli (dither) solar | birim testleri, `polish` akışında ev duvarı önü/sonrası ekran görüntüleri |
| Zıplama / oturma | Boşluk / X: yalnızca yerel, kozmetik (ağa bir şey gitmez) | birim testleri, `polish` akışı |
| Test güvenilirliği | `orbitremote` / `walktoremote` kapalı noktada en yakın açık yeri ve başka açıları dener; HUD oyuncu sayısı doğuşta/kayboluşta hemen güncellenir | `two_clients` art arda 15 kez |

Bilinen eksikler: kütüphanenin oturma klibi sandalyeye oturur (sandalye yok, karakter havada oturur görünür); zıplama/oturma diğer oyunculara gitmez; ağaçlar kamera engeli değildir.

## 11. M8 "Kuşan" — sunucu + ortak kod durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_ITEMS.md` (protokol v2). Bu bölüm yalnızca sunucu ve ortak yarıyı kapsar;
istemci (envanter penceresi, silahların ele takılması) ayrı bir ajanın işi.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Ortak kurallar | `shared/game/Items.{h,cpp}`: `gamedata/items.toml` yükleyici (sıkı doğrulama: yinelenen id, bilinmeyen sınıf/yuva/tür/tutuş/anahtar, başlangıç donanımında bilinmeyen id ya da yasak yuva → satır numaralı hata), envanter modeli, §1.1/§1.2 yerleştirme kuralları (`planItemMove`/`applyItemMove`), çift tık hedefi, başlangıç donanımı yerleşimi | `tests/game/ItemsTests.cpp` (yükleyici + her kural ve uç durum) |
| Protokol | `PROTOCOL_VERSION` 2, 0x0400–0x0404, `CharSummary` / `ENTITY_SPAWN` sonuna görünür ekipman | kodlayıcı gidiş-dönüş, sınırlar, rastgele baytla çözücü fuzz'ı, sunucu fuzz'ında ITEM_MOVE_REQ |
| Veritabanı | `0002_items.sql` (`starter_kit_granted`, `character_items`, ertelenebilir tekil yer kısıtı, `ON DELETE CASCADE`); `IAccountService::loadCharacterItems` (donanım tek işlemde bir kez) ve `applyItemMove` (tek işlem, eski yer kontrolü) — bellek ve PostgreSQL | ortak servis testleri (iki arka uç), eşzamanlı 8 yüklemede donanım tam bir kez, kısıtlar, `vo_admin` yardımcıları |
| Sunucu | açılışta şablonlar (geçersizse çıkış 6); dünyaya girişte donanım, `ENTER_WORLD_RES` → `INVENTORY` → `ENTITY_SPAWN`; taşımalar oturumda sırayla, commit'ten sonra `ITEM_UPDATE` + `ITEM_MOVE_RES`, görünür ekipman değişince dünya üzerinden `ENTITY_EQUIP` (kendine ve görüş alanındakilere); 10/s sel sınırı; bilinmeyen şablonlu satırlar saklanır, bir kez loglanır, istemciye gitmez | `tests/server/ItemServerTests.cpp`, `tests/db/PostgresItemTests.cpp`, `integration.items`, `integration.db.items` |
| Araçlar | `vo_admin give-item <karakter> <şablon>` / `list-items <karakter>` (UTF-8 argümanlar); botclient `items` senaryosu; yük testinde her üç bottan biri ara sıra kurala uygun kuşanıp çıkarır ve `Ok` bekler; paket `gamedata/` içerir | 200 bot yük testi, entegrasyon testleri |

Bilinçli olarak yok / sınırlar: dünyadaki bir karaktere `give-item` ile verilen eşya bir sonraki
girişte görünür (o arada o yuvaya taşıma ServerError alır, hiçbir şey bozulmaz); kafa/göğüs vb.
yuvalar kabloda ve veritabanında var ama M8'de hiçbir şey kabul etmez; istatistik/hasar yok.

## 12. M8 "Kuşan" — sanat / boru hattı durumu (2026-11-16)

| Konu | Ne var | Nasıl doğrulandı |
|---|---|---|
| Silahlar | 8 Tripo H3.1 silahı (iki kılıç, hançer, iki asa, topuz, iki elli balta, kalkan): ham dosyalar (≈1 M üçgen, 287 MB) repoya girmedi; `gen_weapons.py --prepare` 40k üçgene indirip gömülü dokuları bayt bayt koruyarak `tools/blender/source_meshes/weapons/` altına yazar (46 MB). Oyun meshi 1.5-3k üçgen, 512²/1024² renk + normal + ORM, asalarda kristal ışıması; hepsi "tutma çerçevesinde" (orijin = elin kapandığı yer, +Y = uç, +Z = ağız / yüz) | `WEAPON` satırları, `previews/weapons/weapons_sheet.png`, `content.weapons` testleri |
| İkonlar | 128² şeffaf ikon (`assets_src/ui/items/<icon>.png`); assetc `ui/items/*.png` için önceden çarpılmış alfalı RGBA8 `.vtex` yazar (ASSET_FORMATS.md §5.1) | `content.weapons` (biçim, mip, köşe alfası, önceden çarpılmışlık) |
| Elde tutma | `gamedata/attachments.toml`: tutuş × görünüm × el başına kemik + T·R·S (63 satır, `*` yedek satırlarıyla); matematik ve örnek ASSET_PIPELINE.md §4b | `content.weapons` (6 iskelette kemik adları, elde/yanda olma, kalkan yüzü dışa), dosyadan geri okunarak çizilen `hold_<look>.png` / `hold_<look>_hands.png` önizlemeleri (bekleme, koşunun iki ucu; ön ve yan) |

İstemci tarafı (silahı kemiğe bağlayıp çizmek, envanter penceresinde ikonlar) ayrı istemci ajanının
işi; bu turda istemci koduna dokunulmadı. Bilinen eksikler: karakter elleri açık (parmaklar katı),
sap avuçla uyluk arasında durur, yumruk yok; koşu klibi tek kolla salladığı için asa / balta kol
geride iken omuza / sırta yaklaşır; silaha özel bekleme / koşu klibi yok.

## 13. M8 "Kuşan" — istemci durumu (2026-11-16)

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Ağ / durum | `INVENTORY`, `ITEM_UPDATE`, `ITEM_MOVE_RES`, `ENTITY_EQUIP`, `CharSummary` / `ENTITY_SPAWN` ekipman alanları. `world::ClientInventory`: onaylı durum + cevap bekleyen taşımalar üstte yeniden oynatılır (anında görünür); `ITEM_UPDATE` en eski bekleyeni kapatır, başarısız `ITEM_MOVE_RES` onu siler (geri alma) ve sonuç koduna göre kısa Türkçe ileti; kurallara aykırı taşıma gönderilmez; saniyede en çok 8 istek; `LEAVE_WORLD_REQ` sonrası `ITEM_MOVE_REQ` `GameClient`'ta engellenir | `tests/client/InventoryTests.cpp` (iyimser gösterim, kapanış, geri alma, art arda taşımalar, kısıtlama, ENTITY_EQUIP), `items` akışı |
| Silahların çizimi | `gamedata/attachments.toml` yükleyicisi (`shared/game/Attachments`, sıkı, `*` yedeği), `world::placeWeapons`: dünya = karakter · kemik(t) · A. Silahlar sahibinin hemen ardından statik hatla çizilir: sahibiyle birlikte ayıklanır, gölge verir/alır, yakın kamerada sahibiyle solar, asa kristali ışır. Yerel, uzak oyuncular, seçim ve yaratma önizlemesi | ASSET_PIPELINE.md §4b çalışılmış örneği birim testinde (ve yanlış sıra `A · kemik` reddediliyor), Blender önizlemeleriyle (`hold_<look>.png`) ekran görüntüsü karşılaştırması |
| Yaratma / seçim ekranı | Önizleme figürü seçili sınıfın başlangıç donanımını tutar (sınıf değişince figür değişir); seçim listesi `CharSummary` silahlarını hem figürde hem kartta ikon olarak gösterir | `classes` akışı (2 ırk × 4 sınıf, 8 görüntü, `expectpreviewequip`), `items` akışı |
| Envanter penceresi | I ya da HUD düğmesi "Envanter (I)"; Esc önce pencereyi kapatır; modal değil, hareket tuşları çalışır. Solda siluet çevresinde Baş/Gövde/Bacak · Eller/Ayak/Takı (M8'de soluk) ve altta Sağ El / Sol El, sağda 7×4 çanta; sürükle-bırak (yasal hedefler parlar), çift / sağ tık hızlı kuşan-çıkar, ekran içinde kalan ipucu (ad, tür · yuva, saldırı/savunma, sınıflar, seviye; karşılanmayan kırmızı, açıklama) | `items` akışı 1280x720, 1600x900, 1600x900 @125% |
| Otomasyon | Pencere öğesi adlarıyla `dragon`, `hoveron`, `dblclickon`, `rclickon`; `scene inventory/itemmove/expectequip/expectbag/expectremoteequip/expectitemmessage/expectlocalweapons`, `camera front` / `camera at:<ad>`; durumlar `inv`, `invclosed`, `invloaded`, `itemsidle`, `remoteholds<a>_<b>` | `ScriptTests`, akışlar |
| Akışlar | `items` (savaşçı → envanter → ipucu → balta sürükle → kılıç ve kalkan çantada → kılıcı geri → yasak taşıma hem yerelde hem sunucuda reddedilir → seçime dön ve tekrar gir: aynı ekipman), `classes`, `two_clients_items` (B, A'nın silahlarını doğuşta ve baltaya geçişini görür) | `run-ui-flows.ps1` |
| Test güvenilirliği | Test programları (`tests/NoCrashDialogs.cpp`) ve betikli `vo_client` çökme / abort / assert pencerelerini açmaz, iletiyi stderr'e yazar | — |

Bilinen eksikler: eller açık (yumruk yok), asa / balta omuz yanında dik durur, kargan_2'de topuz önlüğe
kısmen girer (sanat sınırı; motor görüntüleri Blender önizlemeleriyle aynı). Envanter penceresi
sağa yaslı ve 1280 genişlikte karakterin bir kısmını örter. Silaha özel bekleme / koşu klibi yok;
`--debug-ghosts` hayaletleri silahsız.

## 14. M9 "İlk Kan" — sunucu + ortak kod durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_COMBAT.md` (protokol v3; metindeki "M8" = M9). Bu bölüm yalnızca sunucu ve
ortak yarıyı kapsar; istemci (canavarları çizmek, hedef çerçevesi, hasar sayıları, beceri düğmesi,
ölüm ekranı) ayrı bir ajanın işi. İstemciye yalnızca v3 ile derlenip çalışmaya devam etmesi için
dokunuldu: `entity_type` 2 olan doğuşlar şimdilik atlanır, yeni paketler yok sayılır.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Ortak kurallar | `shared/game/Combat.{h,cpp}`: sınıf tabloları, seviye + kuşanılan eşyalarla istatistik, hasar / kritik (zarlar dışarıdan verilir) / deneyim formülleri (2 seviyeden sonra seviye başına %20 eksik, 7+ seviyede 0), 4 beceri, seviye eğrisi (`80 × seviye^1.6`, 10'a yuvarlı, sınır 10) | `tests/game/CombatTests.cpp`: formüller, eğri, sınır, seçilmiş zarlarla hasar, 20 000 zarla sınırlar ve kritik oranı, ayar hedefi (başlangıç donanımlı 1. seviye her sınıf 1. seviye Kül Çömezi'ni ortalama 5-6 vuruşta öldürür, 9-12 vuruşuna dayanır) |
| Canavar verisi | `gamedata/monsters.toml` + `shared/game/Monsters.{h,cpp}` (eşya yükleyicisi kadar sıkı): 5 şablon (Kül Çömezi 1-2, Cüruf Yürüyen 3-4, Kor Bekçisi 6 elit, `scale` 1.25), köy meydanında 40 m güvenli bölge, 19 canavar: dört gevşek Kül Çömezi sürüsü 53-67 m (KB, B, G, GD), Cüruf Yürüyen çiftleri 110-120 m, Kor Bekçisi platonun kuzeybatı kenarında; kuzeydoğu yolu (`world_tour`) boş | yükleyici testleri; `content.Monster spawns:*`: derlenmiş bölge 1'de her doğma çemberi yürünebilir, meydandan yürüyerek ulaşılır, güvenli bölge dışında, akışların yollarından uzak |
| Protokol | `PROTOCOL_VERSION` 3, 0x0300–0x0310, `ENTER_WORLD_RES` / `ENTITY_SPAWN` sonuna v3 alanları; tanımsız eylem / neden / tür / sonuç / duruş ve bayrak bitleri reddedilir | gidiş-dönüş, kesilme / fazla bayt, kablo düzeni, rastgele baytla çözücü fuzz'ı, sunucu fuzz'ında savaş istekleri |
| Veritabanı | `0003_combat.sql`: `xp`, `hp`, `mp` (NULL = dolu, 0 = ölü çıkış); `saveCharacterState` (konum + seviye/xp/hp/mp tek UPDATE) bellek ve PostgreSQL arka uçlarında; `vo_admin set-level` | `tests/db/PostgresCombatTests.cpp` (sütunlar, kısıtlar, gidiş-dönüş, set-level, ölü çıkan karakter yeni sunucuda doğma noktasında %50), `integration.db.combat` |
| Sunucu | canavarlar dünyanın varlıkları (AOI, `ENTITY_MOVE`), yapay zekâ Idle → Chase → Return (+ Dead / Gone), yürünebilir hücrelerde düz çizgi ya da sınırlı ızgara araması, güvenli bölgeye girmez; otomatik saldırı, beceriler (büyü süresi, hareketle kesilme, ortak 500 ms bekleme), iyileştirme ve tehdidi, ölüm / 5 s sonra yeniden doğma, deneyim ve seviye atlama (anında kayıt), oturma, yenilenme, istatistik hız sınırları, kuşanma değişince `SELF_STATS`, ölüyken eşya taşıma `SlotBlocked`, 10/s ortak sel sınırı | `tests/server/world/CombatZoneTests.cpp` (sahte saat + sentetik arazi, sözleşmenin dünya kuralları), `tests/server/CombatServerTests.cpp` (süreç içi sunucu), `integration.combat` |
| Araçlar | botclient: v3, `combat` senaryosu (en yakın Kül Çömezi: otomatik saldırı + Ağır Darbe, COMBAT_EVENT / ENTITY_DEATH / XP_GAIN tutarlılığı; Kor Bekçisi'ne bile bile ölüm, erken RESPAWN_REQ `TooEarly`, 5 s sonra doğma noktasında %50); yük testinde her beş bottan biri savaşır, diğerleri güvenli bölgede gezer; paket `gamedata/monsters.toml` içerir | entegrasyon testleri, 200 bot yük testi |

Bilinçli olarak yok / sınırlar: görüş hattı yok (yalnızca menzil); Köz Oku'nun hasarı büyü bitince
uygulanır (mermi yalnızca görüntü); canavarlar birbirini itmez, aynı oyuncunun çevresinde üst üste
durabilir; sürü yardımı ("biri saldırılınca hepsi gelir") yok; ölümde deneyim kaybı yok; görüş
alanında kimse yokken canavarlar dolaşmaz; `set-level` yalnızca dünyada olmayan karakterler içindir
(dünyadaki karakter bir sonraki kaydında üzerine yazar).

## 15. M9 "İlk Kan" — sanat durumu (2026-11-16)

| Konu | Ne var | Nasıl doğrulandı |
|---|---|---|
| Canavarlar | Üç insansı Tripo H3.1 canavarı oyuncu boru hattından geçti (`tools/blender/gen_monsters.py`, `gen_character.py`'nin yardımcılarını kullanır): `ash_thrall` (Kül Çömezi, 1.50 m), `slag_walker` (Cüruf Yürüyen, 2.00 m), `ember_warden` (Kor Bekçisi, 2.05 m; oyunda 1.25× çizilir). Oyuncularla aynı 59 kemikli iskelet, 30k üçgen, 1024² renk + normal + ORM + **ışıma** haritası (lav çatlakları, gözler, göğüs kristali). Ham dosyalar (≈44 MB) repoya girmedi; `--prepare` 50k üçgene indirip gömülü dokuları bayt bayt korur (`source_meshes/monsters/`, 17 MB) | `STATS` / `SLIDE` satırları, `previews/monsters/*_bind_*.png` ve `*_clips_sheet.png`, `content.*monster*` testleri |
| Ağırlıklar | Cürufun dev ön kolları / bacak taşları katı (lowerarm / calf), omuz kayaları katı parça (clavicle 0.6 / upperarm 0.4; `rigid` artık `weights={...}` alır, oyuncu çıktıları değişmedi); Bekçinin omuzlukları katı (0.5 / 0.5), ön ve arka etek panelleri `cloth_front` / `cloth_back` yardımcı kemiklerinde | `--debug` ağırlık görüntüleri (kılıç saldırısı, koşu, ölüm, yuvarlanma), önizleme sayfaları |
| Klipler | Yalnızca savaş seti: idle, walk, run, punch_jab, punch_cross, spell_enter/idle/shoot/exit, hit_chest, death (`monsters/<model>/<clip>.vanim`). Yürüyüş / koşu adım hızları her klibin `moveSpeed`'inde (istemci: hız = sunucu move_speed / moveSpeed) ve `assets_src/monsters/monsters_meta.json`'da: Kül Çömezi 1.18 / 4.87 m/s, Cüruf Yürüyen 1.65 / 6.84 m/s, Kor Bekçisi 1.47 / 6.08 m/s (1.25× çizilen Bekçide ayak hızı da 1.25× büyür) | `content` testleri: tam klip seti, kesintisiz döngüler, patlayan köşe yok, basan ayak kaymıyor |
| Işıma | Skinned yol ışımayı destekliyor (`drawCharacters` → `bindSubmesh`, aynı piksel gölgelendirici); motor görüntüsü `vo_client --debug-model monsters/ember_warden.vmesh` ile alındı | ekran görüntüsü (bağlama pozu) |
| Efektler | `effects/ember_bolt.vmesh` (Köz Oku mermisi, 200 üçgen, 0.5 m, +Z'ye bakar, ışımalı); dört 64² beceri ikonu `ui/skills/{agir_darbe,cifte_kesik,koz_oku,soluk_gunes}.vtex` (assetc kuralı `ui/skills/`'e genişletildi: önceden çarpılmış RGBA8) | `content` testleri, `previews/effects/skills.png` (64 ve 48 px, koyu arka plan) |

Bilinen eksikler: Cüruf Yürüyen'in başı omuzların arasına gömük, Head kemiği omuz kayasının içinde
kalır (baş gövdeyle döner); kolu yukarı kaldıran büyü kliplerinde omuz kayası kolu yarı yarıya izler,
kol kayaya biraz girer. Bekçinin yan etek plakaları uyluğu izler (koşuda uyluğa yapışık görünür). Ölüm klibinin sonunda (yatarken) kütüphane omurgası ince mankene göre olduğundan Bekçinin omuzlukları ve Cürufun omuz kayaları zeminin ~0.3 m altına girer (arazi örter; test sınırı −0.35 m).
Animasyonlu klipler motorda değil yalnızca Blender'da görüldü (`--debug-model` kimlik iskeleti kullanır).

## 16. M9 "İlk Kan" — istemci durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_COMBAT.md` §5 (protokol v3). Ayrıntılar `docs/CLIENT_RENDERING.md` §10'da.
Sunucu, `shared/net`, `gamedata/` ve veritabanına dokunulmadı.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Canavarlar | `entity_type` 2 doğuşları artık atlanmıyor: `RemoteEntity` hp / max_hp / şablon / durum bitleri taşır; model ve ölçek `gamedata/monsters.toml`'dan (sunucunun yükleyicisi), `monsters/<model>.vmesh` + var olan savaş klipleri (`AssetCache::requestMonster`), ışımalı skinned yol, `makeWorld(…, scale)`; yürü / koş klibi hızı `sunucu hızı / (moveSpeed × ölçek)`. Ad etiketi "<ad> · Sv. N" (elit: "· Elit"), seviye farkına göre gri / beyaz / sarı / turuncu / kırmızı; ceset despawn'a kadar ölüm pozunda kalır. HUD oyuncu sayısı ve `remotes<N>` yalnızca oyuncuları sayar | `CombatViewTests` (renk eşikleri, doğuş alanları, ölçekli adım hızı), tüm akışların ekran görüntüleri |
| Hedefleme | Sol tık: varlık başına dikey kapsüle ışın (arazi önündeyse sayılmaz) → seçer; boşa tık yürür. Tab: 40 m içindeki en yakın canlı canavarlar sırayla (UI odağından önce yakalanır). Esc: önce envanter, sonra hedef, sonra menü. Değişiklikte `TARGET_REQ`; hedef çerçevesi üst ortada (sol panel ile sağ düğmeler arasına sığar), hedefin altında halka | birim testleri (kapsül, en yakın, Tab sırası), `combat` akışı (Esc / Tab adımları) |
| Saldırı | F, canavara çift tık ya da sürüklemeden sağ tık (6 px / 0.45 s; sağ sürükleme kamerayı döndürmeye devam eder): `ATTACK_REQ`, menzil dışındaysa karakter koşarak menzile girer (yakın dövüş 1.9 m, asa 12.5 m); takip sırasında sunucunun tek seferlik `OutOfRange` bildirimi gösterilmez; WASD / yere tık takibi bırakır, otomatik saldırı sunucuda kurulu kalır (sözleşme §3.1). Menzilde karakter hedefe döner | `combat`, `combat_mage`, `two_clients_combat` |
| Beceri | 1 tuşu: mana / bekleme / büyü / hedef türü yerelde denetlenir (Türkçe ileti), gerekirse menzile yürüyüp `SKILL_REQ`. Alt ortada ikonlu düğme (`world.skill`, tıklanabilir): "1" ipucu, saat yönünde bekleme taraması + saniye (500 ms ortak bekleme dahil), manasızken / ölüyken soluk; üstünde büyü çubuğu; ACTION_FAIL nedenleri kısa Türkçe iletiler ("Menzil dışında", "Yeterli mana yok", "Güvenli bölgede saldırılmaz", "Henüz hazır değil", …) | `SkillTimers` testleri (ortak bekleme, Çifte Kesik'in ikinci vuruşu, kesilen büyü bedava, tamamlanınca bekleme), `combat_mage` (büyü çubuğu, Köz Oku), `death` (TooEarly iletisi) |
| Geri bildirim | Uçan sayılar (beyaz normal, büyük sarı kritik "N!", yeşil "+N" iyileştirme, kırmızı alınan hasar), hasar alan / hedeflenen canavar ve oyuncuların başında can çubuğu, vurulunca kırmızı kenar parlaması. Saldıranın klibi ana el tutuşuna göre (`sword_attack`, asa `spell_shoot`, çıplak el ve canavarlar `punch_jab`/`punch_cross` sırayla), kurbanda `hit_chest` (saldırı / büyü ortasında değilse), `ENTITY_DEATH`'te ölüm klibi (son karede kalır). Köz Oku: `effects/ember_bolt.vmesh` 0.25 s'de hedefe uçar (yalnızca görüntü; `WorldRenderer::addEffectMesh` + ekran uzayında kor parıltısı) | birim testleri (sayı türü / stili / ömrü, saldırı klibi seçimi, Animator eylemleri), ekran görüntüleri |
| Ölüm / doğma | Ölünce: ölüm klibi, hareket ve eşya taşıma kilitli (`InventoryWindow::setLocked`, `requestItemMove`), karartılmış "Öldün" ekranı, geri sayım, 5 s sonra açılan "Yeniden Doğ" (`RESPAWN_REQ`); `TooEarly` düğmeyi 1 s kapatır ve iletiyi gösterir; `Ok`: `Prediction::teleport` (karışım yok, eski düzeltmeler bayat), kamera sıfırlanır, yeni hp / mp. Diğer oyuncuların ölümü / dirilişi `ENTITY_DEATH` / `ENTITY_STATE` ile | `death` akışı (1280x720 ve 1024x640 @150 %: erken istek reddi, kilitli eşya, köyde %50 can) |
| Seviye atlama | "Seviye N!" altın yazı + yumuşak bant + "Can ve mana yenilendi", karakterin çevresinde genişleyen altın halka ve kısa sıcak parlama, HUD sınıf satırı ve envanterin seviye kuralı yenilenir; `XP_GAIN` deneyim çubuğunu sürer | `levelup` akışı (1920x1080, `scene hunt Kül Çömezi 2`) |
| Duruş | X `STANCE_REQ` gönderir; poz sunucunun `ENTITY_STATE` bit 2'sini izler (kenar tetikli), yer oturuşu (`sit_ground_*`, yoksa sandalye klipleri); diğer oyuncular da aynı şekilde, oturarak doğan oyuncu doğrudan oturur | Animator testleri, `polish` (oturma artık sunucudan), `levelup` (dinlenme görüntüsü) |
| Güvenli bölge | Bölgenin `safe_zones` çemberinde "● Güvenli Bölge" göstergesi (envanter düğmesinin altında) | akışlarda `safezone` / `outsidesafe` durumları |
| Otomasyon | Sahne komutları `target`, `tab`, `attack`, `attackstop`, `cast`, `respawn`, `stance`, `walktomonster`, `hunt`, `expecthp`, `expecttargethp`, `expectlevel`, `expectcombatmessage`, `combatlog`, `camera side`; savaş durumları ve sayaçları (`kills<N>`, `seenhits<N>`, `healed<N>` …); ayrıştırıcı hata iletileri birim testli. Koşucu: koşu / akış başına `TimeoutSec` | `CombatViewTests` (komutlar ve hataları) |
| Akışlar | `combat` (1280x720, 1600x900 @125 %), `combat_mage`, `death` (1280x720, 1024x640 @150 %), `levelup` (1920x1080), `two_clients_combat` (B, A'nın dövüşünü — can çubuğu, vuruşlar, ölüm — görür ve A'yı Soluk Güneş ile iyileştirir) | `run-ui-flows.ps1` |

Bilinen eksikler / sınırlar: Köz Oku'nun hasar sayısı mermi varmadan görünür (sunucu hasarı büyü
bitince uygular); mermi el kemiğinden değil karakterin önünden ~1.35 m yükseklikten çıkar; yerel
karakterin hedefe dönüşü yalnızca görüntüdür (yön sunucuya gitmez, başkaları eski yönü görür);
koşarken gelen saldırı klibi tüm vücudu oynatır (ayaklar kısa süre kayar); canavarlar birbirinin
içine girebilir (sunucu itmez); ölü oyuncuların can çubuğu gösterilmez; hedef çerçevesi 1024x640
@150 %'de daralır.

## 17. M10 "Ganimet" — sunucu + ortak kod durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_LOOT.md` (protokol v4). Bu bölüm yalnızca sunucu ve ortak yarıyı kapsar;
istemci (ganimet kesesini yerde çizmek, ganimet penceresi, Akçe göstergesi, yığın sayıları) ayrı bir
ajanın işi. İstemcide tek değişiklik: dünya yüklenirken ve envanter çizilirken diskte olmayan eşya
ikonları artık istenmiyor (`world::itemIconAvailable`); önceden eksik ikon `[ERROR]` günlüğüne
düşüyor ve UI akışlarını düşürüyordu. Bunun dışında istemci v4 ile aynen çalışıyor: `entity_type` 3
doğuşları "bilinmeyen tür" olarak atlanıyor, yeni paketler (0x0500–0x0507) yok sayılıyor, eksik
ikon boş yuva olarak çiziliyor.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Eşya verisi | `gamedata/items.toml`: yeni tür `material` (yuvası yok; slot / sınıf / seviye / saldırı / savunma / model / tutuş yasak), isteğe bağlı `max_stack` (silah ve kalkanda 1), `value`, `rarity`. 1–8 değişmedi; yeni şablonlar 9 Kül Tozu (20'lik yığın), 10 Cüruf Parçası (20), 11 Kor Çekirdeği (5, nadir), 12 Çömez Bıçağı, 13 Kömürlü Kılıç, 14 Cüruf Topuzu, 15 Bekçi Kalkanı (modeller var olanlardan, ikon adları sanat ajanıyla sabit) | `tests/game/LootTests.cpp` (şablonlar, katı doğrulama), `content.every item…` (kaynak PNG'si gelmemiş ikon yalnızca uyarı) |
| Yığınlar | Çantada aynı malzemenin dolmamış yığınına taşıma birleştirir (hedef büyür, kaynak küçülür ya da silinir; dolu yığın eskisi gibi takas); ganimet ve `give-item` önce var olan yığınlara, sonra en küçük boş yuvalara koyar (`planItemAdd`). Bölme yok | birim testleri, `LootServerTests` (birleştirme, satır silinmesi) |
| Ganimet tabloları | `gamedata/loot.toml` + `shared/game/Loot.{h,cpp}` (eşya / canavar yükleyicileri kadar sıkı, canavar ve eşya kimlikleri denetlenir); zar dışarıdan verilir. Kül Çömezi 3–9 Akçe, Kül Tozu %45 (1–2), Çömez Bıçağı %4, Kömürlü Kılıç %4; Cüruf Yürüyen 12–25 Akçe, Cüruf Parçası %50 (1–2), Cüruf Topuzu %5; Kor Bekçisi 120–180 Akçe, Kor Çekirdeği %100 (1–2), Bekçi Kalkanı %35, Burç Kılıcı %15, Kırağı Asası %15 | yükleyici testleri, 20 000 zarla dağılım ve ortalamalar |
| Protokol | v4, 0x0500–0x0507 (`LOOT_OPEN_REQ/RES`, `LOOT_CONTENTS`, `LOOT_TAKE_REQ/RES`, `LOOT_CLOSE_REQ`, `LOOT_CLOSED`, `COINS`), `entity_type` 3, durum bitleri 0x08 LootLocked / 0x10 LootMine; girişte `INVENTORY`'den hemen sonra `COINS` | gidiş-dönüş, kablo düzeni, reddedilen değerler, çözücü fuzz'ı, sunucu fuzz'ında ganimet istekleri |
| Dünya | Ölen canavarın tablosu boş değilse cesedin yerinde bir ganimet varlığı (AOI ile doğar / kaybolur); sahibi en çok hasar veren karakter (eşitlikte önce dövüşe giren, yoksa son vuruşu yapan), 30 s yalnız onun, sonra herkesin; 90 s sonra ya da boşalınca kaybolur. Açma / içerik / izleyiciler (6 m'de, ölünce kapanır), tek giriş / yalnız Akçe / hepsini alma; alınan birimler rezerve edilip oturuma verilir, sığmayan geri döner. Kopma: yazılmaya başlamamış rezervler geri döner, yazılmakta olan biter; rezervli kese kaybolmaz | `tests/server/world/LootZoneTests.cpp` (sahte saat: sahiplik penceresi, kaybolma, menzil, ölü yağmacı, iki yağmacı yarışı, kopma, 8 eş zamanlı alma sınırı) |
| Oturum | Ganimet istekleri saniyede 10 (ortak) sel sınırı; alma eşya taşımalarıyla aynı sırada, işlenmiş envantere göre planlanır, tek işlemde yazılır, ITEM_UPDATE / COINS / LOOT_TAKE_RES işlemden sonra; dolu çanta `BagFull`, kese sınırı `PurseFull` | `tests/server/LootServerTests.cpp` (süreç içi sunucu: tam akış, kalıcılık, dolu çanta, birleştirme, yazma sırasında ve öncesinde kopma, sel / bozuk / durum dışı) |
| Veritabanı | `0004_loot.sql`: `characters.coins bigint NOT NULL DEFAULT 0`, CHECK 0..1 000 000 000 (0001–0003'e dokunulmadı). `applyItemWrite` (beklenen miktarla yığın güncelleme / silme, ekleme, Akçe artışı `RETURNING coins` ile; hep ya da hiç) bellek ve PostgreSQL arka uçlarında; `vo_admin give-item <karakter> <şablon> [adet]` (önce yığınlar, hep ya da hiç), `vo_admin set-coins <karakter> <miktar>`, `list-items` Akçe'yi gösterir | `tests/db/PostgresLootTests.cpp` (sütun ve kısıt, hep ya da hiç, set-coins, give-items, PostgreSQL üstünde sunucu yeniden başlatma sonrası yığın ve bakiye), `integration.db.loot` |
| Araçlar | botclient: v4 el sıkışması, `loot` senaryosu (Kül Çömezi öldür, kendi kesesini aç, hepsini al; LOOT_CONTENTS'e göre ITEM_UPDATE / COINS / LOOT_TAKE_RES tutarlılığı, kese kaybolur; `--phase check`: aynı bakiye ve yığınlarla tekrar giriş); yük testinde savaşan botlar kendi öldürdüklerinin ganimetini toplar; paket `gamedata/loot.toml` içerir | `integration.loot`, `integration.db.loot`, 200 bot yük testi |

Yük testi (Release, 200 bot, 60 s, 40'ı savaşıp ganimet topluyor): 8.7 s'de hepsi dünyada,
0 düzeltme, 0 kopma; tik ortalaması 1.6–1.7 ms, p99 ≤ 2.75 ms, 0 yavaş / atlanan tik; istemci başına
74 KB/s; PING p50 0.27 ms, p99 6.5 ms; 26 öldürme, 26 ganimet alma (174 Akçe), 0 hatalı cevap.

Bilinçli olarak yok / sınırlar: tüccar yok (`value` yalnızca bilgi); yığın bölme yok; ganimet
keseleri veritabanında tutulmaz (sunucu yeniden başlarsa toplanmamışlar kaybolur); bağlantı tam
`COMMIT` sırasında koparsa ("in doubt") alma `ServerError` sayılır ve birimler keseye döner, oysa
işlem yazılmış olabilir (sonraki giriş doğrusunu yükler); dünyadaki bir karaktere verilen
`give-item` / `set-coins` bir sonraki girişte görünür; geri dönen birimlerin `LOOT_CONTENTS`'i
`LOOT_TAKE_RES`'ten önce de gelebilir; sahiplik yalnızca tek karaktere (grup / paylaşım yok).

## 18. M10 "Ganimet" — sanat durumu (2026-11-16)

Hepsi `tools/blender/gen_loot.py` ile tamamen prosedürel (sabit tohumlu numpy, Blender prosedürel
gölgelendirici düğümleri; indirme yok, üretken model yok). `build_all.ps1`'de `gen_effects.py`'den
sonra çalışır. Ayrıntılar ASSET_PIPELINE.md §4d.

| Konu | Ne var | Nasıl doğrulandı |
|---|---|---|
| Ganimet torbası | `props/loot_bag.vmesh`: yerde duran, boğazı iple bağlı kanvas çuval; 1 380 üçgen, 0.46 × 0.45 × 0.36 m, orijin tabanın ortasında (5 mm gömük). 512² renk + normal + ORM + **ışıma**: ağzındaki kor yatağı, iç çeperde sıcak yansıma, düğümdeki tunç tokanın taşı (güç 3). Dikişler, yama, kıvrımlar normal haritasında; önüne iki sikke dökülmüş | `PROP loot_bag` satırı, `previews/loot/loot_bag.png` (çimen katmanı üstünde üç açı; üçüncüsü oyun kamerası uzaklığında: torba çimende sıcak bir nokta olarak seçiliyor), `content.loot bag…` (bütçe, boyut, zeminde taban, dört harita, ışıma oranı) |
| Malzeme ikonları | `ui/items/ash_dust` (kömür parçalı kül yığını, sönük korlar), `slag_shard` (turuncu çatlaklı koyu cüruf kıymığı), `ember_core` (koyu taş yuvada parlayan turuncu kristal kümesi) | `previews/loot/icons.png` (64 ve 48 px, HUD rengi), `content.loot icons…` |
| Para | `ui/items/coin`: biri gümüş beş aşınmış sikkelik deste ve iki tek sikke (Akçe bakiyesi için) | aynı |
| Silah çeşitleri | Kaynak silahın aynı mesh ve pozuyla yeniden çizildi, gölgelendirici değişti: `dagger_thrall` (külden soluk ağız, kırmızı parlayan damarlar; hunter'dan), `sword_charred` (kararmış ağız, iki keskin kenar kor gibi parlıyor; soldier'dan), `mace_slag` (altın güneş başı yerine parlak çatlaklı cüruf; sun'dan), `shield_warden` (kararmış tahtalar, yüzde parlayan kor arması; oak'tan) | 48 px'te kaynaklarından açıkça ayrılıyor (önizleme sayfasında her biri kaynağının altında); `content.weapon icon variants…` (ortalama renk farkı > 12/255) |
| Boru hattı | `build-assets.ps1` gerekli çıktılarına `props/loot_bag.vmesh`, `ui/items/coin.vtex`, `ui/items/sword_charred.vtex` eklendi; assetc kuralı değişmedi (ikonlar mevcut `ui/items/` kuralıyla önceden çarpılmış RGBA8, 8 mip) | `build-assets.ps1 -Config Debug`; derlenmiş eski 555 dosya bayt bayt aynı |
| Belirlenimcilik | İkinci çalıştırma ve ikonlar silinip yeniden üretilince bütün çıktılar (önizlemeler dahil) bayt bayt aynı | sha256 karşılaştırması |

Bilinen eksikler: torba 24 dilimli; yakın kamerada silüette hafif köşeler görülür. Kül yığını 48 px'te
gri bir tümsek olarak okunuyor (kömür parçaları ve korlar küçük). Silah çeşitlerinin yalnızca ikonları
var: oyunda elde çizilecek ayrı mesh / doku yok (gerekirse istemci kaynak silahın mesh'ini kullanır ya da
ayrı bir iş olarak çeşit dokuları üretilir). İstemci tarafı (torbayı dünyada çizmek, ikonları envanterde
göstermek, Akçe göstergesi) ayrı ajanın işi; `gamedata/` ve istemci koduna dokunulmadı.

## 19. M10 "Ganimet" — istemci durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_LOOT.md` §9 (protokol v4). Ayrıntılar `docs/CLIENT_RENDERING.md` §11'de.
Sunucu, `shared/net`, `gamedata/`, `db/` ve varlıklara dokunulmadı.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Dünyada keseler | `entity_type` 3 artık atlanmıyor: `RemoteEntity::isLoot()`, `WorldModel::lootCount()` (oyuncu / canavar sayılarına girmez). `props/loot_bag.vmesh` arazide 1.3× (`addEffectMesh`, renk çarpanı eklendi, karede en çok 48), kimliğe göre döndürülmüş, doğarken yere oturur; gölge lekesi ve yer halkası. Durum bitlerine göre (`lootLabel`): **senin** altın etiket + nefes alan altın halka, **kilitli** (başkasının) soluk gri etiket, karartılmış kese, silik gri halka, **serbest** parşömen rengi etiket + ılık halka. Etiket "Ganimet"; üstüne gelince / açıkken "Ganimet · <canavar>" ve "Senin ganimetin" / "Başkasının ganimeti" | `LootViewTests` (bit → durum, üç ayrı renk, yalnız seninki nabız atar, kilitli karartılır; dünya modeli tür 3), `loot` ve `two_clients_loot` ekran görüntüleri |
| Açma | Sol tık, sürüklemesiz sağ tık ya da **E** (12 m içindeki en yakın kilitsiz kese): 2.5 m'den uzaksa saldırıdaki gibi önce yürür (WASD / yere tık iptal), 2.2 m'de `LOOT_OPEN_REQ`. Kese hedeflenmez. Sonuçlar §9'daki Türkçe iletiler ("Bu ganimet henüz başkasının", "Çok uzaktasın", "Çantan dolu", …); yakında kese yoksa "Yakında ganimet yok". Ganimet istekleri saniyede en çok 8 | birim testleri (`nearestLoot`, `lootResultText` hepsi farklı ve sözleşmedeki metin), akışlar |
| Ganimet penceresi | `LootWindow` (modal değil, solda HUD panelinin altında; karakteri ve sağdaki envanteri örtmez, 1024x640 @150 %'de de): alt başlık "<canavar> · Sv. N" + "Senin / Başkasının / Herkesin"; önce Akçe satırı (sikke ikonu), sonra girdiler (ikon, nadirlik renginde ad, "×N", ipucu). Satıra tık = o girdiyi al (Akçe 0xFE), "Hepsini Al · E" / E = 0xFF. `LOOT_CONTENTS` satırları yeniler; `LOOT_CLOSED`, kese kaybolunca, 4.5 m'den uzaklaşınca (`LOOT_CLOSE_REQ`), ölünce, dünyadan çıkınca ve Esc ile kapanır. **Esc sırası**: ganimet penceresi → envanter → hedef → menü | `LootWindowModel` testleri (sıra, başka kesenin içeriği yok sayılır, alma seq'leri), `escapeAction` testi, `loot` akışı (Esc adımları) |
| Envanter ve almalar | `ClientInventory::beginTake/endTake`: sunucu taşımaları ve almaları istek sırasıyla cevaplar; bir almanın `ITEM_UPDATE`'leri ondan sonra gönderilen bir taşımayı kapatmaz (yoksa bekleyen taşıma bir an geri alınmış görünürdü) | birim testi (yolda birleştirme + alma) |
| Yığınlar, Akçe, ipuçları | Çanta yuvasında miktar sağ altta (> 1 ise); aynı malzemenin dolmamış yığınına sürükleme olağan taşıma (ortak kural birleştirmeyi öngörür, sunucu karar verir). `COINS` envanter altındaki bakiyeyi sürer ("Akçe", sikke ikonu, 1.234.567 biçimi). Ortak ipucu (`itemTipLines`, envanter ve ganimet penceresi): nadirlik renginde ad, "Malzeme · Sıradan", "Adet: N / en çok", saldırı / savunma, yalnız silah / kalkanda sınıf ve seviye (karşılanmayan kırmızı), "Değer: N Akçe (yığın M Akçe)", açıklama | `formatThousands`, `itemTipLines` testleri, `loot` akışı (yığın ipucu ekran görüntüsü) |
| Alma geri bildirimi | Her `ITEM_UPDATE`'te onaylı envanterin önce / sonra farkı (`inventoryGains`: taşıma ve birleştirme kazanç sayılmaz) ve her `COINS.gained > 0` sol altta kısa bir kayıt satırı: "Kül Tozu x2 alındı" (nadirlik renginde), "+7 Akçe" (altın); pencerelerin üstünde, en çok 5 satır, 3.6 s | `inventoryGains` / `pickupText` testleri, ekran görüntüleri |
| Eski "yok say" kısımları | Tür 3 doğuşları ve 0x0500–0x0507 artık işleniyor; eksik ikon toleransı (`itemIconAvailable`) duruyor, sikke ikonu da aynı şekilde (`SceneResources::uiImage`) | tüm akışlar |
| Otomasyon | Sahne komutları `lootnearest [any]`, `walktoloot`, `takeall`, `take <n>` / `take coins`, `lootclose`, `expectcoins`, `expectitem`, `expectstack`, `expectlootrows`, `expectlootmessage`, `lootfarm <canavar> <şablon> <adet>` / `lootfarm <canavar> rows <n>` (av döngüsü + kendi / serbest keseleri toplama), `movestack`, `lootlog`; durumlar `lootvisible/noloot`, `lootmine/lootlocked/lootfree`, `lootwin`, `lootcontents`, `loottaking/lootidle`, `farmdone`, `remotesitting` ve sayaçlar (`loottakes<N>`, `lootrefused<N>`, `pickups<N>`, `coingains<N>`, `lootdespawns<N>` …); ayrıştırıcı hata iletileri birim testli | `LootViewTests` |
| Akışlar | `loot` (1280x720 ve 1024x640 @150 %: yerde altın etiketli kese, E ile pencere, Esc, envanter açıkken hepsini al, kayıt satırları ve bakiye, sonra iki Kül Tozu alımı tek yığında: sayı ve ipucu), `two_clients_loot` (B, A'nın kesesini kilitli görür, açması "henüz başkasının" ile reddedilir; B oturarak işaret verir, A hepsini alır, kese ikisinde de kaybolur), `loot_rich` (yalnızca PostgreSQL test veritabanı varsa, kendi `vo_it_*` şemasında; yoksa "atlandı"): `vo_admin` ile 10. seviye, 1.234.567 Akçe, üç nadir Kor Çekirdeği ve seyrek Bekçi Kalkanı verilmiş savaşçı; gruplu bakiye, nadirlik renkleri ve ipuçları, sonra `lootfarm … rows 2` ile en az iki satırlı kese açık kalır, hepsini al. (Başlangıç donanımlı 10. seviye savaşçı elit Kor Bekçisi'ni yenemiyor: denendi, öldü; nadir eşya bu yüzden tohumlamadan geliyor) | `run-ui-flows.ps1` |

Bilinen eksikler / sınırlar: kese görüntüsü statik (gölge atmaz, yalnız leke); üstüne gelme etiketi
kese ile oyuncunun adı aynı hizaya düşünce üst üste binebilir; 1024x640 @150 %'de 3'ten çok satırlı
bir kese penceresi alt soldaki kayıt satırlarına kadar uzanır; envanter penceresi 1280 genişlikte
karakteri hâlâ kısmen örter (M8); `loot_rich` test veritabanı olmadan çalışmaz (atlanır); Kor Bekçisi'nden düşen nadir eşyalı bir kese akışlarda hiç görülmedi.

## 20. M11 "Künye" — sunucu + ortak kod durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_STATS.md` (protokol v5). Bu bölüm sunucu ve ortak yarıyı kapsar; Künye
penceresi (nitelikler, "+" düğmeleri, önizleme, onay) istemci ajanının işi (sözleşme §7). İstemcide
yalnız şunlar değişti: v5 el sıkışması (ortak sabit), `ITEM_MOVE_RES(AttributeTooLow)` için Türkçe
ileti ("Bu eşya için niteliklerin yetersiz."), ve otomasyondaki `scene hunt` (aşağıda "Kararsız
akışlar"). `CHAR_STATS` ve `STAT_ALLOCATE_RES` dünya sahnesinde bilinmeyen paket olarak yok
sayılıyor; can / mana göstergeleri eskisi gibi `SELF_STATS` / `COMBAT_EVENT` ile; ıska (`kind` 3)
sayı göstermeden geçiyor.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Nitelikler | Beş nitelik: Güç, Çeviklik, Dayanıklılık, Zekâ, İrade. Sınıf başlangıç dağılımı (toplam 35): Savaşçı 12/6/11/3/3, Haydut 6/13/8/4/4, Büyücü 3/5/6/13/8, Şifacı 5/3/9/8/10. Saldırı niteliği: Savaşçı ve Şifacı Güç, Haydut Çeviklik, Büyücü Zekâ (büyü gücü) | `tests/game/CombatTests.cpp` |
| Puanlar | Her seviye 3 puan (10. seviyede 27); harcanan puan kalıcı, geri almanın tek yolu `vo_admin reset-stats`. Nitelik tavanı 255 (başlangıç + harcanan). M11'den önceki karakterler seviyelerinin bütün puanlarını harcanmamış olarak alır (7. seviye: 18) | birim, `PostgresStatsTests` (0005 bir M10 veritabanına uygulanıyor) |
| Türetilmiş değerler | Tek saf fonksiyon `game::deriveStats(sınıf, seviye, harcanan, ekipman)`: can = taban + seviye + 5×DAY; mana = taban + seviye + 3×İRA + ZEK; saldırı = taban + 2×seviye + silah×(1 + A/20) + A/3 (A = saldırı niteliği); savunma = taban + seviye + ekipman + DAY/5; kritik = sınıf tabanı + %0,5×ÇEV (en çok %50); kaçınma = %0,4×ÇEV (en çok %25); can yenilenmesi %2×(1 + DAY/100), mana %3×(1 + İRA/100); Soluk Güneş hedefin canının %19×(1 + (ZEK + İRA)/60)'ı. Sınıf tabloları, başlangıç dağılımıyla 1. ve (harcanmamış) 10. seviyede v4 sayılarını birebir verecek şekilde ayarlandı | ayar tablosu testi (aşağıda), bölge ve sunucu testleri |
| Kaçınma | Canavarın vuruşu önce oyuncunun kaçınma şansını zarlar; kaçınılan vuruş 0 hasar, `COMBAT_EVENT kind 3` (Miss), can değişmez, savaşta sayılır, oturan kalkar | `CombatZoneTests` (%25 kaçınmada ~%25 ıska, olaylar gözlemcilere de gider) |
| Eşyalar | İsteğe bağlı nitelik bonusları (`strength` … `spirit`, 0..50) ve gereksinimler (`req_*`, 0..255, temel değere göre; yeni sonuç `AttributeTooLow` = 9). Cüruf Topuzu İrade +2, Bekçi Kalkanı Dayanıklılık +3; gereksinimli eşya henüz yok | `ItemsTests` |
| Protokol | v5: `CHAR_STATS` 0x0600 (seviye, 5×(temel, bonus), harcanmamış, can/mana, saldırı, savunma, kritik ‰, kaçınma ‰, yenilenmeler ×10, saldırı aralığı, iyileştirme ‰; 48 bayt), `STAT_ALLOCATE_REQ` 0x0601 (seq, 5 nitelik için puan), `STAT_ALLOCATE_RES` 0x0602 (seq, sonuç, harcanmamış). `SELF_STATS` aynen kaldı (hızlı can/mana kanalı). Girişte sıra: ENTER_WORLD_RES, INVENTORY, COINS, CHAR_STATS, sonra doğuşlar | gidiş-dönüş, düzen, reddedilen değerler, fuzz |
| Oturum | İstekler tek tek, işlenmiş dağılıma ve dünyanın bildirdiği canlı seviyeye göre karar verilir (InvalidAmount → NotEnoughPoints → AttributeCap), veritabanına yazılır, cevap yazmadan sonra; ardından `StatsCommand` ile dünyaya; kuyrukta en çok 4, saniyede 5 istek | `tests/server/StatsServerTests.cpp` |
| Dünya | Girişte, seviye atlayınca, ekipman bonusu değişince ve yeni dağılımla `CHAR_STATS` (tikte en çok bir, yalnız sahibine); can/mana dolmaz, maksimum küçülürse kırpılır; gözlemciler yeni azami canı `ENTITY_STATS` ile görür | `CombatZoneTests` |
| Veritabanı | `0005_stats.sql`: `stat_strength`, `stat_dexterity`, `stat_vitality`, `stat_intelligence`, `stat_spirit` (smallint, varsayılan 0, 0..255) ve toplam ≤ 3×(seviye−1) kısıtı. `allocateStats` iki arka uçta (tek UPDATE … RETURNING; saklanan seviye kuralı bellekte de). `set-level` daha düşük seviyeye sığmayan puanları sıfırlar. 0001–0004'e dokunulmadı; gerçek `vedatonline` veritabanına uygulanmadı (müdürün işi) | `PostgresStatsTests`, `AccountServiceTests` (iki arka uç), `integration.db.stats` |
| Araçlar | `vo_admin reset-stats <karakter>`, `vo_admin show-character <karakter>`; botclient v5, `stats` senaryosu (giriş, ret, 2. seviyeye av — ölünce yeniden doğar —, puan harcama, CHAR_STATS / SELF_STATS / verilen hasar tutarlılığı, yeniden giriş, sel; veritabanıyla `set-level 10`, 24 puan, `reset-stats`); yük testinde savaşan botlar seviye atlayınca puanlarını harcar; sunucuya `world.seed` (test için sabit RNG tohumu) | `integration.stats`, `integration.db.stats`, yük testi |

Ayar tablosu (testte ortalama zarlarla düz dövüş, Kor Bekçisi: 900 can, saldırı 38). 1. seviyede
başlangıç dağılımıyla her sınıf v4 değerlerini aynen veriyor (Savaşçı 90 can / 18 saldırı / 7
savunma, Haydut 98 / 18 / 2 / %15 kritik, Büyücü 90 / 18 / 2, Şifacı 95 / 16 / 3); Kül Çömezi hâlâ
4–6 vuruşta ölüyor, ~10 vuruşa dayanılıyor. 10. seviye:

| Yapı | Can | Saldırı | Savunma | Kritik / kaçınma | Kor Bekçisi'ne karşı |
|---|---|---|---|---|---|
| Savaşçı, harcanmamış | 270 | 37 | 16 | %5 / %2,4 | ölür (16,6 s) |
| Savaşçı, Güç +18, Day. +9 | 315 | 50 | 18 | %5 / %2,4 | 18,8 s'de yener (20,4 s'de ölecekti) |
| Haydut, harcanmamış | 260 | 37 | 11 | %15 / %5,2 | ölür |
| Haydut, Çev. +18, Day. +9 | 305 | 51 | 12 | %24 / %12,4 | 13,0 s'de yener |
| Büyücü, harcanmamış | 216 | 41 | 11 | %5 / %2 | ölür |
| Büyücü, Zekâ +18, Day. +9 | 261 | 56 | 13 | %5 / %2 | 14,5 s'de yener (16,4 s'de ölecekti) |
| Şifacı, harcanmamış | 248 | 36 | 13 | %5 / %1,2 | mana biter, ölür |
| Şifacı, Güç +9, Day. +9, İrade +9 | 293 | 43 | 15 | %5 / %1,2 | kendini iyileştirerek 43,2 s'de yener |

Mantıklı bir harcama saldırıyı %35–38, saniye başı hasarı %55–58, canı %17–21 artırıyor; 5. seviye
hiçbir yapıyla Kor Bekçisi'ni yenemiyor.

Yük testi (Release, 200 bot, 60 s, 40'ı savaşıp ganimet topluyor ve seviye atlayınca puan
harcıyor): 8.7 s'de hepsi dünyada, 0 düzeltme, 0 kopma; tik ortalaması 1.63–1.68 ms, p99 ≤ 2.55 ms,
0 yavaş / atlanan tik; istemci başına 74 KB/s; PING p50 0.26 ms, p99 5.9 ms; 24 öldürme, 24 ganimet
alma (151 Akçe), 2 seviye atlama ve 2 kaydedilen puan dağıtımı, 0 hatalı cevap.

Not: Release derlemesi `client/main.cpp`'deki bir uyarı (Release'de boşa çıkan `_CrtSet*`
makroları, C4189) yüzünden main dalında da kırılıyordu; tek satırla düzeltildi.

Kararsız akışlar: `levelup` (1. seviye savaşçı 2. seviyeden önce Kül Çömezi sürüsüne ölüyordu) ve
`two_clients_combat` (rahibin `waitfor world+casting`'i bir kez zaman aşımına uğramıştı).
Yapılanlar: `run-ui-flows.ps1` her akışın sunucusunu sabit `world.seed` (4711) ile başlatıyor;
`scene hunt` artık komşusu olmayan Kül Çömezi'ni seçiyor (tek tek çekiş), %75 canın altında
dinleniyor ve ölünce akışı bitirmek yerine yeniden doğup avlanmaya devam ediyor (akış seviye
atlamayı denetliyor, dövüşü değil). Bir tam koşuda `levelup` bir kez daha düştü: seviye atladıktan
yarım saniye sonra başka bir Kül Çömezi vurmuş, `expecthp 100 100` 102/110 görmüştü; akış artık
canın seviye atlamada dolduğunu doğrudan denetliyor (`levelupfull`: LEVEL_UP'tan sonraki 2 s içinde
dolu can çubuğu görüldü — seviye atlamanın SELF_STATS'ı ya da araya bir vuruş girdiyse vuruşun
bulduğu can). `two_combat_b`'nin `casting` hatası tekrar tekrar koşturulduğunda yeniden
üretilemedi; nedeni bilinmiyor. Ayrıca `integration.loot` bir kez seviye 1 savaşçının ikinci bir
Kül Çömezi'ne ölmesiyle düştü: botclient'in `combat` ve `loot` senaryoları artık ölünce yeniden doğup
başka bir Kül Çömezi ile dövüşüyor (dövüşülen atlanır, hasar toplamları kesin kalır).

Bilinçli olarak yok / sınırlar: puan geri alma yalnızca yönetici komutuyla; dünyadaki bir
karaktere yapılan `reset-stats` bir sonraki giriş ya da harcamada görülür; gereksinimli eşya yok;
canavarlar kaçınamaz, oyuncuların vuruşları ıskalamaz; çıplak elle saldırı v4'ten düşük (1. seviye
savaşçı 8, v4: 12) çünkü silah saldırısı nitelikle ölçekleniyor; bağlantı tam `COMMIT` sırasında
koparsa harcama `ServerError` sayılır ama yazılmış olabilir (sonraki harcama ya da giriş doğrusunu
gösterir).

## 21. M11 "Künye" — istemci durumu (2026-11-16)

Sözleşme: `docs/PROTOCOL_STATS.md` §7 (protokol v5). Ayrıntılar `docs/CLIENT_RENDERING.md` §12'de,
kontroller `docs/BUILDING.md`'de. Sunucu, `shared/net`, `gamedata/`, `db/` ve varlıklara dokunulmadı.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| CHAR_STATS | Artık işleniyor: `world::StatSheet` (`client/world/StatsView.h`) son CHAR_STATS'ı, bekleyen (henüz onaylanmamış) puanları ve yoldaki tek isteği tutar. SELF_STATS can / mana çubuklarının hızlı kanalı olarak kaldı. Envanter kuralları (`ItemUser::attributes`) CHAR_STATS temel değerlerinden: gereksinimler yerelde öngörülür, ipuçlarında kırmızı | `StatsViewTests` (tel biçiminden geçen CHAR_STATS, 48 bayt) |
| Künye penceresi | C ya da sağ üstte "Künye (C)" düğmesi; solda, dikey ortalı, 580 × 388 mantıksal piksel, iki sütun: "Nitelikler" (Güç, Çeviklik, Dayanıklılık, Zekâ, İrade: temel değer, eşya bonusu yeşil " (+3)", bekleyen puan amber "+2"; sınıfın saldırı niteliği altın elmasla), "Dağıtılmamış puan: N", Onayla / Vazgeç; "Değerler": Can, Mana, Saldırı Gücü (Büyücü: Büyü Gücü), Savunma, Kritik Vuruş %, Kaçınma %, Can / Mana Yenilenmesi /sn, Saldırı Hızı sn, Şifacıda İyileştirme %. Başlıkta ad, "Aydor · Savaşçı · Seviye N", "Deneyim: x / y". Niteliğin üstüne gelince sınıfa göre Türkçe açıklama ("Savaşçı olarak saldırı gücün bu niteliğe bağlı.") | ekran görüntüleri (1024x640 @150 %, 1280x720, 1600x900, 1920x1080 @150 %) |
| Puan dağıtma | "+" bir bekleyen puan, Shift+"+" beş (ya da kalanı), "+" ya da satıra sağ tık / "−" bir geri; toplam `unspent`'i, nitelik 255'i geçmez. Bekleyen puan varken her türetilmiş satır `game::deriveStats` ile önizlenir ("Can 110 → 115"). Onayla tek `STAT_ALLOCATE_REQ` gönderir (bağlantı başına ilk seferde "Dağıtılan puanlar geri alınamaz. Onaylıyor musun?" Evet / Hayır); istek yoldayken ve Ok'tan sonra yeni CHAR_STATS gelene kadar (en çok 3 s) düğmeler kapalı. Ret metinleri sözleşmedeki gibi, pencerede kırmızı; ServerError'da puanlar bekler | birim testleri (ekle / çıkar / tavan / onay / vazgeç / cevaplar / önizleme = deriveStats, dört sınıf), `stats`, `stats_rich` |
| HUD | "Künye (C)" düğmesi "Envanter (I)"nin altında (güvenli bölge göstergesi bir sıra aşağı); harcanmamış puan varken düğmenin solunda nabız gibi parlayan altın daire ve sayı. Seviye atlama yazısına üçüncü satır: "+3 nitelik puanı · Künye: C". Yardım satırına "C: künye" | `levelpoints`, `unspentmarker` durumları |
| Esc sırası | ganimet penceresi → Künye → envanter → hedef → menü (`escapeActionWithKunye`) | birim testi, `stats` akışı |
| Kaçınma | `COMBAT_EVENT kind 3`: kaçınanın üstünde gri, biraz küçük "Iska" (sayı yok, vuruş tepkisi yok, saldıran yine sallar); yerel oyuncuda ayrıca "Kaçındın!" iletisi ve günlük satırı | `StatsViewTests`, `dodge` akışı |
| Eşya ipuçları | Nitelik bonusları ("Dayanıklılık +3", yeşil) ve gereksinimler ("Gerekli Güç: 20", temel değer düşükse kırmızı) | birim testi, `stats_rich` (Bekçi Kalkanı ipucu) |
| Otomasyon | Sahne komutları `kunye open|close`, `statadd`, `statremove`, `expectattr <nitelik> <temel> [bonus]`, `expectunspent`, `expectpending`, `expectderived` / `expectpreview <alan> <değer>`, `expectstatmessage`, `statlog`; durumlar `kunye`, `statsknown`, `unspent<N>`, `points`, `unspentmarker`, `pending`, `statbusy` / `statidle`, `canadd` / `noadd`, `levelpoints`, sayaçlar `charstats<N>`, `allocated<N>`, `allocrefused<N>`, `dodged<N>`, `seenmiss<N>`; Shift+tık betikte `keydown shift` / `clickon kunye.plus.<anahtar>` / `keyup shift` | `StatsViewTests` (ayrıştırıcı ve hataları) |
| Akışlar | `stats` (1280x720, 1024x640 @150 %, 1920x1080 @150 %; her biri kendi sunucusuyla): 1. seviye savaşçı, puan yok, "+" kapalı, ipucu; 2. seviyeye av, işaret ve yazı; 2 Güç + 1 Dayanıklılık bekleyen (önizleme), sağ tıkla geri alma, Vazgeç, yeniden seçip Onayla (onay kutusu), yeni değerler; envanterle yan yana, Esc sırası; karakter seçimine dönüp tekrar girince dağıtım duruyor. `stats_rich` (yalnızca test veritabanı, kendi `vo_it_*` şeması): `set-level 10` ile 27 puan, envanter açıkken Shift+tıkla Güç +18, Dayanıklılık +9, Onayla; `give-item` ile verilen Bekçi Kalkanı kuşanılınca "Dayanıklılık 20 (+3)"; 3. koşu 1600x900'de sonuç. `dodge` (test veritabanı): 10. seviye haydut 27 puanı Çeviklik'e verir (%16 kaçınma), Kül Çömezi'nin vurmasına izin verir, "Iska"yı görür, sonra öldürür | `run-ui-flows.ps1` |

Son tam koşu (Debug): `build.ps1 -Test` 508/508 ctest (test veritabanı ortam değişkeni olmadan
`integration.db.*` atlanıyor), `run-ui-flows.ps1` 64/64 koşu geçti (`stats_rich` ve `dodge` test
veritabanıyla gerçekten koştu). Geliştirme sırasında akışların düşmesi yalnızca betik hatalarındandı
(ör. kapalı "+" düğmesine sağ tık); M11 akışlarında kararsızlık görülmedi, ama `dodge` olasılıksaldır
(aşağıda).

Bilinen eksikler / sınırlar: kapalı (gri) "+" düğmesine sağ tık çalışmaz (kapalı düğme girdi almaz):
bekleyen puanı geri almak için satıra sağ tık ya da "−"; 1024x640 @150 %'de Künye HUD panelini, bilgi
panelini ve alttaki yardım satırlarını örter, envanterle yan yana sığmaz (1280 mantıksal pikselden
itibaren sığar); ganimet penceresi Künye'nin üstüne açılır; seviye atlama yazısının üçüncü satırı
karakterin ad etiketiyle çakışabilir (yazı ekranın %30'unda sabit); "Iska" ilk anda kaçınanın ad
etiketiyle üst üste biner (hasar sayıları gibi, yükselerek ayrılır); `dodge` akışı deterministik
değil (sunucu RNG'si sabit tohumlu ama hangi zarın hangi vuruşa düştüğünü gerçek zaman belirliyor;
%16 kaçınmada ilk 30 vuruşta ıska görmeme olasılığı < %0,6) — istemci tarafı birim testleriyle de
kapsandı; küçük yazı tipinde (Small, özellikle %150) "ırı" harf dizisi yazı tipinin aralama
çiftleri yüzünden "n" gibi okunuyor ("Saldırı" → "Saldın"; metin doğru, yalnız görüntü; yazı
oluşturucuya dokunulmadı); onay sorusu bağlantı başına bir kez (karakter seçimine dönmek yeni
bağlantı sayılmaz).

## 22. M12 "Yetenek Yolları" — sunucu + ortak kod durumu (2026-11-17)

Sözleşme: `docs/PROTOCOL_SKILLS.md` (protokol v6; istemci için §7 "Client spec": yetenek çubuğu,
K ile açılan "Yetenekler" penceresi, bütün Türkçe metinler). Bu bölüm sunucu ve ortak yarıyı kapsar;
çubuk ve ağaç penceresi istemci ajanının işi. İstemcide yalnız şunlar değişti: v6 el sıkışması
(ortak sabit), iki yeni `ACTION_FAIL` sebebinin Türkçe metni ("Bu yeteneği henüz öğrenmedin.",
"Hedefe atlanacak yol yok."), ve dönemsel (`periodic`) `COMBAT_EVENT`'lerin kaynağın saldırısı
sayılmaması (yalnız sayı). Yeni paketler (`SKILL_*`, `EFFECT_*`) dünya sahnesinde yok sayılıyor;
tuş 1 eskisi gibi sınıfın ücretsiz yeteneğini kullanıyor.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Veri | `gamedata/skills.toml`: `[rules]` (seviye başına 1 puan, kademe eşikleri 0/2/4), sekiz `[[branch]]` (Hücum, Siper, Pusu, Zehir, Köz, Kırağı, Şifa, Yargı), 28 `[[skill]]` (sabit id, ad, simge anahtarı; sayılar kademe başına üçlü), her yeteneğin Türkçe açıklaması yer tutucularla (`{damage}`, `{duration}`, …). `shared/game/Skills.h` katı yükleyici (her hatada "dosya:satır: ne" iletisi), `game::skills()` süreç genelindeki katalog; sunucu dosyayı başlangıçta doğrulayıp kuruyor (yoksa / bozuksa çıkış kodu 6), araçlar ve istemci aynı dosyayı ilk kullanımda yüklüyor. Eski dört yetenek (1–4) 1. kademede v5 ile birebir | `SkillsTests` (sabit liste, ~40 bozuk dosya, v5 eşitliği) |
| Puan ve kademe | 1. seviyenin üstündeki her seviye 1 puan (10. seviye: 9); her yetenek 0..3; her sınıfın ilk dalının 1. kademe yeteneği bedava 1. kademede ve 1'in altına inmez. 2. kademe için o dalda ≥2, 3. kademe için ≥4 harcanmış puan. Öğrenmenin geri dönüşü yok; `vo_admin reset-skills` ve daha düşük `set-level` sıfırlar. M12'den önceki karakterler bütün puanlarını harcanmamış olarak alır | birim (her sınıf her daldan 9 puanı harcayabiliyor), sunucu, Postgres |
| Ölçekleme | Yetenekler M11 niteliklerine yalnız türetilmiş değerlerle bağlı: hasar = saldırı (efektlerle) × çarpan, kritik = kritik + yeteneğin bonusu (≤ %85), iyileştirme / HoT = hedefin canı × % × iyileştirme çarpanı, DoT = saldırı × % (savunma yok, kritik yok), kalkan = saldırı × % | birim, bölge testleri |
| Efektler | Oyuncu ve canavar üstünde zamanlı efektler: saldırı / savunma / saldırı hızı / kaçınma / azami can artışı, DoT, HoT, yavaşlatma, sersemletme, kök, emici kalkan, zayıflatma, kışkırtma. Kimlik (yetenek, tür): yeniden uygulamak tazeler. Yardımcılar oyunculara, zararlılar canavarlara. Ölüm efektleri paketsiz siler (istemci ENTITY_DEATH'te temizler), eve dönen canavar `Cleared` ile siler. Her değişiklik `EFFECT_APPLY` / `EFFECT_REMOVE` ile varlığı görenlere; görüş alanına giren `ENTITY_SPAWN`'ın hemen ardından `EFFECT_LIST` alır. Canavar yapay zekâsı sersemletme, kök, yavaşlatma (hareket ve saldırı aralığı), zayıflatma ve kışkırtmaya uyuyor | `SkillZoneTests` |
| Alan, sıçrama, dost hedef | Kendi çevresinde / hedefin çevresinde alan (yarıçap, en çok hedef, en yakın önce, birincil hedef hep içeride), kimseyi bulmayan alan `NoTarget` ve bedelsiz; Gölge Adımı hedefin 1,5 m önüne (yoksa ±30/60/90° çevresine) yürünebilir düz bir hatla sıçrar, yoksa `NoPath`; şifacı yetenekleri herhangi bir canlı oyuncuya (parti yok) | `SkillZoneTests` |
| Bekleme süreleri | Yetenek başına bekleme, kullanınca sahibine `SKILL_COOLDOWNS`. Seçim: veritabanında yok, dünya ayrılan karakterin bekleyen sürelerini 60 s tutuyor (son konumu tuttuğu bellekle), tekrar girişte sürüyor ve girişte `SKILL_COOLDOWNS` ile bildiriliyor; sunucu yeniden başlayınca hepsi hazır. Hiçbir yeteneğin beklemesi 60 s'yi geçemez (yükleyici denetliyor) | bölge testi, `integration.skills` |
| Protokol | v6: 0x0700–0x070B (`SKILL_STATE`, `SKILL_LEARN_REQ/RES`, `SKILL_BAR`, `SKILL_BAR_SET_REQ/RES`, `SKILL_COOLDOWNS`, `EFFECT_LIST/APPLY/REMOVE`, `SKILL_LEAP`, `SKILL_USED`); `COMBAT_EVENT` bayrakları `periodic` ve `absorbed`; `ACTION_FAIL` 13 `NotLearned`, 14 `NoPath`. Girişte sıra: ENTER_WORLD_RES, INVENTORY, COINS, SKILL_STATE, SKILL_BAR, CHAR_STATS, (varsa) SKILL_COOLDOWNS, doğuşlar. `SKILL_REQ` aynen, yetenek id'siyle | gidiş-dönüş, boyutlar, reddedilen değerler, fuzz |
| Oturum | Öğrenme ve çubuk istekleri tek kuyrukta (uçuştakinin arkasında en çok 8, ikisi birlikte saniyede 10), işlenmiş sıralamaya ve canlı seviyeye göre karar, veritabanına yazılıp cevap yazmadan sonra; iş, saklanan sıralamalarla kuralı satır kilidi altında tekrar ediyor (çevrimiçiyken yapılan `reset-skills` sonraki öğrenmede görülür); ardından `SKILL_STATE` ve `SkillsCommand` | `SkillServerTests` |
| Çubuk | 8 yuva; boş ya da öğrenilmiş yetenek (aynı yetenek birkaç yuvada olabilir); istek bütün çubuğu taşır; yeni karakterde (ve M12'den önceki karakterlerde, `reset-skills`'ten sonra) 1. yuvada bedava yetenek | sunucu, Postgres |
| Veritabanı | `0006_skills.sql`: `character_skills (char_id, skill_id 1..63, rank 1..3)` (ücretsiz yeteneğin 1. kademesi satırsız) ve `characters.skill_bar smallint[]` (NULL = varsayılan; 8 değer, 0..63, NULL eleman yok). İki arka uç da `learnSkill` / `setSkillBar` uyguluyor. 0001–0005'e dokunulmadı; gerçek `vedatonline` veritabanına bağlanılmadı ve uygulanmadı (müdürün işi) | `PostgresSkillsTests` (0006 bir M11 veritabanına), `integration.db.skills` |
| Araçlar | `vo_admin reset-skills <karakter>`; `show-character` yetenek puanlarını, her sıralamayı ve çubuğu da gösteriyor; `set-level` sığmayan yetenekleri sıfırlayıp söylüyor. botclient v6, `skills` senaryosu; yük testinde savaşan botlar çubuğunu bir kez kaydediyor, her seviye atlamada bir yetenek öğreniyor ve öğrendiği her yeteneği kullanıyor | `integration.skills`, `integration.db.skills`, yük testi |

Denge (`tests/game/SkillBalanceTests.cpp`, `VO_TUNING_TABLE=1`): 10. seviye, M11'in mantıklı nitelik
dağılımları ve eşyaları, bütün 9 yetenek puanı bir dalda, Kor Bekçisi'ne (900 can, saldırı 38) karşı
düz dövüş, ortalama zarlar, savaşta mana yenilenmesi yok; basit yapay zekâ (canı %60'ın altındayken
iyileş, güçlendirme ve zayıflatmaları açık tut, yoksa en çok hasar veren hazır yetenek, arada temel
saldırı; kışkırtma tek başına işe yaramaz, kullanılmıyor):

| Yapı | Öldürme | Ölecekti | Saniye başı hasar | Kalan can |
|---|---|---|---|---|
| Savaşçı, yalnız Ağır Darbe 1 | 18,6 s | 20,2 s | 51,2 | %8 |
| Haydut, yalnız Çifte Kesik 1 | 12,9 s | 19,6 s | 70,2 | %34 |
| Büyücü, yalnız Köz Oku 1 | 15,0 s | 15,2 s | 66,0 | %2 |
| Şifacı, yalnız Soluk Güneş 1 | 41,3 s | 155,8 s | 22,3 | %73 |
| Savaşçı Hücum (Ağır Darbe 3, Yarma 3, Kan Öfkesi 3, Sarsıntı 1) | 11,6 s | 21,0 s | 78,6 | %45 |
| Savaşçı Siper (Kalkan Duvarı 3, Meydan Okuma 3, Dirençli Soluk 3) | 19,2 s | 57,9 s | 49,7 | %67 |
| Haydut Pusu (Çifte Kesik 3, Sırt Hançeri 3, Seri Eller 3, Gölge Adımı 1) | 9,5 s | 20,2 s | 110,4 | %53 |
| Haydut Zehir (Zehirli Kesik 3, Sis Bombası 3, Kaçamak 3) | 10,0 s | 26,0 s | 90,4 | %62 |
| Büyücü Köz (Köz Oku 3, Yakıcı İz 3, Kor Yağmuru 3, Alev Halkası 1) | 8,3 s | 16,9 s | 110,4 | %51 |
| Büyücü Kırağı (Kırağı Oku 3, Buz Kabuğu 3, Donduran Nefes 3) | 10,0 s | 81,3 s | 90,7 | %88 |
| Şifacı Şifa (Soluk Güneş 3, Yenileyen Işık 3, Kutlu Siper 3, Halka Şifası 1) | 41,3 s | 223,2 s | 22,3 | %81 |
| Şifacı Yargı (Işık Çarpması 3, Bağlayan Söz 3, Öfkeli Dua 3) | 14,7 s | 48,8 s | 72,3 | %70 |

Test şunu denetliyor: her dal Kor Bekçisi'ni yeniyor; saldırı dalları yalnız bedava yetenekli
sınıftan en az %15 daha hızlı öldürüyor, destek dalları (Siper, Şifa) en az %30 daha uzun yaşıyor;
altı saldırı dalının saniye başı hasarı medyanlarının (90,5) ±%35'i içinde (en yüksek %122, en düşük
%80); aynı sınıfın iki saldırı dalı birbirinin %35'i içinde öldürüyor. Destek dallarının hasarı
bilerek düşük (Siper 49,7, Şifa 22,3): değerleri hayatta kalma (Siper 2,9×, Şifa 1,4× daha uzun) ve
gruplarda (kışkırtma, dosta iyileştirme / güçlendirme) — bunlar tek kişilik simülasyonda ölçülmüyor.
Ayarlamada değişenler (ilk taslaktan): Işık Çarpması ×2,0/2,5/3,0 ve 3,5 s, Öfkeli Dua %15/22/30, Seri
Eller %15/20/25, Kor Yağmuru ×1,7/1,9/2,1, Yakıcı İz %25/32/40, Köz Oku 2–3. kademe ×2,15/2,3,
Kırağı Oku ×1,6/2,0/2,4, Donduran Nefes ×1,0/1,2/1,4.

Yük testi (Release, 200 bot, 60 s; 40 savaşan bot çubuğunu kaydediyor, seviye atlayınca yetenek
öğreniyor, öğrendiği her yeteneği kullanıyor): 8,6 s'de hepsi dünyada, 0 düzeltme, 0 kopma; tik
ortalaması 1,52–1,60 ms, p99 ≤ 2,15 ms, 0 yavaş / atlanan tik; istemci başına 74 KB/s; PING p50 0,24 ms,
p99 4,5 ms; 28 öldürme, 27 ganimet alma, 3 seviye atlama, 40 çubuk ve 3 yetenek öğrenme kaydı, 11 yetenek
kullanımı, 0 hatalı cevap.

Entegrasyon tekrarları: `integration.skills` 5/5, `integration.db.skills` 5/5 (test veritabanında
kendi `vo_it_*` şemasıyla, elle; ctest'te test veritabanı ortam değişkeni olmadan atlanıyor),
`integration.stats` 3/3. İlk tam koşuda `integration.stats` ve `integration.skills` düştü: botclient'in
`fightThrall`'ı Ağır Darbe'yi saldırıdan hemen sonra gönderiyordu ve vuruşun 500 ms genel beklemesine
takılınca (OnCooldown) bütün beklemeyi (6,3 s) bekliyordu; saldırı aralığıyla aynı evreye düşüp hiç
kullanılmıyordu (M11'de de böyleydi, şans eseri geçiyordu). Artık OnCooldown'dan ~300 ms sonra tekrar
deniyor; ölümler 4'ten 0–1'e indi.

Bilinçli olarak yok / sınırlar: oyuncular arası savaş ve canavar yetenekleri yok (zararlı efektler
yalnız canavarlara, yardımcılar yalnız oyunculara); parti yok, dost hedef herhangi bir canlı oyuncu;
efektler ve bekleme süreleri veritabanında saklanmıyor (bekleme 60 s bellekte); `stacks` hep 1;
kışkırtma ve alan iyileştirmesinin grup değeri dengelemede ölçülmedi; çubuk ve ağaç penceresi istemci
ajanının işi; gerçek `vedatonline` veritabanına 0006 uygulanmadı.

Son tam koşu (Debug, son kod değişikliğinden sonra): `build.ps1 -Test` 541/541 ctest (test veritabanı ortam
değişkeni olmadan 11 `integration.db.*` atlanıyor; `Postgres*` birim testleri ve `integration.db.skills` test
veritabanıyla koştu), `run-ui-flows.ps1` 64/64 koşu geçti (v6 sunucusuna karşı mevcut istemci). Release derlemesi
uyarısız.

## 23. M12 "Yetenek Yolları" — sanat durumu (2026-11-17)

Hepsi prosedürel: `tools/blender/gen_skill_art.py` (2B) ve `tools/blender/gen_skill_effects.py` (efektler),
`build_all.ps1`'de `gen_effects.py`'den sonra. İndirme, dış üretici, hazır doku / yazı tipi / model
yok; başka bir oyundan simge ya da ad örneklenmedi. İstemcinin bağlaması için ad, boyut, katman sırası
ve efekt bağlama noktalarının tamamı `docs/SKILL_ART.md`'de. Oyun / sunucu / istemci koduna
dokunulmadı.

| Konu | Ne var | Nasıl doğrulandı |
|---|---|---|
| Beceri ikonları | 24 yeni 64² ikon `ui/skills/<id>.vtex`; M9'un dört ikonuyla aynı çizici (`gen_effects.py`'nin SDF rozeti, altın kenar, koyu çizgili sembol), rozet rengi **dala göre**: Hücum kırmızı, Siper mavi-gri, Pusu koyu mor, Zehir hastalıklı yeşil, Köz kor turuncusu, Kırağı soluk buz mavisi, Şifa soluk altın, Yargı fildişi taş | 64 ve 48 px'te çubuk yuvası içinde temas sayfası (`m12_icons.png`), 3× büyütme (`m12_icons_zoom.png`), `content` testi (64², önceden çarpılmış, köşeler saydam, okunur kontrast) |
| Durum ikonları | 14 adet `ui/status/<ad>.vtex` (64², 24 px'te okunur): güçlendirme altın kenarlı (saldırı, savunma, saldırı hızı, kaçınma, azami can, zamanla iyileşme, kalkan), zayıflatma kırmızı kenarlı (yanma, zehir, yavaşlama, sersemleme, kök, zayıflama, kışkırtılma) | `m12_status.png` (48 ve 24 px, portre altı sıra), `content` testi |
| Çubuk / ağaç arayüzü | `ui/skillbar/`: yuva arka planı, çerçeve normal / üstünde / basılı, boş yuva, bekleme perdesi, "mana yetmiyor" tonu, hazır parlaması, kilit örtüsü + küçük kilit, tuş rozeti, rütbe elmasları (tek ve 0–3 şerit), ağaç bağlantıları (dikey, yatay, dört dirsek, T; yanık / sönük), 8 dal başlığı (256×64, madalyon + ad levhası), "beceri puanı var" işareti (M11 altın diskiyle aynı renkler, dört köşeli yıldızlı ve boş) | `m12_ui.png` (tüm parçalar, örnek çubuk, örnek ağaç), `content` testi |
| Efektler | 22 küçük ışımalı mesh `effects/`: `frost_bolt` (42 üçgen), `ember_mote` (72), `light_strike` (68), `ground_ring_{flame,frost,heal,fog,neutral}` (144), `shield_bubble_{gold,frost,neutral}` (720, jeodezik kafes), `buff_aura_{gold,red,steel,green,neutral}` (160), `tick_wisp_{burn,poison}` (60), `stun_marker` (192), `root_spikes` (28), `leap_trail` (64), `fog_cloud` (448, ışımasız, aydınlatmalı) | `m12_effects.png` (EEVEE, 1.8 m ölçek figürüyle), `content` testi (statik, ≤ 800 üçgen, ışıma haritası derlenmiş, buz oku +Z'ye bakıyor) |
| Boru hattı | assetc UI ikon kuralı `ui/status/` ve `ui/skillbar/`'ı da kapsıyor (`tools/assetc/Build.cpp` `isUiIcon`, tek satır); bu kural kenarı ikinin kuvveti olmayan PNG'yi reddettiği için başlıklar 256×64, tuş rozeti 32², rütbe şeritleri 64×16 | `build-assets.ps1 -Config Debug`, `build.ps1 -Test` |

**Yeniden üretme**: `powershell -File tools/blender/build_all.ps1` (hepsi) ya da tek tek
`blender --background --factory-startup --python tools/blender/gen_skill_art.py -- [--only icons|status|ui] [--sheets <klasör>]`
ve `... gen_skill_effects.py -- [--sheets <klasör>] [--no-preview]`. Temas sayfaları varlık değil:
varsayılan `build/art_sheets/m12/` (git dışı). İkinci çalıştırma kaynakları bayt bayt aynı bırakıyor.

**Bilinen zayıf noktalar**: `kacamak` (zıplayan figür + iki silik kopya) 48 px'te okunuyor ama
"acil çıkış" levhasındaki koşan adamı andırıyor; `alev_halkasi` 48 px'te kalabalık (on alev dili +
küçük figür); `yakici_iz` ile `kor_yagmuru` aynı turuncu rozette en yakın çift (biri yükselen üç alev,
öbürü düşen beş kuyruklu yıldız: ayrılıyor ama ilk bakışta değil); `direncli_soluk`'un nefes çizgileri
ince. Efektlerde `fog_cloud` aydınlatmalı düz kümeler olduğu için sis değil gri-yeşil kayalara benziyor
(saydamlık yok: `addEffectMesh` opak çiziyor); `shield_bubble` opak çubuklu bir kafes (saydam baloncuk
yerine); `addEffectMesh` tonu yalnız taban rengini çarptığından renkli çeşitler ayrı dosya, `_neutral`
çeşitleri ancak istemci ışımayı da tonla çarparsa serbestçe renklenir (istemci ajanının kararı).
Efektler motorda görülmedi, yalnız Blender'da; yaşam süreleri / ölçekler `SKILL_ART.md`'de öneri.

## 24. M12 "Yetenek Yolları" — istemci durumu (2026-11-17)

Sözleşme: `docs/PROTOCOL_SKILLS.md` §7 (protokol v6), sanat `docs/SKILL_ART.md`. Ayrıntılar
`docs/CLIENT_RENDERING.md` §13'te, kontroller ve akışlar `docs/BUILDING.md`'de. Sunucuya, `shared/`'a,
protokole, `gamedata/skills.toml` sayılarına ve göçlere dokunulmadı; istemci (görüntüleyici ve gölgelendirici
dahil), testler, akışlar ve belgeler değişti.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Model | `client/world/SkillsView.h`: `SkillBook` (SKILL_STATE sıralamaları ortak kurala göre normalleştirilmiş, puan / harcanmamış, çubuk, yolda tek öğrenme ve tek çubuk isteği, uçuştayken gelen son çubuk isteği sırada), çubuk düzenlemeleri, `EffectBook` (varlık başına zamanlı efektler; (yetenek, tür) tazelenir, en çok 16; doğuş / kayboluş / ölümde silinir, bitişten 2 s sonra emniyet olarak düşer), `decideSkillUse` / `slotLook` (§7.1 hedef kuralı ve yuva tonları), §7.3 ipucu satırları, §7.4 metinleri, efekt sanatı planı, Esc sırası, otomasyon ayrıştırıcı. `SkillTimers` artık yetenek başına bekleme tutuyor (SKILL_COOLDOWNS kesin değeri koyar, SKILL_USED / anlık COMBAT_EVENT / tamamlanan büyü arada öngörür; süren bekleme aynı kullanımın ikinci vuruşuyla yeniden başlamaz) | `SkillsViewTests` (14 test durumu: öğrenme kapısı dört sınıfta rastgele sıralamalarla `game::checkSkillLearn` ile birebir, çubuk sırası, bekleme geri sayımı, efekt defteri, ipucu ve sayı biçimi, sanat planı manifestteki meshler, ayrıştırıcı) |
| Çubuk | Altta ortada 8 yuva (48 mantıksal px), 1–8 tuşları ve tık. `ui/skillbar/` parçaları: kuyu, ikon (52/64), mana yetmezken kırmızı perde + kızaran ikon, düşman yeteneğinde seçili canavar yok / menzil dışı iken koyu perde, ölüyken gri, kendi ya da 500 ms genel beklemenin saat yönünde süpürmesi ve saniyesi, bekleme bitince 0,25 s `slot_ready_flash`, çerçeve (normal / üstünde / basılı), tuş rozeti. Sürükle: yuvadan yuvaya değiştirir, çubuk dışına bırakmak / sağ tık temizler; her değişiklik tek SKILL_BAR_SET_REQ, çubuk her zaman sunucunun sakladığını gösterir. İpucu yuvanın üstünde ortalı. M9'un tek beceri düğmesi kalktı; büyü çubuğu çubuğun üstünde | `skills_bar`, `skills_warrior*` akışları |
| Hedef | Düşman → seçili canavar (menzil dışında önce yürür; yoksa "Önce bir hedef seç."), dost → seçili canlı oyuncu, yoksa kendisi (0), kendine → 0. Oyuncular zaten seçilebiliyordu (sol tık, `target player`) | birim testi; `two_clients_combat` (rahip Soluk Güneş'i 1. yuvadan seçili savaşçıya) |
| Yetenekler penceresi | K ya da "Yetenekler (K)" düğmesi; solda, Künye'nin üstünde, 656 × 424 mantıksal (1024x640 @150 %'e sığar). "Yetenek puanı: N" (N > 0 iken altın), sınıf satırı; her dal için `branch_<anahtar>` levhası ve ad, üç kademe sırası (Hücum / Pusu / Köz / Şifa'nın 3. kademesinde iki yetenek yan yana), kapı açılınca yanan `tree_*` bağlantıları, kilitli kademenin üstündeki boşlukta kilitli hap "Bu yolda N puan gerekli" (altındaki hücreler gri), sütun altında "Bu yolda harcanan: N", pencerenin içinde çubuğun bir kopyası (pencere HUD çubuğunu örttüğünde de sürükle bırak çalışsın diye), ileti satırı. Hücre: ikon (0. kademede soluk, kilitliyken gri), altında `rank_pips_<k>`, ad, "k/3" ya da "Öğrenilmedi", yükseltilebiliyorsa "+" (istek yoldayken kapalı). İlk "+" bağlantı başına bir kez sorar: "Harcanan yetenek puanları geri alınamaz. Onaylıyor musun?" (Evet / Hayır). Öğrenilmiş bir yeteneğin hücresi çubuğa sürüklenir. Esc: ganimet → Yetenekler → Künye → envanter → hedef → menü | `skills_bar`, `skills_warrior` (3 koşu) |
| Metinler | §7.4'ün bütün metinleri (öğrenme sonuçları, çubuk sonuçları, ACTION_FAIL 13 / 14, "Önce bir hedef seç."); seviye atlama yazısına dördüncü satır "+1 yetenek puanı · Yetenekler: K", harcanmamış puan varken düğmenin solunda nabız gibi atan `skill_point_marker_blank` ve sayı. **M11 hatası giderildi**: yazı bloğu artık kendi ad etiketinin 8 px üstünde bitecek şekilde yerleşiyor (hedef çerçevesinin altında kalarak); yer dar ise blok %60'a kadar küçülüyor | birim testi; `skills_bar` (`toastabovelabel` durumu, 1280x720 ve 1024x640 @150 %) |
| Efektler (ekran) | Oyuncunun efektleri HUD panelinin altında (F3 bilgi paneli bir sıra aşağı), hedefinkiler hedef çerçevesinin altında: yeteneğin ikonu, köşede efektin `ui/status/` ikonu, yardımcıda yeşil / zararlıda kırmızı çerçeve (son 2 s yanıp söner), altında kalan saniye, kalkanda üstünde kalan emme. İpucu: yetenek adı, §7.7 tür satırı ("Saldırı +%15", "Zehir: saniyede 14", "Yanma: …", "Kalkan: 54", "Sersemledi", "Kışkırtıldı" …), kalan saniye. Dönemsel vuruşlar daha küçük sayı, tamamen emilen vuruş gri "Emildi", dolu cana gelen 0'lık iyileşme hiç sayı göstermiyor | dört sınıf akışı (`expecteffect` modeli ve HUD ikonunu birlikte denetler, `expecteffecttip`) |
| Efekt sanatı | 22 efekt meshi (+ M9 köz oku) SKILL_USED, dönemsel vuruşlar, SKILL_LEAP ve süren efektlerden: köz / kırağı oku elden göğse uçar (kırağı dönerek, parıltıyla), Işık Çarpması hedefin ayağında ışık mızrağı, Kor Yağmuru'nda 10 kor 7 m'den alana düşer + alev halkası, alan yeteneklerinde yarıçapa ölçekli zemin halkaları (arazinin eğimine yatırılmış), Sis Bombası'nda yavaşlatma süresince sis bulutu + halka, güçlendirmelerde aura / kabarcık parlaması; efekt sürerken baş üstünde sersemlik yıldızları, kalkan kabarcıkları (Buz Kabuğu buz, Kutlu Siper altın), güçlendirmelerde soluk aura; DoT vuruşunda göğüsten yükselen yanma / zehir tülü. SKILL_USED büyü salınımını / silah savuruşunu başlatır (büyülü yetenekler CAST_END'de). **SKILL_LEAP**: sıçrayan 0,25 s'de 0,35 m'lik kavisle iniş noktasına kayar (yerel karakterde hareketçi hemen ışınlanır, çizim ve kamera kaymayı izler; ardından gelen POSITION_CORRECT eskimiş sayılır), arkasında `leap_trail` söner | akışlarda `expectart <mesh>` (son karede görüntüleyiciye verilen örnekler), ekran görüntüleri |
| Saydamlık | `WorldRenderer::addEffectMesh` saydamlık ve ışıma tonu aldı: opaklığı < 1 olan meshler çıkartmalardan sonra, uzaktan yakına, önçarpımlı karışımla, derinlik sınanıp yazılmadan çiziliyor (`Mesh.hlsl` çizimin `alphaTest.z` > 0 iken `rgb × a, a` döndürüyor); `tintEmissive` alt meshin ışıma çarpanını tonla çarpıyor (`_neutral` halkalar her renge boyanıyor). Sis, kabarcıklar, halkalar, auralar %35–90 opak; oklar, korlar, yıldızlar opak | ekran görüntüleri (sis bulutu, kabarcıklar, halkalar saydam) |
| İstek bütçesi | SKILL_LEARN_REQ + SKILL_BAR_SET_REQ sunucuda birlikte saniyede 10 (aşınca ERROR Flood ve bağlantı kapanır): istemci saniyede en çok 8 gönderiyor, fazlası bekleyip sonra gidiyor (geliştirmede betik hızında 9 öğrenme + çubuk bağlantıyı kopardığı için eklendi) | akışlar |
| Otomasyon | `skills open|close`, `skilllearn`, `barset`, `useslot`, `pullpack <canavar> <n>` (boştaki canavarlara yaklaşıp n tanesi peşimize düşene kadar yürür), `expectrank`, `expectskillpoints`, `expectslot`, `expectslotlook`, `expectcooldown`, `expecteffect` / `expectnoeffect`, `expecteffecttip`, `expectart`, `expectskillmessage`, `skilllog`; durumlar (`skillpoints<N>`, `skillmarker`, `canraise` / `noraise`, `locked<dal>_<kademe>`, `ready_<anahtar>` / `cd_<anahtar>`, `selfeffect_<tür>`, `targeteffect_<tür>`, `art_<mesh>`, `toastabovelabel`, sayaçlar `learned`, `barsets`, `skilluses`, `usehits`, `effectticks`, `absorbs`, `leaps` …) | `SkillsViewTests` (ayrıştırıcı) |

**Kontroller**: 1–8 (ya da yuvaya tık) çubuktaki yetenek; çubukta sürükle (değiştir / dışarı bırakınca
temizle), sağ tık temizler; K ya da "Yetenekler (K)" yetenek penceresi ("+" bir kademe, hücreyi çubuğa
sürükle); Esc sırası ganimet → Yetenekler → Künye → envanter → hedef → menü. Yardım satırında "1-8:
yetenekler".

**Akışlar** (`run-ui-flows.ps1`; her biri durumu denetler, yalnız fotoğraf çekmez):

| Akış | Ne denetliyor |
|---|---|
| `skills_bar` (1280x720, 1024x640 @150 %; veritabanısız) | 1. seviye savaşçı: SKILL_BAR'dan 1. yuvada Ağır Darbe, hedefsiz 1 tuşu "Önce bir hedef seç." ve koyu yuva (`expectslotlook notarget`), yuva ipucu (§7.3 satırları); pencere: 0 puan, "1/3" / "Öğrenilmedi", kilit notları, `noraise`; pencereden 3. yuvaya sürükle, 3↔5 değiştir, sağ tıkla temizle, çubuk dışına sürükleyip temizle, geri koy (her biri `barsets<N>` ve `expectslot`); Esc önce pencereyi kapatır; Kül Çömezi'ne 1 tuşu menzile yürüyüp kullanır (`skilluses1`, `expectcooldown 5..6,05`, `gcd`), sayaç düşer; 2. seviyeye av: dördüncü satır, düğme işareti, `toastabovelabel`, ilk "+" onay sorusu (metin birebir), kademe 2; karakter seçimine dönüp girince çubuk ve kademe duruyor |
| `skills_warrior` (test veritabanı; 3 koşu) | 2: `set-level 10` ile 9 puan, işaret ve düğme ipucu, bütün 2. / 3. kademeler kilitli; ilk "+" onayı, "Ağır Darbe kademe 2 oldu.", 2 puanla Hücum 2. kademe açılır (`unlocked0_2`), 4 puanla 3. kademe, Siper 2. kademe; 9 puan harcanınca `noraise`, sıralamalar, "Bu yolda harcanan: 6 / 3"; beş yetenek çubuğa sürüklenir; Kan Öfkesi: oyuncu efekt ikonu, ipucu "Saldırı +%15", kırmızı aura; Kalkan Duvarı: altın kabarcık + çelik aura; `pullpack` ile iki Kül Çömezi, Meydan Okuma (hedef çerçevesinde kışkırtma), Sarsıntı (sersemlik ikonu, "Sersemledi", baş üstü yıldızlar), tek Yarma iki canavarı vurur (`usehits2`) ve halka. 3 (1024x640 @150 %): tekrar girişte sıralamalar ve çubuk geri geliyor, pencere sığıyor, pencere içi çubuğa sürükle / sağ tık, ipuçları, Esc |
| `skills_rogue` (test veritabanı) | Haydut 9 puanı harcar (Zehir'de 3. kademe de açılır); Kor Bekçisi'ne 12 m'den Gölge Adımı: menzile yürür, SKILL_LEAP, iz sanatı, hedefin dibine iner; Sis Bombası (yavaşlatma ikonu "Yavaşlatıldı %30", sis bulutu + halka), Zehirli Kesik (zehir ikonu, ipucu "Zehir: saniyede", dönemsel vuruş, zehir tülü) |
| `skills_mage` (test veritabanı) | Büyücü 9 puan; Buz Kabuğu (kalkan ikonu ve emme miktarı, buz kabarcığı); Kor Bekçisi vurur, kalkan emer (`absorbs1`); Kırağı Oku (büyü çubuğu, dönen buz oku, "Yavaşlatıldı %40"), Yakıcı İz ("Yanma: saniyede", vuruşlar), Kor Yağmuru (düşen korlar, alev halkası) |
| `skills_priest` (test veritabanı) | Rahip 9 puan; seçili oyuncu yokken dost yetenekler kendine: Kutlu Siper (savunma ve "En yüksek can +%10" iki ikon, altın kabarcık), Yenileyen Işık ("Yenilenme: saniyede"), Halka Şifası (şifa halkası); Kor Bekçisi'ne Bağlayan Söz ("Zayıfladı %20", "Yavaşlatıldı %20") ve Işık Çarpması (ışık mızrağı) |

Ekran görüntüleri: `docs/screenshots/m12/01…14_*.png` (00 sanat ajanının ikon sayfası için ayrıldı).

Son tam koşu (Debug, son kod değişikliğinden hemen önce; ardından yalnız sersemlik yıldızlarının ölçeği büyütüldü ve `vo_tests "[client]"` ile `skills_warrior` 3/3 yeniden koştu): `build.ps1 -Test` 557/557 ctest (test veritabanı ortam değişkeni olmadan 11 `integration.db.*` atlanıyor, M11/M12 sunucu koşularındaki gibi), `run-ui-flows.ps1` 75/75 koşu geçti (eski 64 + yeni 11; `skills_*` test veritabanıyla gerçekten koştu). Geliştirmede düşmeler: test adlarındaki "§" ctest süzgecini bozdu (adlar ASCII yapıldı); betik hızında 9 öğrenme + çubuk isteği sunucunun ERROR Flood sınırına takıldı (istek bütçesi eklendi); genel bekleme yüzünden reddedilen tuşlar (betikler artık `ready_<anahtar>` bekliyor); kalkanın yürürken bitmesi ve görüş dışındaki canavarlar (betik düzeltmeleri).

**Sözleşmeden sapmalar / yorumlar**:
- Efekt ikonu: sözleşme "yeteneğin ikonu" diyor; ikon o, ama aynı yeteneğin iki efekti (Kutlu Siper:
  savunma + can) ayırt edilsin diye köşeye efektin `ui/status/` ikonu da kondu. İpucuna "N sn kaldı" satırı
  eklendi.
- "Mana yetmiyor" tonu sözleşmedeki gibi **kırmızı**; `SKILL_ART.md` mavi `slot_nomana` öneriyordu, kullanılmadı.
- §7.3'te "Menzil: … | "Kendine" | "Çevrene"" ve "Hazırlık: … | "Anında"" seçenekleri satırın tamamı
  olarak okundu ("Kendine", "Çevrene", "Anında" tek başına).
- 0. kademedeki hücrede "0/3" yerine "Öğrenilmedi" yazıyor (rozetler 0 dolu); "k/3" 1–3'te.
- Kademe kapısı notu her hücrede değil, kilitli kademe sırasının üstünde bir kez (her sıranın hücreleri
  aynı kapıya bağlı); hücreler gri.
- HUD düğmesinin yazısı "Yetenekler (K)" (ipucu da aynı, harcanmamış puan varken ikinci satır "Yetenek puanı: N").
- Pencerenin içinde çubuğun ikinci bir kopyası var (sözleşmede yok): 1024x640 @150 %'de pencere HUD
  çubuğunu örtüyor.
- İstemci öğrenme + çubuk isteklerini saniyede 8 ile sınırlıyor (sözleşme 10'da kesiyor).
- Efekt sanatı manifestin önerileri; manifestte olmayan seçimler: Yarma (sıcak tonlu nötr halka),
  Meydan Okuma (çelik halka), Bağlayan Söz (kehribar halka), Soluk Güneş (küçük şifa halkası); Işık
  Çarpması verilerde `projectile` ama manifestteki gibi mermi değil yerde ışık mızrağı; Sis Bombası'nın
  atılan bir bombası yok (bulut hemen çıkıyor); kor tanesi ölçeği 1,0–1,6 (manifest 0,8–1,3: uzaktan
  okunsun diye).
- Sunucu, ortak kod, protokol, `skills.toml` ve göçler değişmedi.

**Bilinen eksikler / sınırlar**:
- 1024x640 @150 %'de Yetenekler penceresi HUD çubuğunu, oyuncu efekt sırasını ve HUD panelini örtüyor
  (pencere içi çubuk bunun için var); alttaki ilk yardım satırı bu boyutta ekrana sığmıyor (M11'den beri,
  "1-8: yetenekler" ikinci satırı uzatmadı ama birinci satır zaten taşıyordu).
- Efekt ikon sıraları pencerelerin altında kalıyor; ipuçları her şeyin üstünde.
- `fog_cloud` saydam ama hâlâ köşeli kümeler (sanat sınırı); kabarcıklar saydam kafes. Zemin halkaları
  merkezdeki eğime yatırılıyor; engebeli zeminde bir kısmı toprağa gömülebilir.
- Kor taneleri uzaktan küçük. Sersemlik yıldızları manifestin 1,7 katı çiziliyor ama yine küçük ve canavarın ad etiketinin arkasında kalabiliyor. Kök (`root`) efekti hiçbir gönderilen yetenekte yok: `root_spikes` kodda
  bağlı ama hiç görülmedi.
- Sıçrayan uzak oyuncunun kayması 0,25 s boyunca enterpolasyonun üstüne yazılıyor; iki istemcili bir
  akışla denenmedi.
- Alan akışı (`pullpack` + Yarma) canavarların dolaşmasına bağlı; bu oturumdaki altı koşunun hepsinde geçti ama deterministik
  değil (sunucu tohumu sabit, zamanlama gerçek).
- `statlog`'daki "client derivation differs" uyarısı güçlendirme açıkken beklenen şekilde çıkar
  (CHAR_STATS artık efektli değerleri taşıyor).
- Avlanma döngüsü (`scene hunt`) yalnız sınıfın bedava yeteneğini kullanıyor.

**Doğrulanmadı**: Release derlemesi; başka bir istemcinin efektleri / auraları / sıçramayı görmesi (EFFECT_LIST
doğuşta yalnız birim testlerinde ve tek istemcide canavarlar için); kök efekti; gerçek `vedatonline`
veritabanı (yalnız `vedatonline_test` şemaları); 60 s yeniden giriş bekleme belleği yalnız dolaylı
(`skills_warrior` 3. koşusu Kan Öfkesi'nin hazır olmasını bekliyor, girişte SKILL_COOLDOWNS günlükte);
başka ekran kartları / sürücüler.

## 25. M13 "Üç Yurt" — harita ve sanat durumu (2026-11-17)

Hepsi Blender'da başsız, depodaki betiklerle üretildi; indirme, dış üretici ya da yeni hazır varlık
yok (yeni dokular depodaki dört Poly Haven taramasından, yeni canavarlar depodaki kaynak mesh'lerden
türetildi). Ayrıntılar, işaret tablosu, canavar tablosu ve istemcinin her bölgede ne yükleyeceği:
`docs/WORLD_ZONES_ART.md`. Sunucu / istemci / ortak koda ve `skills.toml` / `items.toml`'a dokunulmadı;
assetc'ye değişiklik gerekmedi.

| Konu | Ne var | Nasıl doğrulandı |
|---|---|---|
| Bölge 1 (Ortaocak) | Yükseklik ve splat haritası M12 ile bayt bayt aynı; tanıma dört işaret (`gate_aydor` 287/213, `arrive_aydor` 282.32/219.49, `gate_kargan` 274/326, `arrive_kargan` 266/325.94) ve iki kapı eklendi, yürüme maskesine yalnız kapı ayakları. Aydor kapısı köyün güneyinde, Kargan kapısı kuzeyinde; Aydor kapısı meydana, Kargan kapısı 12 m ötedeki kuzey yoluna bakıyor; ikisi de `world_tour` hattının ve kurt çemberlerinin dışında | `gen_terrain.py` kontrolleri (açıklık yürünebilir, kapı → varış → yol düz yürünebilir), `content` testleri |
| Bölge 2 (Tanyeri Burcu) | 512 m altın ova, iki basamaklı tebeşir yarlarla çevrili; teras üstünde beyaz taş kasaba (burç kulesi, sur, 15 ev, soluk güneşli sancaklar, kuyu), çevrede çitli tarlalar (saban izi), uzakta yanık tarlalar (av yeri); kuzey yolu yarı kesip kapıya varıyor, kapının ardı kapalı. `spawn_aydor`, `gate_esik`, `arrive_esik` | `gen_zones.py` kontrolleri, kuşbakışı + yer seviyesi çizimler, `content` testleri |
| Bölge 3 (Demirağız geçidi) | Karlı koyu dağlar arasında kuzeye yükselen dar vadi, üç yan çanak (av yerleri), kasabanın kuzeyinde dik kaya duvarı; duvara oyulmuş beş kapı, taş kulübeler, iki ocak, mangallar, demir bağlı çit; güneyde boğazın dibinde kapı. `spawn_kargan`, `gate_esik`, `arrive_esik` | aynı |
| Doku katmanları | 8 yeni 1024² PBR katman (`steppe`, `chalk_road`, `chalk_rock`, `scorched`, `ash_tundra`, `gravel`, `basalt`, `snow`), mevcut taramalardan renk ayarıyla; üç bölge hiçbir zemin katmanını paylaşmıyor | `previews/textures/terrain_layers_m13.png`, test |
| Kapılar | Prosedürel taş kemer, üç görünüm: Aydor (beyaz taş, altın soluk güneş, sancaklar, mazgallar), Kargan (koyu taş, demir kuşaklar, dikenler, ayaklarda mangal, kor runları), Eşik (yıpranmış gri, soluk run kakmaları). "Kapı vardığı yerin görünümünü taşır": bölge 1'de Aydor ve Kargan kapıları, yurtlarda Eşik kapısı. Ayrıca kemeri dolduran çift yüzlü ışıyan perde efekt mesh'i (`effects/gate_veil_<görünüm>`), yarı saydam çizilmek üzere | `previews/props/m13_props_sheet.png`, `docs/screenshots/m13/zone1_gate_*.png`, test |
| Yerleşim nesneleri | Aydor: `aydor_tower`, `aydor_wall`, `aydor_house`, `aydor_banner`; Kargan: `kargan_door`, `kargan_forge`, `kargan_brazier`, `kargan_palisade`, `kargan_hut` (120–912 üçgen, Cycles ile pişirilmiş renk / normal / ORM / ışıma) | aynı |
| Canavarlar | 6–13 numaralı şablonlar: Anız Yanığı (1–2, `stubble_thrall`), Yanık Nöbetçi (3–4, `burnt_sentry`), Kırağı Kayası (1–2, `rime_rock`), Donuk Ocakçı (3–4, `frozen_hearthguard`); mevcut iskelet ve 11 klip, yeni deri (UV uzayında renk eşleme, çatlak ağları, kırağı / saman desenleri); ganimet tabloları mevcut eşyalarla | `previews/monsters/*`, `docs/screenshots/m13/zone*_monster_*.png`, `content` ve `game` testleri |
| Doğuş / güvenli bölgeler | Bölge 2'de 20, bölge 3'te 15 doğuş noktası (yakında 1–2. seviye sürüler, uzakta 3–4); her kasaba ve her varış noktası için güvenli bölge; bölge 1'e iki küçük güvenli bölge (yeni varış noktaları köyün dışında) | `MonsterSpawnTests` (üç bölge), `map_zone*_overview.png` |
| Testler | `EnvironmentTests` üç bölgeye genişledi (işaretler, kapı açıklığı ve yola düz yol, yürüme maskesi, yerleşim, katmanlar, determinizm); `MonsterSpawnTests` üç bölge; M9/M10 kadro testleri (`CombatTests`, `LootTests`) yeni şablonları kapsayacak şekilde güncellendi; `MonsterTests` yeni dört modeli de denetliyor | `build.ps1 -Test` |

**Yeniden üretme**: `powershell -File tools/blender/build_all.ps1` (`gen_zone_props.py` `gen_props.py`'den
sonra, `gen_zones.py` en sonda), inceleme çizimleri `gen_zone_shots.py` (varlık değil,
`docs/screenshots/m13/`). İkinci çalıştırma kaynakları bayt bayt aynı bırakıyor (pişirilmiş haritalar
`write_png_stable` ile).

**Sapmalar**: yeni canavarlar yeni üretilmiş mesh değil, mevcut kaynakların yeniden derilenmesi
(Yanık Nöbetçi `aydor_2`, Donuk Ocakçı `kargan_0` oyuncu görünümlerinden: düşmüş askerler);
bölge 1'de Aydor kapısı meydana, Kargan kapısı en yakın yola bakıyor (kapıdan yola / meydana düz yol için; güney yolu güney sürüsünün içinden geçtiği için Aydor kapısı ona bakmıyor), bu yüzden
`arrive_*` ilk işlemdeki konumundan kaydı; yurt bölgelerinde yalnız kendi ırkının doğuş noktası var
(sunucunun bugünkü `Zone::create`'i ikisini de istiyor).

**Bilinen zayıf noktalar**: yeni nesneler düşük çokgenli ve temiz-stilize (120–900 üçgen), bölge 1'in
yarı gerçekçi üretilmiş nesnelerinin yanında sade kalıyor; bölge 3'ün dağları gürültü dokulu ve kasaba
arkasındaki kaya duvarı düz bir perde gibi (önizlemede doku düzlemsel izdüşümle uzuyor, oyunda kaya
katmanı üç düzlemli); Donuk Ocakçı'nın soğuk çatlakları az (ışıma payı %0.2); Yanık Nöbetçi'nin ölüm
klibinde zırhı yere 0.56 m giriyor (oyuncu görünümünün aynı klibi; test eşiği bu model için 0.6 m);
bölge 2'de pazar tezgâhı ve eşya yığınları az (ev halkası plazayı dolduruyor); bölge 1'in güneyi iki sürüyle dolu olduğundan Aydor kapısı güneydoğu sürüsünün saldırı menzilinin yalnız 4.8 m ötesinde (sürüler taşınmadı; daha fazla pay istenirse o sürü kaydırılmalı).

**Doğrulanmadı**: hiçbiri oyun istemcisinde görülmedi (istemci yeni bölgelere giremiyor); kapı
perdesinin yarı saydam görünümü yalnız Blender'da; yeni dokuların BC sıkıştırmalı hâli yalnız test
ile; canavarların oyundaki dengesi (değerler mevcut eğriden türetildi, oynanmadı).

## 26. M13 "Üç Yurt" — sunucu + ortak kod durumu (2026-11-17)

Sözleşme: `docs/PROTOCOL_ZONES.md` (protokol v7; istemci için §7 "Client spec": yükleme ekranı, bölge
afişi, geçit etiketleri, bütün Türkçe metinler), kodla birlikte güncel tutuldu. Sunucu tarafı
`docs/SERVER_WORLD.md` §8f'de, veritabanı `docs/DATABASE.md`'de, testler ve yük testi
`docs/BUILDING.md`'de. Bu bölüm sunucu ve ortak yarıyı kapsar; afiş, geçit etiketleri ve yükleme
ekranı istemci ajanının işi. İstemcide yalnız asgari değişiklik var (aşağıda).

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Veri | `gamedata/zones.toml`: `[start]` (Aydor → 2, Kargan → 3) ve üç `[[zone]]` (id, anahtar, Türkçe ad ve alt yazı, arazi yolu, izinli ırklar) ile dört `[[zone.gate]]` (bu bölgedeki işaret, tetik yarıçapı 2,5 m, hedef bölge, oradaki varış işareti). `shared/game/ZoneData.h` katı yükleyici (her hatada "dosya:satır: ne": bilinmeyen anahtar, tekrar eden id / anahtar / arazi / geçit işareti, aralık dışı sayı, bilinmeyen ırk, kendine ya da bilinmeyen bölgeye geçit, kimsenin geçemeyeceği geçit, ırkına kapalı başlangıç bölgesi, 16'dan çok bölge / 8'den çok geçit), süreç genelindeki `game::zones()`; saf kural `game::checkGate` (Yükleniyor → Ölü → Kurulu değil → Irka kapalı → Geç). `shared/game/Zones.h`'nin ad tablosu dosyayla aynı (testle denetleniyor): 1 Ortaocak, 2 Tanyeri, 3 Demirağız | `ZonesTests` (gönderilen dosya, ~40 bozuk dosya, kural, v7 kodlayıcıları) |
| Başlangıç | vo_server listedeki **her** bölgeyi yükler; arazi yoksa / bozuksa, bir ırkın `spawn_<ırk>`'ı, bir geçit işareti ya da hedefteki varış işareti eksik / yürünemez ise, ya da varış işareti hedefin bir geçidine yarıçap + 1 m'den yakınsa çıkış kodu 6 ve bölgeyi, dosyayı, işareti söyleyen ileti. `world.zones_file` dosyayı, `world.assets_dir` arazileri değiştirir; yüklenmemiş bölgelerin canavar doğuşları uyarıyla atlanır | `ZoneChangeTests` (başlangıç denetimleri), `integration.noterrain` |
| Çok bölgeli dünya | Her bölgenin kendi oyuncuları, canavarları, ganimeti, efektleri, AOI ızgarası ve rastgele sayı üreticisi (`world.seed + id − 1`; bölge 1 eski zarları tutar); paketler yalnız bir bölgenin varlıklarından kurulur, başka bölgenin varlığı asla gönderilmez; varlık numaraları dünya çapında tek | `ZoneChangeTests` (yalıtım, 12 gidip gelen ile AOI), `integration.zones --phase pair` |
| Geçit kuralı | Oyuncu işaretin yarıçapı içindeyse içeride. Tuşa basmadan, içerideyken koşullar sağlanır sağlanmaz geçer (seviye tetikli). Kurulma: bölgede belirdikten (giriş, varış, ZONE_LOADED_REQ) 2 s sonra **ve** o zamandan beri bütün geçitlerin dışına çıkmışsa; geçidin içinde oturum açan önce dışarı çıkmalı. Varış noktaları ve doğuş dağılımı hiçbir tetiğin içine düşmez, yani geçitten çıkan geri sekmez. Ölü oyuncu geçmez. Öteki ırkın yurdu: içeride kalış başına bir `ZONE_GATE_REFUSED`, en az 3 s arayla; oyuncu yerinde kalır | `ZoneChangeTests` (yeniden kurulma, girişte içeride, ret sayısı, ölü) |
| Durumlar | Savaşta geçmek **serbest** (eski bölgenin canavarları oyuncuyu bırakıp eve döner, eski canavarlara konan DoT'lar kimsenin sayılmadan sürer); büyü yarıda kalır (bedelsiz, beklemesiz); otomatik saldırı ve hedef temizlenir; oturan ayakta varır; açık ganimet penceresi paketsiz kapanır; yazılmakta olan ganimet alma normal biter; eşya / nitelik / yetenek istekleri oturumda sürer ve yeni bölge sonucu kullanır. Efektler kalan süreleriyle taşınır (kendi efektlerinin kaynağı yeni varlık numarası, başkalarınınki 0), yetenek beklemeleri yeniden girişteki gibi | `ZoneChangeTests` (efektler ve beklemeler), `ZoneServerTests` |
| Geçiş sırası | Kapı ateşlenince: eski bölgenin o tikteki son paketleri + `ZONE_CHANGE_BEGIN` (aynı toplu gönderimde), eski bölgede gözlemcilere `ENTITY_DESPAWN`; varış konumu hedefte hesaplanır (işaret, doğuş gibi dağıtılmış) ve normal durum kaydıyla **önce veritabanına yazılır**; yazma bitince (`ZoneArrivedCommand`) yeni varlık numarasıyla yeni bölgeye giriş: `ZONE_CHANGE`, `INVENTORY`, `COINS`, `SKILL_STATE`, `SKILL_BAR`, `CHAR_STATS`, (varsa) `SKILL_COOLDOWNS`, (varsa) kendi `EFFECT_LIST`'i, görüştekilerin doğuşları. İstemci yükleyip `ZONE_LOADED_REQ(zone)` gönderir; o zamana kadar (en çok 20 s) hareketleri yok sayılır, saldırı / ganimet istekleri düşürülür, canavarlar ona saldırmaz, geçitler ateşlenmez; MOVE_REQ sırası sıfırdan başlar. Yazma başarısızsa oyuncu durduğu yere, eski bölgeye geri konur (`ZONE_CHANGE` Failed) | `ZoneChangeTests` (sıra, yazma tutulurken), `ZoneServerTests` (tel), `PostgresZonesTests` |
| Kalıcılık ve çökme | Karakter tek satır (`characters.zone_id` + konum): varış yeni bölgede görünmeden önce yazıldığı için çökme ya eski bölgedeki son kayıtta ya varışta bırakır, asla ikisinde ya da hiçbirinde. Geçiş sürerken çıkış / kopma yazmayı bekler (başarısızsa durduğu yer yazılır), çift giriş varışı devralır, `World::stop()` varışı bir kez daha yazar. Göç **gerekmedi** (0001–0006'ya dokunulmadı) | `PostgresZonesTests` (istemci ZONE_CHANGE'i aldığı anda satır 2. bölgede; yeniden başlatmadan sonra aynı yerde), `ZoneChangeTests` (çıkış / çift giriş / stop) |
| Yerleşim | Yeni karakterler yurtlarının ırk doğuşunda (Aydor Tanyeri'nin `spawn_aydor`'u, Kargan Demirağız'ın `spawn_kargan`'ı). Kayıtlı bölge yüklü değil, ırka kapalı ya da konum kullanılamaz ise: yurdun ırk doğuşu. Var olan karakterler (hepsi bölge 1'de) yerinde kalır. Ölünce yeniden doğma: öldüğü bölgenin ırk doğuşu (yurtta yurdun doğuşu, Eşik'te Ortaocak'ınki). Test kancası `world.start_zone_override` (her ırkı o bölgeye başlatır) | `ZoneChangeTests` (yerleşim, doğuşlar), `PostgresZonesTests` |
| Protokol | v7: 0x0800 `ZONE_CHANGE_BEGIN` (4 bayt), 0x0801 `ZONE_CHANGE` (68 bayt: sonuç, eski bölge, ENTER_WORLD_RES gövdesi), 0x0802 `ZONE_LOADED_REQ` (2 bayt), 0x0803 `ZONE_GATE_REFUSED` (3 bayt). `ZONE_LOADED_REQ` değişiklik başına bir kez ve ZONE_CHANGE'ten sonra (yoksa BadState), başka bölge numarası protokol ihlali | gidiş-dönüş, boyutlar, reddedilen değerler, çözücü fuzz, sunucu fuzz'ı |
| Araçlar | `vo_admin show-character` bölgeyi adıyla (`zone 2 (Tanyeri)`); `vo_admin move-character <karakter> <bölge> [işaret]` (bölge id ya da anahtar, varsayılan `spawn_<ırk>`, ırka kapalı bölge / bilinmeyen bölge / eksik ya da yürünemez işaret çıkış 3). botclient `zones` senaryosu (her iki ırk; `--phase pair`; `--phase check`), yük testinde `--zones` (her beşinci bot Eşik ile yurdu arasında gidip gelir, bir beşte biri yurduna gidip kalır). Paketleme `zones.toml`'u ve zones.toml'daki her araziyi alır | `integration.zones` / `integration.db.zones`, `integration.load` |
| İstemci (asgari) | v7 ortak sabitle; `ZONE_CHANGE_BEGIN` gelince MOVE_REQ gönderilmez; `ZONE_CHANGE` dünya sahnesini yeni bölgenin arazisiyle baştan kurar (arazi yolu `terrain/zone<N>/zone<N>.vterrain`, zones.toml'daki yolla aynı), sahne hazır olunca `ZONE_LOADED_REQ`; `ZONE_GATE_REFUSED` savaş iletisi satırında "<Bölge> yalnızca Aydorlulara / Karganlara açık."; afiş, etiketler, yükleme yazısı yok (istemci ajanı) | derleme, akışlar |
| Eski akışlar ve senaryolar | Ortaocak için yazılmış uçtan uca senaryolar (`RunScenario.ps1`) ve UI akışları (`run-ui-flows.ps1`) ürettikleri yapılandırmaya `world.start_zone_override = 1` yazıyor: yeni karakterleri M13 öncesindeki gibi Ortaocak'ta başlıyor; yalnız `zones` yurtlarla koşuyor | ctest, akışlar |

**Seçilen kurallar (özet)**: tetik = işaretin 2,5 m yarıçapı (sanat ajanının varışları kapıdan ~8 m; varış
yarıçap + 1 m'den yakınsa sunucu açılmaz), tuşsuz ve seviye tetikli; yeniden kurulma = bölgede
belirdikten 2 s sonra ve o zamandan beri bütün geçitlerin dışına çıkmış olmak; ölü geçmez; savaşta
geçmek serbest, büyü bedelsiz biter, oturan ayakta varır, ganimet penceresi kapanır, istekler oturumda
sürer; ret: içeride kalış başına bir paket, en az 3 s arayla; yeniden doğma öldüğü bölgenin ırk
doğuşunda; ırk kısıtı yalnız sunucuda.

**Yük testi** (Release, bu makine, gerçek üç bölge, 200 bot, 60 s, `world.start_zone_override = 1`,
`--zones`: 40 savaşçı, 40 gidip gelen, 40 yurduna gidip kalan, gerisi köyde): 8,5 s'de hepsi dünyada,
0 düzeltme, 0 kopma; pencerede 256 bölge değişikliği, 0 hatalı, 38 ret (yolu öteki ırkın geçidinden
geçen botlar); sonda bölge 1/2/3'te 136/31/33 bot. Dünya tiki 10 s'lik pencerelerde ortalama
1,14–1,74 ms, p99 ≤ 2,55 ms, en çok 2,87 ms (ısınma sırasında 12,0 ms), 0 yavaş / atlanan tik (M12:
1,52–1,60 ms tek bölgede; oyuncular artık üç bölgeye dağıldığı için görüş çiftleri azaldı); istemci
başına 46 KB/s, 1022 paket/s; PING p50 0,27 ms, p99 5,7 ms; 26 öldürme, 26 ganimet alma.

**Testler**: `build.ps1 -Test` gerçek arazilerle 585/585 (test veritabanı olmayan worktree'de 12
`integration.db.*` atlanıyor; elle `vedatonline_test`'e karşı koşuldu, aşağıda). Yeni: `ZonesTests`
(5 durum), `ZoneChangeTests` (11), `ZoneServerTests` (2), `PostgresZonesTests` (2), fuzz eklemesi,
`integration.zones`, `integration.db.zones`; `integration.load` artık `--zones` ile 40 s.

**Değiştirilen eski testler**: `MessagesTests` (HELLO'daki sürüm baytı ve `kProtocolVersion` 7),
`SkillsTests` (`kProtocolVersion >= 6`), `FakeLink` (ZONE_CHANGE_BEGIN'i bir istemci gibi işler),
`TestServer` (bölge kurulumu seçeneği), `RunScenario.ps1` (`start_zone_override = 1`, noterrain
iletisi aynı), `verify-package.ps1` (yeni günlük satırı "world: zone 1 (Ortaocak) from").

**Sapmalar**: `vo_admin move-character` çevrimiçi karakteri reddedemiyor (veritabanında çevrimiçi bilgisi
yok): `set-level` gibi uyarı yazıyor. Bölge 1'in görünen adı "Ortaocak" kaldı (istemci ve akışlar
buna bakıyor); "Eşik" bölge anahtarında (`esik`) ve alt yazıda. Başarısız varış yazımı için ek bir
sonuç kodu (`ZONE_CHANGE` Failed, oyuncu eski yerine) eklendi. `ZONE_LOADED_REQ` istemciden onay
olarak eklendi (MOVE_REQ sırası yeniden başlar). Etkiler bölge değişiminde taşınıyor (sözleşmede
belgelendi).

**Bilinen sınırlar**: asgari istemcide afiş, geçit etiketi, "Geçitten geçiliyor…" yazısı ve Failed
iletisi yok (istemci ajanı); `ZONE_CHANGE`'te sahne baştan kuruluyor (arazi ve varlıklar yeniden
yükleniyor). Bekleme ve efektler sunucu yeniden başlayınca kaybolur (M12 gibi). Yük testindeki
gidip gelenlerin yolu geçitlerden kaçınmıyor (ret sayısı bu yüzden). 20 s yükleme korumasından sonra
bile istemci ZONE_LOADED_REQ göndermeden hareket edemez.

**Akışlar**: `run-ui-flows.ps1` gerçek üç bölgeyle 76/76 koşu geçti (bütün eski akışlar,
`start_zone_override = 1` ile Ortaocak'ta). Yeni `zone_gate` akışı (yurtlar açık, `StartZone = 0`):
Aydor Tanyeri'de başlar, kuzey yolundan `gate_esik`'e yürür, sahne Eşik'le yeniden kurulur
(`arrive_aydor`'da), `gate_aydor`'dan Tanyeri'ye döner (`arrive_esik`'te), karakter seçimine çıkar —
1/1 geçti (tam koşudan sonra eklendi; eklenen `zone<N>` / `zonechanging` istemci durumları yalnız bu
koşuda denendi).

**Veritabanı (elle, `vedatonline_test`)**: worktree'de ctest `integration.db.*`'ı atlıyor; ana çalışma
kopyasının yapılandırmasıyla 12 `integration.db.*` senaryosunun hepsi (zones dahil: `show-character`
"zone 2 (Tanyeri)", `move-character` ırk reddi / bilinmeyen bölge / `arrive_kargan` ve sonraki girişte
tam orada) ve `vo_tests "[db]"` (28 durum, `PostgresZonesTests` dahil) geçti. `vedatonline`
veritabanına bağlanılmadı, göç uygulanmadı (göç yok).

## 27. M13 "Üç Yurt" — istemci durumu (2026-11-17)

Sözleşme: `docs/PROTOCOL_ZONES.md` §7 (istemci sunumu). Ayrıntılar `docs/CLIENT_RENDERING.md` §14'te,
akışlar `docs/BUILDING.md`'de. Protokole, sunucuya, `shared/` koduna, `gamedata/` verisine ve göçlere
dokunulmadı; istemci, testler, akışlar ve belgeler değişti.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Model | `client/world/ZoneView.h`: bölge adı ve alt yazısı `zones.toml`'dan (`game::zones()`, yoksa `shared/game/Zones.h` tablosu, yoksa "Bölge N"), §7'nin sabit metinleri, kapı görünümleri (işaret, yön, hedef, ırka açık / kapalı, etiket, ipucu, işaretin 4 m içindeki `props/gate_*` yerleşimi ve onun perdesi), en yakın kapı, etiket solması (32 → 40 m), bölge başına yükleme ekranı söz satırları (Tanyeri ve Demirağız için yeni, LORE.md'ye uygun) | `ZoneViewTests` (5 durum, 78 doğrulama) |
| Geçit ekranı | `ZONE_CHANGE_BEGIN`: hedef nokta, yol, bekleyen tıklama, basılı yürüme, kamera / yetenek sürükleme, hedef, otomatik saldırı, kovalama, sıradaki yetenek ve büyü bırakılır; ganimet penceresi paketsiz kapanır; bütün uzak varlıklar, efektler, oklar, sanat ve sayılar silinir; Esc menüsü kapanır; arayüz gizlenir ve dünya yerine yükleme ekranı çizilir: hedef bölgenin adı, alt yazısı, **"Geçitten geçiliyor…"**, gidip gelen çubuk ve hedef bölgenin bir söz satırı. Bu sürede dünya simüle edilmez, girdi yok sayılır; hedef bölgenin arazisi arka planda okunmaya başlar (yeni sahne çoğunlukla önbellekte bulur). Donmuş dünya ya da eski bölgeden bir kare görünmez | `zones` akışı (04, 09, 16) |
| Yükleme | `ZONE_CHANGE(Ok)`: yeni sahne; ekran aynı düzende ad, alt yazı, **"<Bölge> yükleniyor…"** ve dosya ilerlemesi; hazır olunca `ZONE_LOADED_REQ`, afiş ve savaş iletisi satırında **"<Bölge> bölgesine geçtin."** (istemcide kayan bir savaş günlüğü yok; sistem satırı bu satır) | `zones` (05, 06, 17, 18) |
| Başarısız geçiş | **Asgari istemcinin hatası giderildi**: `ZONE_CHANGE(Failed)` gelince de sahne baştan kuruluyor, afiş ve (sonradan eklenecek) geçtin satırı çıkacaktı, ret metni hiç yoktu. Şimdi hazır bir sahnede aynı bölgeye dönüşte sahne korunuyor: yeni varlık numarası ve yerle `WorldModel::enter` (arazi kalıyor), savaş durumu ve efektler sıfırlanıyor, `ZONE_LOADED_REQ`, arayüz geri, afiş yok, **"Geçit şu an kullanılamıyor, tekrar dene."**; sahne henüz yükleniyorsa yeniden kuruluyor, yine aynı satırla ve afişsiz | `zones` (09, 10; otomasyonun yerel benzetimiyle, aşağıda) |
| Afiş | Üstte ortada (hedef çerçevesinin yeri: girişte hedef yok, hedef seçilince afiş hemen söner), toplam **4 s** (0,6 giriş, 2,6 bekle, 0,8 sönme; önceden 5,2 s), ad, altın çizgi, alt yazı; HUD paneli ile düğmeler arasına ad sığmazsa 30 px'e küçülür, yine sığmazsa panellerin altına iner. Önceden alt üçte birdeydi ve M12'nin yetenek çubuğu alt yazısını örtüyordu. HUD'un sağ üst bölge etiketi bölge adı | `zones`, `zones_refused` (1024x640 @150 % dahil), `polish` |
| Kapılar | Her kapıda perde (`effects/gate_veil_<görünüm>`, kapı yerleşiminin kendi matrisiyle, `addEffectMesh`, opaklık 0,5 ± 0,08 nefes alır; üç perde her bölgede yükleniyor); 40 m içinde kapının 0,7 m üstünde **"<Bölge> Geçidi"** (açık kapı altın, kapalı pas rengi; tepeler gizler, evler gizlemez); 8 m içinde altta hap biçiminde **"Geçide yürü: <Bölge>"** ya da **"<Bölge> yalnızca Aydorlulara / Karganlara açık."** (savaş iletisi aynı sözü söylerken çizilmez). Kapı nesneleri üç bölgede yerinde ve görünür | `zones`, `zones_refused` |
| Ret | `ZONE_GATE_REFUSED`: "<Bölge> yalnızca Aydorlulara / Karganlara açık." savaş iletisi satırında; karakter yerinde | `zones_refused` (içeride beklerken ikinci ret yok, çıkıp girince ikinci ret) |
| Karakter kartı | Kart satırı bölge adını `zones.toml`'dan alıyor: "Aydor · Savaşçı · Tanyeri", "Kargan · Savaşçı · Demirağız" | `zones` (11, 12) |
| Yeni canavarlar | Anız Yanığı (Tanyeri) ve Kırağı Kayası (Demirağız) oyunda: ad levhası "Ad · Sv. N", seçim halkası, saldırı ve ölüm klipleri, ganimet torbası ve penceresi (Akçe + Kül Tozu / Çömez Bıçağı). Yanık Nöbetçi ve Donuk Ocakçı yalnız uzaktan görüldü | `zones_hunt` |
| Düzeltmeler | Ganimet torbası ceset üstündeyken cesedin etiketi artık çizilmiyor (iki etiket üst üste biniyordu; M10'dan beri, yeni canavarlarda görüldü). M12 hatası: Yetenekler penceresinin yarı saydam kaplamasının altına opak zemin kondu (F3 paneli içinden görünmüyor) | ekran görüntüleri |
| Otomasyon | `scene` komutları `gates`, `expectgates`, `walktogate <hedef|işaret> [m]`, `facegate`, `expectgatelabel` / `expectnogatelabel` (başarısızlıkta her kapının uzaklığı ve etiketin neden çizilmediği), `expectgatehint` / `expectnogatehint`, `expectzonemessage`, `expectveils`, `expectzoneseen`, `zonehold <geçit s> <yükleme s>` (sonraki geçişlerde iki ekran en az o kadar kalır; `UiScene::holdNetEvents` kuyruktaki paketleri bekletir; oyuncular hiç beklemez), `zonefail <hedef>` (ZONE_CHANGE(Failed)'in **yerel benzetimi**: BEGIN ve mevcut varlık / yerle bir Failed istemcinin kendi kuyruğuna konur; sonra ZONE_LOADED_REQ gönderilmez); durumlar `gatescreen`, `gatehint` / `nogatehint`, `gatelabel`, `nobanner` | akışlar |

**Akışlar** (`run-ui-flows.ps1`):

| Akış | Ne denetliyor |
|---|---|
| `zones` (yurtlar açık, tek hesap, `--fixed-dt`) | 1: Aydor Tanyeri'de (afiş, HUD adı, 1 kapı), kuzey yolundan kapıya: 26 m'de etiket ve perde, ipucu yok; 6 m'de ipucu; geçiş: geçit ekranı, "Ortaocak yükleniyor…", Ortaocak'ta afiş + "Ortaocak bölgesine geçtin.", `arrive_aydor`'da, 2 kapı; geri bakınca "Tanyeri Geçidi", ipucu, 2 perde; kapıdan Tanyeri'ye dönüş (`arrive_esik`, geçtin satırı, afiş); benzetilen başarısız geçiş: geçit ekranı, aynı yerde, afiş yok, satır, sonra yürüyebiliyor; karakter seçiminde "Tanyeri". 2: aynı hesaba Kargan: listede iki yurt; Demirağız'da afiş, vadiden kapıya (etiket, perde, ipucu), geçit / yükleme ekranları, Ortaocak'ta `arrive_kargan`'da satır, "Demirağız Geçidi" ve ipucu, Demirağız'a dönüş |
| `zones_refused` (1280x720 ve 1024x640 @150 %) | Ortaocak'ta Aydor: kartta "Ortaocak", afiş, Kargan kapısında kapalı renkli etiket, kapalı ipucu, içeri girince bir ret ve satır, hâlâ bölge 1'de; içeride beklerken ikinci ret yok, çıkıp girince ikinci ret |
| `zones_hunt` (gerçek zaman) | Tanyeri'de Aydor bir Anız Yanığı'nı, Demirağız'da Kargan bir Kırağı Kayası'nı öldürür: hedef, iki yönlü vuruş, ceset, ganimet penceresi |

Ekran görüntüleri: `docs/screenshots/m13/01…32_*.png` (Blender çizimlerinin adları değişmedi; hepsine bakıldı).

**Son tam koşu** (Debug, bu dalın son kodu): `build.ps1 -Test` 590/590 ctest (585 + 5 yeni `ZoneViewTests` durumu; test veritabanı ortam değişkeni olmadan 12 `integration.db.*` atlanıyor, önceki bölümlerdeki gibi); `run-ui-flows.ps1` 83/83 koşu geçti (eski 77 + yeni 6: `zones` 2, `zones_refused` 2, `zones_hunt` 2; veritabanlı akışlar `vedatonline_test` şemalarıyla gerçekten koştu). Geliştirmede düşmeler: test adındaki "§" (ASCII yapıldı), Demirağız yolundaki mangalda takılan yürüyüş (akış iki parçalı), afiş sönmeden benzetilen başarısızlık (akış afişin sönmesini bekliyor).

**Değiştirilen eski dosyalar**: `UiScene` (sanal `holdNetEvents`, varsayılan hayır), `AccountState`
(`zoneChangeFailed`), `SceneWidgets` (kart bölge adı), `SkillWindow` (opak zemin), `WorldScene`
(afişin yeri ve süresi, yükleme ekranı düzeni, ceset etiketi, M13 asgari kodu `WorldZones.cpp`'ye
taşındı). Eski akışlara ve testlere dokunulmadı.

**Bilinen eksikler / zayıf noktalar**:
- Başarısız geçiş gerçek sunucuyla denenmedi (sunucu ancak veritabanı yazımı başarısız olursa gönderir);
  akış istemci içi benzetimle deniyor. Failed geldiğinde sahne henüz yükleniyorsa yeniden kurulan yol hiç
  koşmadı.
- Yükleme ekranının arka planı her bölgede aynı giriş resmi (Ortaocak benzeri köy); bölgeye özgü resim yok.
- Bölgeye özgü gök / sis rengi (WORLD_ZONES_ART.md öneriyor) uygulanmadı; üç bölge aynı ışıkta.
- Kapı etiketi evlerin arkasından da görünür (bilinçli: tabela gibi); perdeler parlak ve az saydam
  görünüyor (ışıma gücü 2), Aydor perdesi neredeyse opak altın.
- Ortaocak'ta `arrive_kargan`'dan bakınca batıdaki yamaçta bir Kor Bekçisi (Sv. 6, Elit) görünüyor; yeni bir
  Kargan ilk geçişte ona yakın çıkıyor (canavar yerleşimi, harita verisi; dokunulmadı).
- Demirağız'da kasabadan güneye yol bulmalı yürüyüş yol kenarındaki bir mangalda takılıyor (istemci
  hareketçisi 0,5 s takılınca vazgeçiyor); akış iki parçada yürüyor. Elle yürümeyi etkilemez.
- Kayan bir savaş günlüğü olmadığından "geçtin" satırı 2,6 s'lik savaş iletisi.
- Buz Kabuğu kabarcığının tel kafes görünümü (M12, sanat) düzeltilmedi.
- Yanık Nöbetçi ve Donuk Ocakçı ile dövüşülmedi.

**Doğrulanmadı**: Release derlemesi; iki istemcinin aynı anda bölge değiştirmesi (öteki oyuncunun
kaybolup belirmesi); 20 s yükleme korumasının dolması; başka ekran kartları.
