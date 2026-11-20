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

## 28. HUD — oyun ekranının arayüzü (2026-11-17)

İstek: alttaki gri yardım satırları ("X: otur / kalk" …) amatör görünüyordu; yerine bitmiş bir oyunun
HUD'u. Ayrıntılar `docs/CLIENT_RENDERING.md` §15'te, tuşlar `docs/BUILDING.md`'de. Protokole, sunucuya,
`shared/` oyun koduna, `gamedata/` verisine ve göçlere dokunulmadı.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Yardım satırları | İki satır (`world.help`, `world.help.combat`) ve sağ üstteki üç yazılı düğme kaldırıldı | `hud` akışı, bütün ekran görüntüleri |
| Alt çubuk | Altta ortada tek çerçeveli parça (pencerelerin koyu zemini, bronz kenar, üst kenarda altın çizgi ve elmaslar, köşe süsleri): solda dört eylem düğmesi (Saldır F, Otur / Kalk X, Ganimet E, Yürü / Koş R), ayraç, 8 yetenek yuvası (davranış aynı: 1-8, tık, sürükle, sağ tık, bekleme süpürmesi), ayraç, beş menü düğmesi (Envanter I, Künye C, Yetenekler K, Tuşlar H, Menü Esc); altında deneyim çubuğu ("Seviye N · xp / sonraki · %p", onda bir çentikler; üstüne gelince tam sayılar ve kalan). Düğmeler tuşa basmakla aynı işi yapar; köşede tuş rozeti; açık / basılı durum yanar (oturmak, saldırmak, açık pencere), Yürü / Koş simgesi yürüyüşü gösterir (çizme / ayak izi), Ganimet erişilebilir kese varken nabız gibi parlar, Künye / Yetenekler'de harcanmamış puan altın dairede | `hud` akışı (her düğme tıklanarak) |
| İpuçları | Her HUD düğmesinde: "Ad (Tuş)", bir satır, varsa durum ("Şu an: oturuyorsun", "Şu an: yürüyüş (Shift basılıyken koş)", "Önce bir canavar seç…", puanlar); kutu çubuğun üstünde | `HudViewTests`, `hud` (`hud.tooltip`) |
| Tuşlar penceresi | H, çubuk düğmesi ya da "X" ile; gruplar Hareket, Kamera, Diğer · Dövüş, Pencereler; her satırda tuşlar tuş başlığı biçiminde ve bir açıklama. Eski yardım satırlarındaki her denetim içinde (basılı tut: oraya git, Tab, Shift ile yürü, PgUp / PgDn, tekerlek, F3 …). Ekrana sığmazsa içerik küçülür; opak zemin | `HudViewTests` (eski satırların her parçası aranıyor), `hud` (`controls.text`) |
| Esc sırası | ganimet penceresi → Tuşlar → Yetenekler → Künye → envanter → hedef → menü (`escapeActionHud`) | `HudViewTests`, `hud` |
| Oyuncu çerçevesi | Sol üstte: seviye madalyonu (altın kenar; dövüşte kırmızı-altın ve hafif nabız), ad, "ırk · sınıf", sayılı can ve mana çubukları; etki simgeleri altında | ekran görüntüleri |
| Hedef çerçevesi | Ad (seviye rengi), "Sv. N" çipi, elitlerde altın "Elit" çipi ve kenar, ikinci satır ("Canavar", "Elit canavar" ya da oyuncunun ırk · sınıfı; ölüyse "Ölü"), sayılı can çubuğu; etki simgeleri altında | `hud` (15) |
| Sağ üst | Bölge adı yumuşak koyu bir zemin üstünde, "● Güvenli Bölge" hemen altında | ekran görüntüleri |
| F3 | Bilgi paneli oyuncu için varsayılan kapalı; akışların okuduğu yazılar gizliyken de okunuyor | `hud` (`noinfopanel`), eski akışlar |
| Simgeler | `tools/blender/gen_hud_art.py`: on düğme simgesi (`ui/hud/*.png`, 64²), M12 simgeleriyle aynı SDF ressamı ve mürekkep çizgisi; M12 yuva parçalarının içine çiziliyor; indirme yok, Blender yalnız arka planda. assetc'nin UI kuralı `ui/hud/`'u da kapsıyor | `build-assets.ps1` (`ui\hud\attack.vtex` zorunlu), çizim sayfası `build/art_sheets/hud` |
| Yerleşim | `HudView.h` `computeHudBar` çubuğu mantıksal ekrana göre kurar (tam boyda 843x86; ekran 859 mantıksal pikselden darsa orantılı küçülür — ölçek uydurması 1024x640 @150 %'ü 960x600 yaptığından pratikte olmuyor). Döküm çubuğu, savaş iletisi, kapı ipucu ve ganimet satırları çubuğun üstünden hesaplanıyor (`hudBottomLines`) | `HudViewTests` (dokuz ekran boyunda hiçbir parça çakışmıyor) |
| Pencereler | Künye ve Yetenekler solda, envanter sağda, Tuşlar ortada: önce üstteki HUD ile çubuk arasındaki boşluğa ortalanır; sığmazsa ekranın üstünden çubuğa kadar; o da olmazsa bütün ekrana. Açık bir pencerenin hâlâ örttüğü HUD parçası (çubuk, oyuncu çerçevesi + etkileri, hedef çerçevesi, bölge adı, F3 paneli) pencere kapanana kadar gizlenir. **Eski çakışma giderildi**: 1024x640 @150 %'te (960x600 mantıksal) Künye artık oyuncu çerçevesi ile çubuk arasına sığıyor; Yetenekler (424 yüksek) oyuncu çerçevesinin üstüne geliyor (çerçeve o sürede gizli), çubuğu hiç örtmüyor | `HudViewTests`, `hud` (21-23) |

**Yeni akış** `hud` (tek koşu; 1920x1080 açılır, sonra 1280x720'ye ve 1024x640 @150 %'e küçültülür —
pencere masaüstünden büyüyemediği için büyükten küçüğe): ipuçları (deneyim çubuğu dahil), üç pencere bir
arada, otur ve yürü düğmeleri; 1280x720'de her düğme tıklanarak, Tuşlar ve Esc sırası (Künye açıkken),
Menü düğmesi, pencere düğmeleri, Saldır düğmesiyle dövüş, Ganimet düğmesiyle aç ve hepsini al; köye
dönüş; 1024x640 @150 %'te ipucu, Künye, Yetenekler, envanter, Tuşlar, otur, menü. Ekran görüntüleri:
`docs/screenshots/hud/01…25_*.png` (hepsine bakıldı).

**Değişen eski dosyalar**: `WorldScene` (HUD kurulumu ve yerleşimi, H tuşu, Esc sırası, durumlar, afişin
sağ sınırı), `WorldCombat` (hedef çerçevesi, oyuncu çerçevesi çizimi; can / mana / deneyim çubukları
eski yerlerinden kalktı), `WorldSkills` (yuvalar çubuğun içinde, eski "Yetenekler (K)" düğmesi ve
yardım satırı kalktı, HUD ipuçları), `WorldStats` (eski "Künye (C)" düğmesi kalktı), `WorldLoot`
(ganimet penceresi ve satırları yeni çubuğa göre, `lootReachable`), `WorldZones` (kapı ipucu çubuğun
üstünde), `SkillBar` (`SkillsButton` kalktı, `SkillSlotsWidget::setMetrics`), `LootView.h`
(`EscapeAction::CloseControls`), assetc (`ui/hud/`), `build-assets.ps1`, `build_all.ps1`,
`run-ui-flows.ps1` (`hud` akışı).

**Son tam koşu** (Debug, bu dalın kodu): `build.ps1 -Test` 599/599 ctest (590 + 9 yeni `HudViewTests`
durumu; test veritabanı ortam değişkeni olmadan 12 `integration.db.*` atlanıyor, önceki bölümlerdeki gibi);
`run-ui-flows.ps1` 84/84 koşu geçti (eski 83 + `hud`; veritabanlı akışlar `vedatonline_test` şemalarıyla
koştu). Bu tam koşudan sonra yalnız puan işaretinin yeri değişti (düğmenin kendi sağ üst köşesine; komşu
düğmenin tuş rozetini örtüyordu) ve test adları ASCII yapıldı; ardından `stats` (3 koşu) ve `hud`
yeniden koştu ve geçti, ctest yeniden 599/599. Geliştirmede düşmeler: 1024x640 @150 %'in gerçekte
960x600 mantıksal olduğu (UiScale.h) akışta görüldü ve beklentiler düzeltildi; savaştan hemen sonra
oturmak "Şu an oturamazsın" (akış köye dönüyor); "Tuşlar" içeren test adları ctest süzgecinde eşleşmedi.

**Eski akışlara dokunulmadı**: yardım satırlarını ya da taşınan düğmeleri okuyan bir akış yoktu
(`clickon world.inventory`, `hoveron world.skills` + `skills.tooltip` aynı adlarla çalışıyor; F3
panelinin yazıları gizliyken de okunuyor).

**Bilinen eksikler / zayıf noktalar**:
- 1920x1080 @%100'de çubuk ekrana göre küçük kalıyor (843 mantıksal piksel; arayüz ölçeği oyuncunun).
- Çok dar mantıksal ekranlarda (859 pikselden dar) çubuk küçülüyor; ölçek uydurması bunu ürettiğinden
  yalnız birim testiyle denendi.
- Yetenekler penceresi 960x600'de oyuncu çerçevesini örtüyor (çerçeve o sürede gizli); pencere 424
  mantıksal piksel yüksek, daha küçük bir düzeni yok.
- Pencereler birbirinin üstüne gelebiliyor (Tuşlar ortada, 1920'de üç pencere aynı anda); bu HUD ile
  çakışma değil ama düzenleyen bir pencere yöneticisi yok.
- Oturan karakterin ad etiketi ayaktaki boyda kalıyor (eski davranış; dokunulmadı).
- Yürü / Koş simgesindeki çizme küçük boyda biraz belirsiz; ipucu ve ayak izi karşıtlığı durumu anlatıyor.
- Saldır düğmesi, hedef yokken de tıklanabilir (tuş gibi); yalnız koyu çiziliyor.

**Doğrulanmadı**: Release derlemesi; gerçek bir ekranda %125 ve %175 ölçek; sürükle-bırak ile yetenek
yuvasına çubuk içinden bırakma (M12 akışları geçiyor, davranış değişmedi).

## 29. M14 "Pazar" — tüccar sanatı durumu (2026-11-17)

Hepsi Blender'da başsız, depodaki betiklerle ve depodaki kaynaklarla üretildi (oyuncu görünümlerinin
mesh'leri, depodaki CC0 animasyon kütüphanesi, prosedürel geometri ve dokular); indirme, dış varlık,
başka oyundan alıntı yok. Anahtarlar, dosyalar, istemci için öneriler: `docs/SHOP_ART.md`. Sunucu /
istemci / ortak koda, `gamedata/`'ya ve bölge yerleşimine dokunulmadı. assetc'ye tek satır: `ui/shop/`
klasörü de UI simge kuralına (premultiplied RGBA8 `.vtex`) eklendi.

| Konu | Ne var | Nasıl doğrulandı |
|---|---|---|
| Tüccarlar (`npcs/<anahtar>.vmesh` + `npcs/<anahtar>/<klip>.vanim`, canavarlarla aynı düzen) | `npc_potion_esik` (`aydor_1`), `npc_smith_esik` (`kargan_0`), `npc_potion_aydor` (`aydor_2`), `npc_smith_aydor` (`aydor_0`), `npc_potion_kargan` (`kargan_1`), `npc_smith_kargan` (`kargan_2`): oyuncu iskeleti (59 kemik), klipler `idle`, `idle_talking`, `interact`; gövde 25k + kıyafet 4.6–4.8k üçgen, tek malzeme, 1024² atlas. İksirciler: yüzü açık başlık + omuz pelerini, uzun bez önlük, şişeli kemer, heybe; demirciler: deri önlük, eldivenler, takke, kemerde çekiç ve kerpeten (Kargan: kızgın külçe). Kasaba renkleri: Ortaocak toprak tonları, Tanyeri beyaz–mavi–altın, Demirağız koyu–demir–kor | `previews/npcs/*`, `docs/screenshots/m14/art_npcs_*.png`, `ShopTests` |
| Tezgâhlar (`props/`) | `aydor_potion_stall`, `aydor_smithy`, `kargan_potion_stall`, `kargan_smithy` (878–6 548 üçgen, Cycles ile pişirilmiş renk / normal / ORM / ışıma); tüccarın duracağı nokta `shop_props_meta.json` → `stand` | `previews/props/m14_props_sheet.png`, `art_stalls.png`, `ShopTests` |
| Eşya simgeleri (`ui/items/`) | `potion_hp_small`, `potion_hp_medium`, `potion_mp_small`, `potion_mp_medium` (128², silah / ganimet simgeleriyle aynı yol); küçük: yuvarlak şişe, orta: uzun, mühürlü, etiketli şişe; dördü aynı çerçevede, küçük şişe yuvada daha küçük | `art_icons_real.png` (72 / 64 / 48 / 40 piksel), `art_icons_enlarged.png`, `ShopTests` |
| Dükkân arayüzü (`ui/shop/`) | `akce` (64²), `tab_al`, `tab_sat` (64², avuç + şişe / sikke, zıt oklar), `marker_potion`, `marker_smith` (128² iğneli yuvarlak levha; baş üstünden 0.45 m yukarıda, ~0.55 m önerildi) | `art_markers.png`, `art_icons_real.png`, `ShopTests` |
| Testler | Yeni `tests/assets/content/ShopTests.cpp` (5 durum: tüccar iskeleti / boyu / bütçesi / malzemesi, klipler ve döngü dikişleri, tezgâhlar, iksir simgeleri — orta şişe küçükten büyük, can kırmızı, mana mavi —, arayüz parçaları); `build-assets.ps1` zorunlu çıktılara beş dosya | `build.ps1 -Test` |

**Yeniden üretme**: `build_all.ps1` (`gen_shop_props.py` `gen_zone_props.py`'den, `gen_npcs.py`
`gen_monsters.py`'den, `gen_shop_icons.py` `gen_loot.py`'den sonra); inceleme çizimleri
`gen_shop_shots.py` (varlık değil). Tezgâhların ikinci çalıştırması haritaları bayt bayt aynı bıraktı.

**Son tam koşu** (Debug): `build.ps1 -Test` 604/604 ctest (599 + 5 yeni `content.*` durumu; test
veritabanı olmadan 12 `integration.db.*` atlanıyor, önceki bölümlerdeki gibi). Bu koşudan sonra üç
iksircinin omuz pelerini daraltıldı ve Aydor demircisinin duruş noktası kaydırıldı; ardından
`build-assets.ps1 -Config Debug` ve bütün `content` testleri (39 durum) yeniden geçti.

**Bilinen zayıf noktalar**: her tüccar bir oyuncu görünümünün yüzünü ve gövdesini taşıyor (kıyafet ve
renk siluetini değiştiriyor, yüzü değil; `npc_smith_kargan` zaten demirci olan `kargan_2`'ye en
yakın); Kargan iksircisinin pelerini geniş kürk yakası yüzünden kutu gibi; yumuşak takkeler basit
kubbe (Eşik demircisininki uzaktan çanak / miğfer gibi okunabiliyor); önlükler kumaş benzetimi olmayan
sert levhalar; kıyafet dokuları gövdenin taranmış görünüşlü dokularının yanında sade; tüccarlar
yürüyemez (üç klip); yeni tezgâhlar M13 nesneleri gibi düşük çokgenli; iksir simgelerinde camın üst
kısmı soluk, 40 pikselde orta şişenin ip ve etiketi kayboluyor.

**Doğrulanmadı**: hiçbiri oyun istemcisinde görülmedi (istemci tüccarları henüz yüklemiyor; yeni klip
adları `idle_talking` ve `interact` istemcinin klip listesinde yok); işaretin oyundaki boyu ve
okunurluğu yalnız Blender çiziminde; tezgâhların bölgelerdeki yeri (yerleşim dosyalarına dokunulmadı).

## 30. M14 "Pazar" — sunucu + ortak kod durumu (2026-11-17)

Sözleşme: `docs/PROTOCOL_SHOP.md` (protokol v8; istemci için §8 "Client spec": satıcılar, dükkân
penceresi, satış, iksirler çantada ve çubukta, bütün Türkçe metinler), kodla birlikte güncel tutuldu.
Sunucu tarafı `docs/SERVER_WORLD.md` §8g'de, veritabanı `docs/DATABASE.md` §3.6'da, testler
`docs/BUILDING.md`'de. Bu bölüm sunucu ve ortak yarıyı kapsar; dükkân penceresi, satıcı modelleri ve
iksir simgeleri istemci ajanının / sanat ajanının işi. İstemcide yalnız asgari değişiklik var (aşağıda).

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Veri | `gamedata/npcs.toml`: altı satıcı (her kasabada bir şerbetçi / şifacı, bir demirci): id, ad, unvan, karşılama sözü, model anahtarı (`npc_potion_esik` … `npc_smith_kargan`), bölge, konum, yön, tür (`merchant`), stok. `shared/game/Npcs.h` katı yükleyici ("dosya:satır: ne": bilinmeyen anahtar, tekrar eden id / model, aralık dışı sayı, boş / uzun metin, boş / 24'ten uzun stok, iki kez geçen ya da değeri olmayan eşya), süreç genelindeki `game::npcs()`. `items.toml`: dört iksir (16 Kızılca Şerbet, 17 Közlü Şerbet, 18 Şafak Çiyi, 19 Ayaz Özü; `kind = "consumable"`, `restore_hp` / `restore_mp`), her eşyanın bir değeri var (başlangıç donanımına 10–60 Akçe eklendi). `loot.toml`: her tablonun sonunda seyrek iksir satırları | `ShopTests` (gönderilen dosya, ~25 bozuk dosya, iksir anahtarları, ganimet satırları) |
| Satıcıların yeri | Her kasabanın ırk doğuşunun 6–12 m yakınında, yol katmanının dışında (splat G ≤ %50), doğuş işaretinden düz yürünebilir bir hatla; her biri kasabanın doğuşuna bakıyor. Sunucu açılışta denetliyor (`game::checkNpcSpot`): bölgenin içinde, kendisi ve 1 m çevresi yürünebilir, yolun üstünde değil, geçitlerden yarıçap + 2 m, `spawn_*` / `arrive_*` işaretlerinden 4 m uzak; değilse çıkış kodu 6 ve satıcıyı, bölgeyi, kuralı söyleyen ileti | `ShopZoneTests` (sentetik arazide her kural), gerçek arazilerde bütün uçtan uca testler |
| Dünyada | Varlık türü 4 (`kEntityTypeNpc`), AOI'den loot kapları gibi; `ENTITY_SPAWN`'da ad ve şablon numarası = NPC id, gerisi 0. Hiç hareket etmez, saldırılamaz (`NotAttackable`; dost yetenek `WrongTargetType`), alan yeteneklerine girmez | `ShopZoneTests`, `integration.shop` |
| Dükkân | Açma: NotFound → YouAreDead → OutOfRange (konuşma menzili 4 m + 0,5); oyuncu başına bir açık dükkân. Satın alma: NotOpen → YouAreDead → OutOfRange → NotInStock (dünya), BadQuantity → NotEnoughCoins → BagFull (oturum, kaydedilmiş çanta ve bakiyeyle); satış: NotOpen → YouAreDead → OutOfRange (dünya), BadQuantity → NoItem → BadQuantity → CannotSell → PurseFull (oturum). 8 m'den uzaklaşınca / ölünce `SHOP_CLOSED`; bölge değişiminde, çıkışta sessiz | `ShopZoneTests`, `ShopServerTests`, `integration.shop` |
| Fiyat | Satıcı eşyayı `value`'suna satar, yarısına (aşağı yuvarlanmış) alır; değeri 0 ya da 1 olan satılamaz (yok). Bir istek bir yığın (en çok max_stack); her satıcı her eşyayı alır | `ShopTests` |
| İksirler | Anında yenileme (zamana yayılmış değil), bütün iksirler için ortak 10 s bekleme; ölü / sersemlemiş / doluyken / beklemedeyken ret. Atomik: dünya kabul edip beklemeyi başlatır, oturum bir birimi tek işlemde düşer, **kayıttan sonra** dünya yeniler (`ITEM_USED` içene ve görenlere). Bekleme yeniden girişte ve bölge değişiminde sürer (yetenek beklemeleri gibi 60 s bellek) | `ShopZoneTests`, `ShopServerTests`, `PostgresShopTests`, `integration.shop` |
| Çubuk | Yuva artık `0x8000 | şablon` ile bir iksir de tutabilir; tüketilir olmayan / bilinmeyen şablon `NotLearned`. Veritabanında −şablon olarak saklanır (`0007_bar_items.sql`); eski çubuklar aynen geçerli | `ShopTests`, `PostgresShopTests` (0007 bir M13 veritabanına), `integration.db.shop` |
| Protokol | v8: 0x0900–0x090B (`SHOP_OPEN_REQ/RES`, `SHOP_CLOSE_REQ`, `SHOP_CLOSED`, `SHOP_BUY_REQ/RES`, `SHOP_SELL_REQ/RES`, `ITEM_USE_REQ/RES`, `ITEM_COOLDOWN`, `ITEM_USED`); dükkân istekleri saniyede 10, `ITEM_USE_REQ` saniyede 10 | gidiş-dönüş, boyutlar, reddedilen değerler, sunucu fuzz'ı |
| Kalıcılık | `ItemWrite::coinsRemoved`: satın alma, satış ve içilen iksir `applyItemWrite` ile tek işlem; bakiye satır kilidi altında düşer, 0'ın altına inecekse (ör. arada `vo_admin set-coins`) hiçbir şey değişmez | `AccountServiceTests` (iki arka uç), `PostgresShopTests` |
| Araçlar | `vo_admin show-character` çubuktaki iksirleri ("Kızılca Şerbet (eşya 16)") ve çantadaki tüketilirleri gösteriyor; `give-item` iksir de verir. botclient `shop` senaryosu (deterministik, canavarsız); test kancası `world.testing_enter_vitals_percent`. Paketleme `npcs.toml`'u alıyor | `integration.shop` / `integration.db.shop` |
| İstemci (asgari) | v8 ortak sabitle; tür 4 varlıklar eskisi gibi yok sayılıyor (çizilmiyor, tıklanmıyor); çubuktaki iksir yuvası tuşa basınca `ITEM_USE_REQ` gönderiyor (yuva boş çiziliyor), reddedilince §8.3'teki satır; dükkân paketleri ve `ITEM_COOLDOWN` / `ITEM_USED` yalnız günlüğe | derleme, akışlar, `ShopTests` (metinler) |

**Seçilen kurallar (özet)**:

| Kural | Seçim |
|---|---|
| Konuşma menzili / dükkânın kapandığı uzaklık | 4 m (+0,5 m tolerans) / 8 m |
| Açık dükkân | oyuncu başına bir; başkasını açmak öncekini sessizce kapatır; ölünce `YouDied`, uzaklaşınca / satıcı görüşten çıkınca `OutOfRange`; bölge değişimi ve çıkış sessiz |
| Satış fiyatı | `value`; geri alış `value / 2` (aşağı); hiçbir eşya aldığından pahalıya satılamaz |
| Satılamayanlar | kuşanılmış eşya (istek yalnız çanta yuvası adlandırır: önce çıkar), gizli (bilinmeyen şablonlu) eşya, satış fiyatı 0 olan eşya (gönderilen veride yok); başlangıç donanımı satılabilir (demirciler de satıyor) |
| Satın alma miktarı | bir istek bir yığın: 1..max_stack (donanım 1); hepsi sığmazsa hiç alınmaz (BagFull) |
| Bakiye | 0..1 000 000 000; satış tavanı aşacaksa hiç satılmaz (PurseFull); alımda bakiye kayıtta da kilit altında denetlenir |
| İksir etkisi | anında; `restore_hp` / `restore_mp` en yüksek değere kırpılır |
| Ortak bekleme | 10 s, bütün iksirler; kabul edilince başlar; başarısız yazımda da kalır; yeniden girişte ve bölge değişiminde sürer, sunucu yeniden başlayınca kaybolur |
| İksir retleri | NotUsable, NoItem (oturum) → YouAreDead, Stunned, OnCooldown, AlreadyFull (dünya) → NoItem, ServerError (yazım); oturmak, savaş ve büyü engel değil |
| Çubukta iksir | `0x8000 | şablon`; sahip olmak gerekmez (yuva 0 adetle kalır); veritabanında −şablon |
| Sel sınırları | dükkân istekleri birlikte 10/s, `ITEM_USE_REQ` 10/s; kuyrukta 16'dan fazla bekleyen alım / satım / kullanım ServerError |
| İksir ganimeti | her tablonun sonunda: 1–2. seviye %5 küçük can, %3 küçük mana; 3–4. seviye %4 orta can, %3 orta mana; Kor Bekçisi %50 1–2 Közlü Şerbet |

**Ekonomi** (sözleşme §5): 1–2. seviye canavar başına ≈6,7 Akçe (para + malzeme satışı), 3–4. seviye ≈20.
Seviye başına gelir (öldürme sayısı × ortalama): 1 → ≈27, 2 → ≈54, 3 → ≈160, 4 → ≈200, 5 → ≈280,
6 → ≈380, 7–9 → 600–1800. Üç öldürmede bir şerbet içen karakter 1–2. seviyede gelirinin ≈%40'ını
küçük iksirlere, 3–4. seviyede ≈%13'ünü (küçük) ya da ≈%40'ını (orta) verir; 3. seviye bir silah
(60 Akçe) o seviyenin gelirinin üçte biri kadar: iksir düzenli, donanım ara sıra alınır. 7–9. seviye
harcayabileceğinden çok kazanıyor (ileride onarım / geliştirme gibi bir Akçe çıkışı gerekecek).

**Göç** `db/migrations/0007_bar_items.sql`: yalnız `characters_skill_bar` CHECK'i genişliyor (eleman
0..63 ya da −1..−32767). Var olan satırlara dokunulmuyor (hepsi 0..63, yeni CHECK'i geçiyor); NULL hâlâ
varsayılan çubuk. Gerçek `vedatonline` veritabanına bağlanılmadı, uygulanmadı (müdürün işi).

**Değiştirilen eski testler**: `MessagesTests` (HELLO'daki sürüm baytı ve `kProtocolVersion` 8),
`ZonesTests` (`kProtocolVersion >= 7`), `LootTests` (şablon sayısı 19, Kül Çömezi 5 / Kor Bekçisi 5 düşüş
satırı, 1–8'in değeri artık ≥ 10), `PostgresSkillsTests` (bir M11 veritabanına uygulanan göçler {6, 7};
`-1` artık geçerli bir eşya yuvası, yerine `-32768` reddediliyor), `PostgresStatsTests` (uygulanan göçler
{5, 6, 7}), `WeaponTests` içerik testi (tüketilirlerin modeli yok, malzemeler gibi), sunucu fuzz'ı
(sürüm uyuşmazlığı deseni rastgele sürüm olarak 8'i çekebiliyordu; artık atlıyor; v8 istekleri ve bir
satıcı eklendi), `FakeLink` (dükkân / kullanım izinlerini kaydediyor), `TestServer` (NPC kurulumu
seçeneği), `AccountServiceTests` (eşya yazımına alım / satım bölümü eklendi), `RunScenario.ps1`
(`shop` senaryosu; `start_zone_override` shop için 0; `testing_enter_vitals_percent`).

**Sapmalar**: çubuktaki iksirler `SKILL_BAR`'ın aynı u16 düzeninde (`0x8000 | şablon`), ayrı bir paket
yok. Dünya iksiri ancak oturumun yazımı kaydedildikten sonra yeniler (bir tik gecikme; ölen oyuncunun
birimi gider, can gelmez). Botların deterministik iksir içebilmesi için test kancası
`world.testing_enter_vitals_percent` eklendi (yalnız `shop` senaryosunun yapılandırmasında 10). Sersemletme
denetimi var ama v8'de oyuncuyu sersemleten bir kaynak yok. Dükkân / kullanım istekleri bölge değişirken
sessizce düşüyor (loot istekleri gibi); istemci `ZONE_CHANGE_BEGIN`'de bekleyenleri unutmalı.

**Bilinen sınırlar**: dükkân penceresi, satıcı modelleri ve iksir simgelerinin çizimi istemci ajanının
işi (asgari istemci tür 4'ü çizmiyor; sanat artık main'de: `npcs/<anahtar>.vmesh`, `ui/items/potion_*`,
`ui/shop/`). Tezgâh yerleşimi (`props/{aydor,kargan}_{potion_stall,smithy}`) yapılmadı: arazinin yürüme
maskesi Blender'da (`gen_zones.py`) üretiliyor, yerleşim satıcıların yerini de değiştirirdi; istemci /
sanat ajanına bırakıldı. Bekleme süresi sunucu yeniden başlayınca kaybolur. 7–9. seviyede Akçe birikiyor
(Akçe çıkışı sonraki işlere). Yük testi botları dükkân kullanmıyor (yalnız NPC'leri görüyorlar).
Gerçek `vedatonline` veritabanına 0007 uygulanmadı.

**Testler**: yeni `ShopTests` (11 durum), `ShopZoneTests` (7), `ShopServerTests` (2), `PostgresShopTests`
(3), fuzz'da v8 istekleri, `integration.shop`, `integration.db.shop`; sanatın getirdiği içerik testi
`content.` ShopTests da koşuyor.

**Son tam koşu** (Debug, main'in M14 sanatı (ffeb8e2) birleştirildikten ve `build-assets.ps1` yeniden
koşulduktan sonra): `build.ps1 -Test` 629/629 ctest (worktree'de test veritabanı ortam değişkeni
olmadığı için 13 `integration.db.*` atlanıyor; `Postgres*` birim testleri ana kopyanın yerel
yapılandırmasıyla `vedatonline_test`'e karşı koştu); `run-ui-flows.ps1` 84/84 koşu geçti (yeni akış yok;
eski akışlar NPC'li sunucuya karşı değişmeden geçiyor). 13 `integration.db.*` senaryosunun hepsi elle,
ana çalışma kopyasının `config/server.local.toml`'unun test bağlantısıyla, kendi `vo_it_*` şemalarında
geçti (shop dahil: `show-character` çubuk ve tüketilirler, yeni sunucu sürecinde aynı durum). İlk tam
koşuda (birleştirmeden önce) üç eski test düştü ve düzeltildi: `LootTests` (1–8'in değeri 0
bekleniyordu), içerik testi (tüketilirin modeli yok), sunucu fuzz'ı (rastgele "yanlış sürüm" 8'i
çekti). Release derlemesi ve 200 botluk yük testi bu turda koşulmadı.

## 31. M14 "Pazar" — istemci durumu (2026-11-17)

Sözleşme: `docs/PROTOCOL_SHOP.md` §8 (istemci sunumu). Ayrıntılar `docs/CLIENT_RENDERING.md` §16'da, tuşlar ve
akışlar `docs/BUILDING.md`'de. Protokole ve sunucu kurallarına dokunulmadı. Veri değişikliği: satıcıların
yerleri (`gamedata/npcs.toml`) ve iki yurdun tezgâh yerleşimi (zone 2 / 3 yürüme maskesi ve nesneleri, Blender
başsız). Göç yazılmadı, uygulanmadı; yalnız `vedatonline_test` kullanıldı.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Satıcılar dünyada | Tür 4 varlıklar `WorldModel`'e girmiyor: ayrı bir `world::NpcBook` (npcs.toml kaydıyla); hedef, uzak oyuncu, Tab adayı değil, saldırı / yetenek isteği gitmez. `npcs/<anahtar>.vmesh` + klipler (`AssetCache::requestNpc`, canavar düzeni), boy 1 (oyuncularla aynı ölçek). Yeni klip adları `idle_talking`, `interact` (`CharacterClip` / `AnimState`; oyuncularda dosya yok, istenmiyor). Dükkânı bizimle açıkken `idle_talking` ve bize döner; her alış / satışta bir `interact`. Ad levhası (turkuaz, oyuncu / canavar renklerinden ayrı) ve altında "‹Unvan›"; başın üstünde işaret (`marker_potion` / `marker_smith`, 0,55 m'nin ekran boyu, 30–52 px, levhanın hep üstünde, 40 → 60 m'de söner); üzerine gelince parlama, ayakta halka, "<Ad> — <Unvan>" / "Konuşmak için tıkla" | `shop` (01–03), `shop_homes` (18, 19, 24, 27, 28, 32), `ShopViewTests` |
| Konuşma | Sol / sağ tık (önde başka varlık yoksa) ya da E (yakında ganimet yoksa, 6 m) konuşma menziline yürür (3,2 m, gerekirse yol bulur), `SHOP_OPEN_REQ`; başka bir satıcıya tıklamak açık dükkânı kapatır | `shop`, `two_clients_shop` |
| Dükkân penceresi | Solda (HUD yerleşim kuralıyla), envanter sağda yanında açılır (dükkân açtıysa onunla kapanır); Künye / Yetenekler yer açar. Başlık ad, "‹Unvan›", karşılama tırnak içinde (araç takımında italik yok), satırlar: simge, ad (nadirlik rengi), ikinci satır ("Can +70", "Saldırı +9 · Sv. 3", karşılanmayan "Gereken seviye: 3" / "Yalnız Büyücü" kırmızı), fiyat Akçe sikkesiyle (yetmiyorsa kırmızı); satış alanı (`tab_sat` + "Satmak için çantada sağ tıkla ya da eşyayı buraya sürükle."; sürüklerken yanar), ileti satırı, canlı "Akçe:". İstek yoldayken satırlar bekler ("Bekleniyor…"). İki pencereli düzen seçildi (sekmeli değil): satış, açık envanterde sağ tık olarak hep görünür | `shop` 04–08, `shop_homes` 20–23, 29, 33 |
| Alış / satış | Yığın: miktar kutusu ("Kaç tane?", oklar, tekerlek, PgUp/PgDn, rakam yazma, "Toplam: n Akçe", Enter / Esc); 50 Akçe üstü donanım "… ödensin mi?". Satış: sağ tık / sürükleme; yığın "Kaç tane satılsın?" (varsayılan bütün yığın, "Alınacak: n Akçe"); nadir ya da 40+ Akçe'lik eşya "… Akçe'ye satılsın mı?"; kuşanılmış eşya bırakılınca "Önce çıkarmalısın."; değeri 0 olan "Bu satılamaz." (istek yok). Dükkân açıkken çanta ipucu "Satış: n Akçe" / "Satılamaz". Alış / satışın ITEM_UPDATE / COINS'i ganimet satırı üretmez, eşya taşımalarını da kapatmaz | `shop`, `shop_homes` |
| Kapanma | Esc sırası: ganimet → **dükkân** → Tuşlar → Yetenekler → Künye → envanter → hedef → menü; X; uzaklaşınca `SHOP_CLOSED` ("<Ad> çok uzakta kaldı."; gelmezse istemci 9,5 m'de kendisi kapatır); ölünce; satıcı görüşten çıkınca; bölge değişiminde ve çıkışta sessiz (yoldaki istekler unutulur) | `shop` 14, `ShopViewTests` |
| Metinler (§8.3) | Hepsi `world::shopResultText`, `buyOkText`, `sellOkText`, `shopClosedText`, `itemUseResultText`; pencerenin ileti satırında ve çubuğun üstündeki satırda | `ShopViewTests` (her metin birebir), akışlar |
| İksirler | Çantada çift / sağ tık içer (dükkân açıkken sağ tık satar); çantadan çubuğa sürükle (`0x8000 | id`; başka eşya: "Çubuğa yalnız iksir konabilir."). Yuva: simge, çantadaki toplam adet sağ altta (beyaz; bitince kırmızı 0, gri simge, yuva kalır), ortak bekleme süpürmesi çubukta ve çantada (`ITEM_COOLDOWN`; 500 ms GCD uygulanmaz); 0 iken tuş: "Elinde <Ad> kalmadı." (istek yok). İpucu: ad, "Tüketilebilir", "Can: +n" / "Mana: +n", "Bekleme: 10 sn (bütün iksirler)", açıklama, "n / 20", çubukta "Çubuktan kullan: <tuş>", dükkân açıkken "Satış". `ITEM_USED`: içenin üstünde yeşil "+hp", mavi "+mp" (başka oyuncu için de) | `shop` 10–13, `shop_homes` 30–31, `two_clients_shop` 35–36 |
| Ganimet (§8.5) | İksirler ganimet penceresinde ve "Kızılca Şerbet alındı" satırıyla (M10 satırı; sözleşmedeki "+2 Kızılca Şerbet" örneği M10'un mevcut biçimiyle farklı, M10 biçimi korundu) | `shop_homes` 34 |
| Tezgâhlar | `gen_zones.py` `SHOP_STALLS`: Tanyeri `aydor_potion_stall` (234, 281), `aydor_smithy` (258, 273); Demirağız `kargan_potion_stall` (240.5, 336), `kargan_smithy` (276.5, 346), ırk doğuşuna dönük, en son yerleştirilir (başka hiçbir şey kımıldamaz); bölge 2 / 3 yeniden üretildi (aynı girdiyle bayt bayt aynı çıktığı önce denendi), önizlemeler güncellendi. Satıcılar `stand` noktasında **duramaz**: sunucunun kuralı (1 m çevresi yürünebilir) tezgâhın ayak izinin içinde sağlanamaz; tezgâhın yanında (merkezden ~4 m, 0,6 m önde, tezgâhın yönünde) duruyorlar. Ortaocak'ta batı ve doğu pazar tezgâhlarının yanına taşındılar | `ShopZoneTests`, `integration.shop`, içerik testleri, akışlar |
| Tuşlar penceresi | Yeni "Pazar" grubu (satıcıya tık, E, dükkân açıkken sağ tık sat, çift tık iksir iç) | `HudViewTests`, `hud` |

**Yeni akışlar** (`run-ui-flows.ps1`, `ServerOpts.EnterVitals` → `world.testing_enter_vitals_percent`; gerçek
zaman, iksir beklemesi sunucu saati): `shop` (Ortaocak, 1280x720 ve 1024x640 @150 %, sunucu her koşuda yeni),
`shop_homes` (test veritabanı; 4 koşu: oluşturma, Tanyeri, Demirağız 1024x640 @150 %, ganimetten iksir),
`two_clients_shop` (iki istemci). Ekran görüntüleri: `docs/screenshots/m14/01…36_*.png` (sanat çizimleri `art_*`
korundu; hepsine bakıldı).

**Değiştirilen eski dosyalar / testler**: `WorldScene` (tıklama / E / Esc / yerleşim / durumlar / komutlar,
`camera at:` satıcıları da hedefler), `WorldSkills` (çubukta iksir yuvası, asgari v8 kodu kaldırıldı),
`WorldLoot` (alış / satış sırasında ganimet satırı yok, dükkân bakiyesi), `WorldZones` (bölge değişiminde
dükkân unutulur), `InventoryWindow` (kanca: sağ / çift tık, dışarı bırakma, ek ipucu, süpürme), `SkillBar`
(iksir yuvası çizimi), `Animator` / `CharacterAssets` (iki yeni klip), `AssetCache` (`requestNpc`), `CombatView`
(`FloatKind::Mana`), `LootView` (`EscapeAction::CloseShop`, tüketilirlerin ipucu), `HudView` ("Pazar"
grubu), `run-ui-flows.ps1` (`EnterVitals`, üç akış), `gen_terrain.py` (dükkân meta dosyası), `gen_zones.py`
(tezgâhlar). Eski test değişikliği: `EnvironmentTests.cpp` `knownMeshes()` dört tezgâhı da biliyor (yeni
yerleştirildikleri için; başka kontrol değişmedi). Eski akışlara dokunulmadı.

**Bilinen eksikler / zayıf noktalar**:
- Satıcılar tezgâhın arkasında değil yanında duruyor (sunucu kuralı; yukarıda). Tanyeri iksir tezgâhının
  tentesi arkadan bakınca görünmüyor (tek yüzlü; sanat kaynağı).
- Karşılama sözü italik değil (yazı tipinde italik yüz yok); tırnak ve sıcak soluk renkle ayrılıyor.
- İçme hareketi yok (sanatta klip yok); yalnız sayı.
- Pencere dükkânla birlikte açılan envanter, konuşulan satıcıyı ekranın ortasında kapatabiliyor (iki pencere
  1280'de neredeyse bütün genişliği alıyor).
- Satıcı başka bir oyuncunun dükkânı açıkken `idle_talking` oynamıyor (yalnız bizimkiyle açıkken).
- Künye / Yetenekler dükkân açıkken yeniden açılırsa dükkânın altında kalır.
- Akış dışı: dükkân penceresinde 24 kalemlik stok (en çok 8 gönderiliyor) hiç denenmedi; ekran yüksekliğinde
  kaydırma yok.
**Son tam koşu** (Debug, bu dalın son kodu): `build.ps1 -Test` 640/640 ctest (önceki 629 + 11 yeni `ShopViewTests`
durumu; test veritabanı ortam değişkeni olmadan 13 `integration.db.*` atlanıyor, önceki bölümlerdeki gibi);
`run-ui-flows.ps1` 92/92 koşu geçti (eski 84 + yeni 8: `shop` 2, `shop_homes` 4, `two_clients_shop` 2; veritabanlı
akışlar `vedatonline_test` şemalarıyla koştu). Bu koşudan sonra kodda değişiklik yok (yalnız belgeler ve ekran
görüntülerinin kopyalanması). Geliştirmede düşmeler: `--fixed-dt` ile istemci saati sunucunun iksir beklemesinden
hızlı aktı (akışlar gerçek zamana alındı); alışveriş sırasında can / mana kendiliğinden dolduğu için ikinci iksir
"zaten dolu" oldu (akış yeniden girişle %10'a dönüyor); bir satıcıdan ötekine giderken eski dükkân açık kalıyordu
(artık kapanıyor).

**Doğrulanmadı**: Release derlemesi; 24 kalemlik stok; başka bir oyuncunun dükkânı açıkken satıcının görünüşü;
gerçek bir ServerError / PurseFull yanıtı (metinleri yalnız birim testinde).
## 32. Temizlik turu "fix1" — istemci arayüzü (2026-11-18)

Bilinen istemci arayüzü kusurları. Ayrıntılar `docs/CLIENT_RENDERING.md` §17'de, akışlar `docs/BUILDING.md`'de.
Protokole, sunucuya, `shared/` oyun koduna, `gamedata/`'ya ve göçlere dokunulmadı.

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Pencereler üst üste | Tek yerleşim yöneticisi (`HudView.h` `WindowArranger`, `scenes/WorldWindows.cpp`): dükkân, envanter, Künye, Yetenekler, Tuşlar. Tercih edilen taraf (dükkân / Künye / Yetenekler sol, envanter sağ, Tuşlar orta); açık pencere yerinde kalır; yeni açılan önce kendi yerine, olmazsa ilk boş yere (açık bir pencerenin yanı, orta, öbür taraf); hiçbiri çubuğu örtmez. Yer yoksa yoldaki en uzun süredir kullanılmayan pencere kapanır (açmak, içine tıklamak, sürüklemek "kullanmak" sayılır). Başlık çubuğundan sürüklenebilir; sürüklenen pencere bir çakışmaya kadar orada kalır, sonra yeniden yerleşir. Tuşlar bir sol pencerenin yanına sığmak için %85'e kadar küçülür, yüksekliği çubuğun üstündeki bantla sınırlı | `HudViewTests` (4 yeni durum), `fix1_windows`, güncellenen `hud`, `stats`, `skills_warrior`, `shop` |
| Dükkânla açılan envanter satıcıyı örtüyor | Dükkân solda, envanter sağda; aralarında 160 mantıksal piksellik boşluk yoksa (960x600) ikisi çubuğun hemen üstüne iner. İzdüşüme yatay / dikey bir mercek kaydırması (`cameraProjection`; çizim, etiketler ve seçim aynı matrisle) satıcının göğsünü boşluğun ortasına, dar ekranda ayaklarını pencerelerin hemen üstüne getirir; dükkân kapanınca yumuşakça sıfırlanır | `fix1_shop` (`merchantvisible`: göğüs ve baş ekranda, hiçbir pencerenin içinde değil) 1280x720, 1920x1080, 1024x640 @150 % |
| Çubuk 1920x1080'de küçük | Dünya sahnesinin arayüz ölçeği = max(oyuncu ölçeği, yükseklik / 720'nin en yakın çeyrek adımı) (`hudResolutionScale`): 1080 → 1,5, 1440 → 2, pencereli 1061 → 1,5, 1421 → 2; ardından `fitUiScale`. Yazı tipleri ölçekli boyutta pişirildiği için keskin | `HudViewTests`, `fix1_windows` (`expectuiscale` 2 / 1,5 / 1 / 1,067), ekran görüntüleri |
| Oturanın ad etiketi | Oturan karakterin (kendisi ve başkaları) etiketi baş kemiğinin dinlenme yüksekliğinden ne kadar aşağıda olduğu kadar iner (`seatedHeadDrop`): ayakta 1,82 m, otururken 1,00 m | `fix1_windows` (`expectlabellift`), `two_clients_shop` (`expectremotelabellift`) |
| Stokta kaydırma yok | Dükkân penceresinin en büyük yüksekliği oyuncu çerçevesinin altındaki bant; uzun stok tam satırlarla kayar (en az 3 satır), tekerlek (çentik başına bir satır) ve kaydırma çubuğu (başparmak sürüklenir, iz tıklanınca sayfa). Test kancası `scene shopfakestock <n>` (yalnız istemci; satın alınırsa sunucu reddeder) | `ShopViewTests` (düzen hesabı), `fix1_shop`; `shop` akışı 1024x640 @150 %'te demircinin 8 kalemi artık kayıyor |
| Karşılama italik değil | `FontWeight::Italic` (Segoe UI Italic, Arial Italic); yüz yoksa glif dörtgenleri 0,2 eğilir (`TextStyle::slant`); `Label::setItalic` | `fix1_shop` (`greetingitalic`), ekran görüntüleri |
| Alma satırı biçimi | Sözleşmenin (§8.5) biçimi: "+2 Kızılca Şerbet", "+1 Kor Çekirdeği", "+10 Akçe"; ganimet, satın alma ve satışta aynı (satın alma / satış artık sonuç satırının yanında bu satırı da ekliyor). Belgeler (PROTOCOL_SHOP §8.5, PROTOCOL_LOOT, CLIENT_RENDERING) uyumlu | `LootViewTests`, `fix1_shop` (`expectpickup`) |
| "Karakterler yükleniyor…" kalıyor | `setPending(None, "")` durumu silmiyordu; liste gelince yükleme satırı temizleniyor | `fix1_windows` (`nolistloading`, ilk girişte ve dünyadan dönüşte) |
| İçme hareketi | `drink` klibi (`CharacterClip::Drink`, `characters/<ırk>_<n>/drink.vanim`) `ITEM_USED`'de içende bir kez oynar (kendisi ya da başkası); yalnız dururken ve başka eylemde değilken; hareket edince hemen yürüyüşe döner; klip yoksa sessizce yok | `ShopViewTests` (Animator), `shop` / `two_clients_shop` (ITEM_USED yolu; sanat klibi henüz yok: `drinknogesture`) |
| Bölgeye özgü yükleme ekranı | `ui/loading/zone<N>` (.png / .jpg, assets/ui ya da assets_src/ui, yoksa derlenmiş .vtex) varsa girilen / geçitteki bölgeninki, yoksa ortak arka plan; durum `loadingart<N>` | elle: geçici bir `assets/ui/loading/zone1.png` ile yükleme ekranı görüldü ve `loadingart1` doğrulandı (dosya silindi; sanat henüz main'de değil) |

**Değişen eski akışlar**: `hud` (1920'de çubuk 1,5 ölçekte: deneyim çubuğu noktası; üç pencere yerine envanter → Yetenekler → Tuşlar, en eski kapanır), `stats` ve `skills_warrior_view` (960x600'de pencereler birlikte sığmıyor), `shop` (Tuşlar ile Esc sırası boyuttan bağımsız; 1024x640 @150 %'te kılıç satırına kaydırma), `two_shop_a/b` (oturanın etiketi).
Ekran görüntüleri: `docs/screenshots/fix1/ui_*.png` (hepsine bakıldı).

**Bilinen eksikler / zayıf noktalar**:
- Ganimet penceresi yöneticiye dahil değil (eskisi gibi solda, Künye'nin üstüne gelebilir).
- 960x600'de dükkân çifti alçakta dururken satıcının başı / işareti ekranın üst kenarına yakın; uzun stokla pencere uzayınca satıcı daha da yukarı çıkar (o durumda `merchantvisible` beklenmiyor).
- Tuşlar küçültülünce (k < 1) yazı boyları yuvarlandığından bir satır sütun aralığına biraz taşabiliyor (1024x640 @150 %).
- Ölçek yalnız dünya sahnesinde; giriş / karakter ekranları oyuncunun ölçeğinde.
- İçme klibi ve bölge yükleme resimleri sanat ajanından gelecek; gelmeden yalnız geri dönüş yolu denendi.

**Son tam koşu** (Debug, bu dalın son kodu): `build.ps1 -Test` 648/648 ctest geçti (13 `integration.db.*` test veritabanı ortam değişkeni olmadan atlanıyor, önceki bölümlerdeki gibi); `run-ui-flows.ps1` 96/96 koşu geçti (eski 92 + `fix1_windows` 1 + `fix1_shop` 3). Geliştirmede düşmeler: pencereli 2560x1440'ın istemci alanı 1421 (aşağı yuvarlanan adım 1,75 verdi: en yakın adıma geçildi), Tuşlar 960x600'de çubuğu örtüyordu (yüksekliği banda sınırlandı) ve 1280'de Künye'nin yanına sığmıyordu (%85'e kadar küçülür), tekerlek çentik sayısını yok sayıyordu, `shop` akışı 1024x640 @150 %'te kaydırılan kılıç satırını bulamadı.

**Takip turu (fix1 devamı, 2026-11-18)** — koordinatörün gördükleri:

| Konu | Ne yapıldı | Nasıl doğrulandı |
|---|---|---|
| Yetenek kartında ad iki satıra taşıyordu ("Meydan Okuma" 1,5 ölçekte "Öğrenilmedi"nin üstüne biniyordu) | Ad tek satır; sığmazsa yazı Small boyundan 0,5'lik adımlarla 10'a kadar küçülür (`SkillCellWidget::skillNameFontSize`); kartta ad + seviye satırı yüksekliği ve "+" düğmesinin solunda seviye satırı denetleniyor | `scene expectskillnamesfit` (dört sınıfın bütün yetenekleri, aynı piksel hesabıyla) `fix1_windows`'ta 2560x1440, 1920x1080, 1280x720, 1024x640 @150 %; ekran görüntüsü ui_06 |
| Ganimet penceresi yöneticinin dışında | `HudWindow::Loot` (sol taraf); eski konumlama kaldırıldı | `HudViewTests` (yeni durum), `hud` akışı (ganimet penceresi açıkken Künye: yan yana, `expectnooverlap`, ui_40) |
| İpucu metni 1920x1080'de sağ kenara değiyordu | Kaydırılmayan satır kutuyu ekran genişliğine kadar genişletir (önceden 270 mantıksal piksele kırpılıp taşıyordu); taşan satır sayacı `tipoverflows<N>` | `fix1_windows` dört boyutta `tipoverflows0`, ui_05 |
| Küçülen Tuşlar'da satır sütun aralığına taşıyordu | Sütun genişlikleri çizimde kullanılan (yuvarlanmış boylu) yazı tipleriyle ölçülüyor; pencere genişliği ve küçültme buna göre | `scene expectcontrolsfit` üç yerde, ui_16 |
| 960x600'de uzun stokla satıcı | Dar ekranda dükkân envanterden en çok 12 mantıksal piksel uzun (uzun stok daha erken kayar); çerçeveleme önce işaret ve başı gösterir (gerekirse dizlerden aşağısı pencerelerin arkasında); `merchantvisible` işaretin üstünü de denetler | `fix1_shop` uzun stokta ve kaydırınca `merchantvisible` (üç boyut), ui_22/23 |
| Oturanın etiketi | Etiket baş kemiğini üç boyutta izler (öne eğilme dahil), ayaktaki aralıkla | `fix1_windows` / `two_clients_shop` `expectlabellift`, ui_12, ui_30 |

Takip turunun son tam koşusu: `build.ps1 -Test` 649/649 ctest (13 `integration.db.*` atlanıyor), `run-ui-flows.ps1` 96/96 koşu; ekran görüntüleri yeniden alındı (`docs/screenshots/fix1`, ui_40 yeni).

## 33. Temizlik turu "fix1" — doğrulama boşlukları ve sunucu tarafı (2026-11-18)

Bu tur içerik eklemedi; sunucu (iki test kancası), `tools/botclient/`, testler ve belgeler değişti; `shared/` değişmedi. İstemci, sanat,
`assets_src/`, `gamedata/` ve göçlere dokunulmadı. Yalnız `vedatonline_test` kullanıldı (worktree'nin
git'e girmeyen `config/server.local.toml`'unda yalnız test bağlantısı; `vedatonline`'a bağlanılmadı).

### 32.1 Release derlemesi

| Adım | Sonuç |
|---|---|
| `build.ps1 -Config Release` (ilk yapılandırma dahil, vcpkg önbellekten) | uyarısız, hatasız; 112 s (değişikliklerden sonra artımlı 40 s) |
| `ctest --test-dir build\x64-release -j 4` (worktree'de test veritabanıyla, `integration.db.*` dahil) | **640/640 geçti**, 588 s; atlanan yok |
| `run-ui-flows.ps1 -Config Release -Only happy,world_tour,combat,zone_gate,shop,hud,two_clients_shop` | **11/11 koşu geçti**, 299 s, 9 sunucu, sızan süreç yok |

Yalnız iyileştiriciye özgü bir hata ya da uyarı çıkmadı; `-Config` anahtarı akış aracında zaten vardı.
En uzun Release testi `integration.db.zones` (330 s, zaman aşımı 520 s).

### 32.2 200 botluk yük testi

Release, bu makine, `vedatonline_test`'te kendi şeması (`vo_it_load_*`, sonra silindi), `world.start_zone_override = 1`,
`botclient --bots 200 --duration 600 --threads 4 --ramp-ms 20 --zones`: 40 savaşçı, 40 gidip gelen, 40 yurduna
gidip kalan, 40 **alışverişçi** (yeni: her 6–15 s'de kasabasının şerbetçisine yürür, açar, Akçe'si yetmezse yedek
asayı satar, Kızılca Şerbet alır, içer, kalırsa geri satar, kapatır; kötü yanıt koşuyu düşürür), 40 köyde gezen
(bunların bir kısmı kuşan / çıkar yapıyor). Aynı anda çalışanlar: sanat ajanının Blender'ı (ilk ~3,5 dakika 1–3
süreç), başka bir worktree'nin bir botclient'ı, kullanıcının boştaki vo_server'ı; derleme yoktu.

| Ölçü | Değer |
|---|---|
| Giriş | 200/200 dünyada 4,2 s'de; pencere 600 s, sonda 200 dünyada |
| Dünya tiki (10 s'lik 60 pencere) | ortalama 1,07–2,36 ms (ilk dakika, Blender çalışırken 1,6–2,4), son 5 dakika ortalama 1,15 ms, p99 ≤ 1,8 ms, en çok 2,06 ms; bütün koşuda p99 en çok 6,25 ms, en çok 8,1 ms (ilk pencere); **0 yavaş, 0 atlanan tik** |
| Bellek (vo_server, 10 s'de bir) | özel bellek 20. saniyede 30,1 MB, 600. saniyede 30,3 MB; çalışma kümesi 41–42 MB; tutamaç 447 sabit: **büyüme yok** |
| Ağ | istemci başına 31 KB/s (en az 9, ortanca 39, en çok 46 KB/s), 696 paket/s (681 ENTITY_MOVE/s); toplam 6,25 MB/s; PING p50 0,58 ms, p90 3,2, p99 4,0, en çok 8,3 ms |
| Hatalar | 0 düzeltme, 0 kopma, sunucu günlüğünde 0 WARN / ERROR; 0 kötü eşya / ganimet / nitelik / yetenek / bölge / dükkân yanıtı |
| Bölgeler | 2633 değişim (~4,4/s, her biri bir veritabanı yazımı), 0 hatalı, 396 ret (gidip gelenlerin yolu öteki ırkın geçidinden geçiyor); sonda bölge 1/2/3: 127/42/31 |
| Savaş | 401 öldürme, 401 ganimet alma (5158 Akçe), 50 seviye, 50 nitelik, 50 yetenek yazımı, 3 ölüm / 3 dirilme |
| Dükkân | 2271 açma, 240 alım, 280 satış; içme 0 (köydekiler hep tam canda: AlreadyFull; Akçe'si bitenler NotEnoughCoins — 2271 beklenen ret), 0 kötü |

M13 ölçümüyle (§26: 60 s, 1,14–1,74 ms, p99 ≤ 2,55) aynı düzeyde; dükkân ve 10 dakika bir şey değiştirmedi.
Gerçek bir sorun bulunmadı, kod düzeltmesi gerekmedi. **Ayar önerileri (uygulanmadı)**: (1) bant genişliğinin
çoğu köydeki ENTITY_MOVE yayını (sunucu 131 000 iletim/s): uzak AOI hücrelerine 5 Hz göndermek istemci başına
trafiği yaklaşık yarıya indirir (protokol davranışı, sözleşme değişikliği ister); (2) yük botlarında gidip
gelenlerin yolu öteki ırkın geçidinden kaçınabilir (396 ret yalnız bot yolu yüzünden; bot tarafı); (3)
Akçe'si biten alışverişçi açmayı bırakabilir (bot tarafı); (4) bölge değişimi başına bir yazım 4,4/s'de sorun
değil, ama binlerce oyuncuda varış yazımları toplu yapılabilir.

### 32.3 Aynı anda bölge değiştirenler (`integration.zonesrace`)

Yeni test kancası `world.testing_zone_arrival_delay_ms` (varsayılan 0): bir bölge değişimi, yazımı bitse de
`ZONE_CHANGE_BEGIN`'den en az bu kadar sonra tamamlanır (`World::onZoneArrived` erteler, her tik yeniden
dener). botclient `zonesrace`:

| Aşama | Ne denetleniyor | Sonuç |
|---|---|---|
| `same` (kancasız ve 1500 ms ile) | iki Aydor aynı adımda `gate_esik`'e girer (`ZONE_CHANGE_BEGIN` iki tarafta 0 ms arayla: aynı tik), ikisi de `arrive_aydor`'da, birbirini **bir kez** görür; `gate_aydor`'dan birlikte geri | geçti |
| `cross` | biri Tanyeri → Eşik, öteki Eşik → Tanyeri aynı anda; her bölgede bir gözlemci gideni kaybolurken geleni görür, ikisi birbirini bölgeler arasında görmez | geçti |
| `logout` (1500 ms) | `ZONE_CHANGE_BEGIN`'den hemen sonra LOGOUT: bağlantı ~1540 ms sonra kapanır (varış yazımını bekler); Eşik'teki gözlemci karakteri görmez; yeni girişte karakter Eşik'te varış işaretinde. Aynısı LEAVE_WORLD_REQ ile (yanıt ~1490 ms sonra), aynı bağlantıda yeniden giriş Eşik'te | geçti |
| `drop` (1500 ms) | `ZONE_CHANGE_BEGIN`'den hemen sonra bağlantı kopar; ~400 ms sonra yeni giriş (geçiş sürerken, `World::onEnter` varışı devralır: "replacing session"): Eşik'te varışta, gözlemci karakteri tam **bir** varlık olarak görür. İkinci kopma: gözlemci varışı hiç görmez; değişim bittikten sonraki giriş Eşik'te | geçti |

Sunucuda hata çıkmadı. Testte bulunan ve düzeltilen: gözlemcinin gördüklerini anlık değil sürekli saymak
gerekiyordu (senaryo hatası).

### 32.4 Dükkân hata yolları (`integration.shoperrors`, `integration.db.shoperrors`)

Yeni test kancası `world.testing_fail_first_item_write` (varsayılan 0): her oturumda o şablonun ilk alımı,
ilk satışı ve ilk kullanımı yazım başarısız olmuş gibi biter (`Session::applyShopWrite`; hiçbir şey
değişmez, başarısız kullanımın beklemesi sürer). Sonuç kodu → hangi senaryoda:

| Kod | Daha önce | Şimdi (protokol üzerinden) |
|---|---|---|
| Shop `ServerError` (alım, satış) | yalnız birim | `shoperrors` (kanca 16): çanta / bakiye değişmez, sonraki istek kaydolur |
| Use `ServerError` | yalnız birim | `shoperrors`: yeniden doğduktan sonra %50 canla; bekleme başlar, 10 s sonra içilir |
| `PurseFull` | yalnız birim | `db.shoperrors`: `vo_admin set-coins … 999999990`, 30 Akçe'lik satış reddedilir |
| `BagFull` | yalnız birim | `db.shoperrors`: 27 kılıç çantayı doldurur, 28. reddedilir, satış sonra sığar |
| `AlreadyFull` | yalnız birim | `shoperrors`: tam canda Kızılca Şerbet (bekleme başlamaz) |
| Alım / satışta `OutOfRange` | yalnız birim (açmada `shop`'ta vardı) | `shoperrors`: satıcıdan 6–7 m'de, dükkân açıkken |
| Use `YouAreDead` | yalnız birim | `shoperrors`: Kor Bekçisi'ne ölünce |
| Açmada `YouAreDead` | yalnız birim | **ulaşılamıyor**: kasabalarda öldüren canavar yok, ölünce satıcı görüşte kalmıyor (senaryo `NotFound`'u bekliyor ve söylüyor) |
| Alım / satışta `YouAreDead` | yalnız birim | **pratikte ulaşılamıyor**: ölüm dükkânı aynı tikte kapatır, `NotOpen` önce gelir (tek tiklik yarış) |
| Use `Stunned` | yalnız birim | **ulaşılamıyor**: v8'de oyuncuyu sersemleten kaynak yok |
| 16 bekleyen istek → `ServerError` | yalnız birim | protokolden deterministik değil (sel sınırı 10/s); birimde kalıyor |
| `NotFound`, `NotOpen`, `NotInStock`, `BadQuantity`, `NotEnoughCoins`, `NoItem`, `NotUsable`, `OnCooldown`, açmada `OutOfRange` | `shop` | değişmedi |
| `CannotSell` | yalnız birim | gönderilen veride değeri 0 olan eşya yok (veriye dokunulmadı) |

### 32.5 Avcı botlar ve geçitler

c7c57a6'nın düz çizgi denetimi yalnız hedef seçiminde işliyordu; kovalarken ya da ızgara yolunda bot yine
geçide girebiliyordu. Şimdi `approach()` (bütün av senaryoları onu kullanıyor) her `gate_*` işaretinin
tetik yarıçapı + 3 m'sinin içine adım atmıyor (çağıran `allowGate` istemedikçe): düz yol böyle bir alandan
geçiyorsa ızgara yolu (`Nav::bfs`, alanlar kapalı) çevresinden dolaşır, hedef alanın içindeyse bırakılır,
son savunma olarak alana doğru her adım reddedilir (alanın içinde duran bot dışarı yürüyebilir).
`fightThrall` alanın içindeki hedefleri de seçmiyor. `combat`, `loot`, `stats`, `skills`, `shoperrors`
her adımda ve sonda `ZONE_CHANGE_BEGIN` görürse "the bot changed zone unexpectedly … near (x, z)"
iletisiyle düşer.

### 32.6 Ekonomi: seviyeye göre Akçe geliri ve harcanabilecekler

`vo_tests "[economy]"` (`tests/game/EconomyTests.cpp`, `VO_ECONOMY_TABLE=1` tabloyu yazar) gönderilen
`gamedata` ve paylaşılan formüllerle (deriveStats, computeDamage, yenilenme, xpToNext) hesaplar. Model:
yalnız temel saldırı (yetenekler hasarı ~%25 artırır), puanların hepsi sınıfın saldırı niteliğine, donanım
başlangıç takımı + seviyesinde satılan en iyisi (açıldığı seviyede alınır), av = bir seviye üstüne kadar
saat başı en çok xp veren seçkin olmayan canavar (5–10. seviye 4. seviye canavarları avlar: daha üstü yok),
öldürme başına 8 s yürüme / ganimet, sonra kaybedilen can oturarak yenilenir ("dinlenme"); ya da oturmak
yerine en ucuz can iksiri içilir ("iksir", 10 s bekleme içinde). Gelir = ortalama para + her düşüş satırı ×
şans × ortalama adet × satış fiyatı.

Savaşçı (öteki sınıflar ±%10, 7–9. seviyede aynı tablo):

| Sv. | Av | Akçe / öldürme | Öldürme / saat | Akçe / saat (dinlenme) | Seviyede saat | Seviyede Akçe | Açılan donanım | İksir modu: kazanılan / iksire / pay |
|---|---|---|---|---|---|---|---|---|
| 1 | Kırağı Kayası (1) | 7,4 | 113 | 836 | 0,03 | 24 | – | 1168 / 652 / %56 |
| 2 | Kül Çömezi (2) | 7,6 | 108 | 822 | 0,07 | 57 | – | 1237 / 1015 / %82 |
| 3 | Cüruf Yürüyen (4) | 21,9 | 71 | 1564 | 0,09 | 135 | Burç Kılıcı 60 | 3345 / 2100 / %63 |
| 4 | Cüruf Yürüyen (4) | 21,9 | 85 | 1867 | 0,12 | 216 | – | 3669 / 1881 / %51 |
| 5 | Cüruf Yürüyen (4) | 21,9 | 98 | 2151 | 0,14 | 307 | – | 4032 / 1765 / %44 |
| 6 | Cüruf Yürüyen (4) | 21,9 | 110 | 2406 | 0,17 | 412 | – | 4293 / 1589 / %37 |
| 7 | Cüruf Yürüyen (4) | 21,9 | 120 | 2636 | 0,20 | 527 | – | 4586 / 1500 / %33 |
| 8 | Cüruf Yürüyen (4) | 21,9 | 130 | 2849 | 0,23 | 652 | – | 3517 / 986 / %28 |
| 9 | Cüruf Yürüyen (4) | 21,9 | 137 | 3007 | 0,26 | 787 | – | 4997 / 1289 / %26 |
| 10 | Cüruf Yürüyen (4) | 21,9 | 146 | 3206 | – | – | – | 5225 / 1147 / %22 |

1–10 toplamı: savaşçı 3117 Akçe (1,3 saat saf av), haydut 3168, büyücü 3165, rahip 3106. Bütün oyun boyunca
satın alınabilecek donanım: savaşçı 60, haydut 15, büyücü 60, rahip 45 Akçe (satıcılarda en pahalı eşya 60
Akçe; Bekçi Kalkanı 70 yalnız ganimette). Tek düzenli harcama iksir: dinlenen oyuncu hiç harcamaz, oturmak
yerine içen 7–9. seviyede gelirin %26–41'ini iksire verir, yani saatte ~1000–1800 Akçe biriktirir.
**Sonuç**: 7–9. seviyede saatte 2250–3090 Akçe (dinlenme) kazanılıyor, o seviyelerde satın alınacak yeni bir
şey yok; birikim seviye 10'da da sürüyor (seviye tavanında xp gitmiyor, gelir 2800–3250/saat). Model
iyimser değil: yetenekler ve oturmadan iksir içmek geliri artırır. (§30'daki "7–9 → 600–1800 Akçe seviye
başına" tahmini bu hesapla tutarlı: 527–787 dinlenerek, iksirle daha hızlı.)

**Akçe çıkışı seçenekleri** (karar müdürün; içerik eklenmedi):

1. **Yıpranma ve onarım**: silah / kalkan her öldürmede (ya da her ölümde) dayanıklılık kaybeder,
   demircide `değer × kayıp oranı × seviye çarpanı` ile onarılır. Seviyeyle büyüyen, kaçınılamayan ama
   küçük bir çıkış; mekanik KO benzeri, sayılar özgün. Sunucuda bir sütun + bir göç, istemcide bir çubuk.
   Hedef: 7–9. seviyede gelirin ~%20–30'u.
2. **Güçlendirme (artı seviyeleri)**: donanımı Akçe + malzemeyle (Kor Çekirdeği, Cüruf Parçası) +1…+5'e
   çıkarmak, yüksek basamaklarda başarısızlık şansı. Malzemelere de anlam verir, birikimi sınırsız
   emebilir; dengesi en zor seçenek.
3. **Hizmet ücretleri**: geçit / yurtlar arası yolculuk bedeli, yerinde dirilme bedeli, yetenek / nitelik
   sıfırlama bedeli (şimdi yalnız `vo_admin`). Uygulaması en kolay; tek başına yeterli değil, 1. ya da 2.
   ile birlikte düşünülmeli.

### 32.7 Testler

Yeni: `integration.zonesrace`, `integration.shoperrors`, `integration.db.shoperrors`, `Economy: Akce income per
hour by level …` (`[economy]`). Hepsi `--repeat until-fail:3` ile üç kez üst üste geçti (zonesrace tek başına
440–446 s; shoperrors ~60 s, db.shoperrors ~76 s); değişen avcı senaryoları `integration.combat`, `loot`,
`stats`, `skills`, `db.stats`, `db.skills` da üçer kez geçti.

**Son tam koşu** (Debug, `build.ps1 -Test`, worktree'de test veritabanıyla, seri, 2526 s): **643/644**; düşen tek
test ekonomi testiydi — adı "Akçe" içeriyordu, ctest ASCII olmayan adla süzgeci kuramadı (test kendisi geçiyordu).
Ad ASCII yapıldı, yeniden derlenip tek başına koşuldu: geçti. Atlanan yok (13 + 1 `integration.db.*`
`vedatonline_test`'e karşı koştu). İlk Release koşusu (değişikliklerden önce) 640/640; değişikliklerden sonraki
Release: **644/644** (`ctest -j 4`, 896 s, `integration.zonesrace` ve `db.shoperrors` dahil).

Geliştirmede düşen ve düzeltilen: zonesrace'te ikinci yaklaşım noktası seçimi (geçidin önünde 1,5–3,5 m arayla
nokta yoktu: oyuncular çarpışmadığı için aynı çizgiye izin verildi), ızgarada 0,4 m'lik hedef (hücre merkezine
ulaşılamıyordu), `cross`'ta gözlemcilerin yeri (geçitten uzaktaydılar), kopma aşamasında gözlemcinin sayımı,
zonesrace'in 420 s zaman aşımı (paralel koşuda; 900 s yapıldı, ikinci `same` aşaması çıkarıldı).

### 32.8 Kalanlar

- Ulaşılamayan dükkân kodları (yukarıdaki tablo): açmada / alım-satımda `YouAreDead`, `Stunned`, `CannotSell`,
  16 bekleyen istekte `ServerError` yalnız birim testlerinde.
- Shop `ServerError`'ın gerçek veritabanı arızası protokol üzerinden denenmedi (kanca oturumda, yazımı hiç
  göndermiyor); veritabanı arızası `WorldDbOutageTests` / `ItemServerTests` birim düzeyinde.
- `integration.zonesrace` uzun (~7,5 dakika tek başına; zaman aşımı 900 s): çoğu süre geçitlere yürümek ve
  2 s kurulma beklemeleri.
- Yük testindeki alışverişçiler hiç iksir içemiyor (köyde can hep dolu); Akçe'si bitince boşuna açıyorlar.
- İlk denemede `vo_load_1794988140` adlı bir şema (40 bot hesabıyla) `vedatonline_test`'te kaldı: `vo_admin testdb
  drop-schema` yalnız `vo_test_*` / `vo_it_*` siliyor (sonraki koşu `vo_it_load_*` adıyla, silindi). Elle
  silinmeli (`DROP SCHEMA vo_load_1794988140 CASCADE` — yalnız `vedatonline_test`'te).
- Ekonomi: Akçe çıkışı seçimi müdürde (§33.6).

## 34. Temizlik turu "fix1" — dünya ve sanat (2026-11-18)

Hepsi Blender'da başsız, depodaki betiklerle; indirme, dış üretici, başka oyundan alıntı yok. Ekran görüntüleri:
`docs/screenshots/fix1/world_*.png` (`fix1_world` akışı ve `two_clients_shop`, `skills_mage`).

| # | Kusur | Ne yapıldı (dosyalar) | Nasıl doğrulandı |
|---|---|---|---|
| 1 | Tüccarlar tezgâhın ~4 m yanında | Altı tüccar tezgâhının `stand` noktasında, tezgâhın ardında (`gamedata/npcs.toml`; Ortaocak `market_stall` için (0, −0,3)). Sunucu kuralı değişti (en küçük gerekçeli değişiklik): "kendisi ve 1 m çevresi yürünebilir" yerine "müşteriye yer var": çevresindeki 16 noktadan biri 2,5 m'de, 0,5 m çevresiyle yürünebilir (`shared/game/Npcs.h/.cpp` `kNpcCustomerRing`, `kNpcCustomerClearance`); tüccarın kendisi tezgâhın kapalı izinde durabilir. `gen_zones.py` `check_merchants` aynı kuralı denetler ve noktaları yazar. Yük testi botları müşteri noktasına yürür (`tools/botclient/LoadTest.cpp`). Belgeler: `PROTOCOL_SHOP.md` §1.2, `SERVER_WORLD.md`, `SHOP_ART.md` §2 | `ShopTests`, `ShopZoneTests` (yeni durumlar), `integration.shop`, `integration.load`, akışlar `fix1_world` (altı tüccarla dükkân açılıyor), `shop`, `shop_homes`, `fix1_shop` |
| 2 | Demirağız yolunda mangala takılma | Yol kenarı mangalları yol ekseninden ayak izi ≥ 5,5 m (fener direkleri ≥ 3,5 m) uzağa itiliyor, yer yoksa kaldırılıyor (`gen_terrain.clear_roadsides`); Demirağız'da 7 mangal 1,25 m dışarı. Üç bölgede yol ekseni ±1,5 m yürünebilir denetimi (`gen_zones.check_road_paths`); bölge 1 ve 2'de değişiklik gerekmedi | `fix1_world` 2: kasabadan geçide tek yol bulmalı yürüyüş (eski akış iki parçaydı) |
| 3 | Kor Bekçisi `arrive_kargan`'a yakın | Kor Bekçisi (223,9, 324,9) → (210, 338): `arrive_kargan`'dan 57 m; güneydoğu sürüsünden bir Kül Çömezi `arrive_aydor`'dan 15,1 m yerine 17,9 m. Test: her canavar aggro + 8 m, elitler aggro + tasma + 10 m her varış / ırk doğuşundan uzak (`MonsterSpawnTests`); `death` akışı yeni yere yürüyor | `content` testleri, `death`, `skills_*` akışları, `fix1_world` 2 (`arrive_kargan`'da 2 s, can dolu) |
| 4 | Kapı perdesi neredeyse opak | Perde dokusunda alfa (ortası ~%16, kenar / kemer daha yoğun, ince akıntılar); `Mesh.hlsl` yarı saydam efektlerde opaklığı doku alfasıyla çarpıyor (alfası olmayan efektler aynı) | `fix1_world` 05, 17 |
| 5 | Demirağız kayalığında dikiş | Kasabanın kuzeyindeki duvar 6 m'de ~95 m yükseliyordu (metrede 20 m) ve üstü düz bir rafla bitiyordu; 2,6 m/m'den dik basamaklar komşularına doğru gevşetiliyor (`gen_zones.relax_steps`); kapı oyukları duvarın yeni dibinde | yükseklik profili, `fix1_world` 14, `EnvironmentTests` |
| 6 | Tanyeri iksir tezgâhı tentesi arkadan görünmüyor | Tezgâh tenteleri (çizgili bez, deri) pişirmeden sonra ters yüzlü kopyalarla çift yüzlü (`gen_zone_props.make_two_sided`, `gen_shop_props.TWO_SIDED`) | `fix1_world` 02, 04, 11 |
| 7 | Kargan iksircisinin pelerini kutu gibi | Dökümlü kısa pelerin (`gen_npcs.drape_cowl`) | `fix1_world` 12, Blender önce / sonra |
| 8 | Buz Kabuğu tel kafes | 320 plakalı yarı saydam buz kabuğu, çift yüzlü, RGBA atlas (çatlak / kırağı) (`gen_skill_effects.py`) | `skills_mage` (02_frost_shield), Blender önce / sonra |
| 9 | Yükleme ekranı her bölgede aynı | `gen_loading_art.py`: üç bölgenin kendi arazisinden Blender çizimi (Ortaocak öğleden sonra, Tanyeri şafak, Demirağız mavi saat), `ui/loading/zone1..3.png`. İstemci seçimi main'deki `SceneResources::zoneLoadingImage` (bu daldaki eşdeğer kanca birleştirmede bırakıldı) | `fix1_world` 06, 07, 10, 19 |
| 10 | İçme hareketi | Altı oyuncu görünümüne `drink` klibi (30 kare, sağ el dudağa, baş geriye; `vo_ual.make_drink`); tüccar ve canavarlarda yok | `ContentTests` (30 klip), `two_clients_shop` (`drinks1` kendi ve uzak; içen artık ayağa kalkıp içiyor) |
| 11 | Yurt canavarları renk değişimi, tüccarlar oyuncu yüzü | Dört yurt canavarına kendi silueti (`monster_shapes.py`: saman korkuluk, sancaklı nöbetçi, buz dikenli kaya, boynuzlu buz muhafızı); altı tüccara birer özellik (ot dalı, keçe börk, pipo + toka, bandana + bıyık, alın halkası, sakal halkaları) | `MonsterTests`, `ShopTests`, Blender önce / sonra; oyunda yalnız tüccarlar görüldü |

**Değiştirilen başka ajan alanları (asgari)**: `shared/game/Npcs.*` (kural), `tests/game/ShopTests.cpp`,
`tests/server/world/ShopZoneTests.cpp` (yeni kurala göre), `tests/assets/content/MonsterSpawnTests.cpp`, `ContentTests.cpp`
(`drink`), `tools/botclient/LoadTest.cpp` (müşteri noktası), `client/shaders/Mesh.hlsl` (doku alfası),
`client/automation/flows/death.voscript`, `two_shop_a/b.voscript`, `tools/dev/run-ui-flows.ps1` (`fix1_world`).

**Son tam koşu** (birleştirilmiş ağaç): `build.ps1 -Test` 653/653 (14 `integration.db.*` test veritabanı ortam
değişkeni olmadan atlanıyor); `run-ui-flows.ps1` 98/98 koşu geçti (yeni `fix1_world` 2 koşu; `two_clients_shop` içme hareketini denetliyor; veritabanlı akışlar `vedatonline_test` şemalarıyla). Birleştirmeden sonra ilk koşuda `integration.load` düştü
(tüccara yürüyen botlar tezgâhın içine yol bulamıyordu; düzeltildi).

**Bilinen eksikler**: içme hareketinde el boş (şişe yok; parmaklar ele bağlı) ve iniş anında avuç bir an öne bakıyor;
Kargan demircisinin sakal halkaları gri sakalda zor seçiliyor, Eşik iksircisinin ot dalı küçük; tüccarların yüzleri hâlâ oyuncu
görünümlerinin; yeni canavar siluetleri oyunda görülmedi (yalnız Blender ve içerik testleri); saman eteğin yürüyüşte bacaklarla
kesişmesi denetlenmedi; Demirağız yükleme çiziminde kasaba küçük; `integration.db.*` bu worktree'de koşulmadı.

### 34.1 fix1 ikinci tur (müdürün oyun içi incelemesinden sonra)

Ekran görüntüleri `docs/screenshots/fix1/world_3x_*.png`.

| Konu | Ne değişti | Doğrulama |
|---|---|---|
| Buz Kabuğu hâlâ tel kafes gibi | 80 geniş yüzlü kabuk, dolgulu buz mavisi yüzler (alfa ~0,4–0,6), çatlak ve dikişler yalnız silik ayrıntı; yarı saydam efektlere `rim` (yüzey bakıştan döndükçe daha opak ve parlak; `WorldRenderer::addEffectMesh`, `Mesh.hlsl` `g_alphaTest.w`), Buz Kabuğu opaklığı 0,4 → 0,8 (`SkillsView.cpp`) | `skills_mage` 02 / 02b (normal uzaklık ve yakın) |
| İçerken kılıç ağza | `drink` oynarken ana el silahı çizilmiyor, yerine küçük kırmızı şişe (`props/drink_flask`, `gen_drink_flask.py`) bakışın hançer tutuşunda; klip inerken kalkışın tersini izliyor (avuç öne dönmüyor) | `two_clients_shop` (kendi `drinks1`, uzak `drinks1`), `shop_homes` 3 (Kargan büyücü, asa) |
| Kapı perdeleri | Perdenin ortası neredeyse saydam (alfa ~0,05) ve ışımasız; ışıma kenar / kemerde, alfayla birlikte | `fix1_world` 05, 08a, 17, 18a (dört kapı, gündüz) |
| Ortaocak iksircisi mallarla gizli | Tezgâhın fener ucunda, tezgâhın hemen dışında, bütün boyuyla görünüyor; M14 sanat çizimleri yeniden (`gen_shop_shots.py`) | `fix1_world` 09, `docs/screenshots/m14/art_*` |
| Yurt canavarları oyunda | Dördü oyunda: yaklaşma, saldırı, ölüm (`fix1_monsters` akışı, test veritabanı, 10. seviye savaşçılar). Anız Yanığı'nın saman eteği artık uyluklara bağlı (yürürken bacak içinden geçmiyor) | `fix1_monsters`, Blender yakın çekim |
| Akış dayanıklılığı | `skills_mage` / `skills_rogue` Kor Bekçisi'ne kuzeyden (240 / 315) yürüyor (yeni yerinde yol kuzeybatı sürüsünün içinden geçiyordu); büyücü emilen ilk vuruşu bekliyor ve büyüden önce duruyor (hareket büyüyü kesiyor) | akışlar |

**İkinci tur koşuları** (birleştirilmiş ağaç, 97ff508 üstünde): `build.ps1 -Test` 653/653 (14 `integration.db.*` test
veritabanı ortam değişkeni olmadan atlanıyor); `run-ui-flows.ps1` 101/101 (yeni `fix1_monsters` 3 koşu). Geliştirmede
düşmeler: şişenin uç halkalarında dejenere üçgenler (içerik testi; düzeltildi), `skills_mage` yarışları (yukarıda).

**Hâlâ zayıf**: içerken el açık (parmaklar ele bağlı, şişe avuçta duruyor gibi); uzaktan oyuncunun içişi arkadan
görünüyor (akış ekranı arkadan); buz kabuğu yakında biraz bulanık-opak (karakter seçiliyor ama soluk); Tanyeri'nin kapı
perdesinin ardı tebeşir kaya olduğundan saydamlık orada az belli; Yanık Nöbetçi'nin sancağıyla kolu arasında kesişme
Blender'da görülmedi ama oyunda yakından denetlenmedi; Kargan demircisinin sakal halkaları zor seçiliyor.

## 35. M15 "Avlaklar" — seviye 20, av yerleri (slot), bosslar, Eşsiz eşyalar, zırh (2026-11-18)

Sözleşme: `docs/PROTOCOL_LEVELS.md` (protokol **v9**). Mekanik KO benzeri (avlaklar, boss + eşsiz düşüş, kademeli
donanım); bütün adlar, sayılar, metinler ve görünüş özgün. Ekran görüntüleri: `docs/screenshots/m15/`.
Veri üretimi tek seferlik bir betikle yapıldı (kimlikler sabit; dosyalar elle düzenlenebilir), tablolar aşağıda.

### 35.1 Seviye tavanı 10 → 20

| Konu | Karar |
|---|---|
| `kLevelCap` | 20 (`shared/game/Combat.h`); `vo_admin set-level`, HUD ("en yüksek seviye"), Künye, karakter listesi ve bütün kıskaçlar bu sabiti kullanıyor |
| Deneyim eğrisi | 1–9: değişmedi (`80 · L^1.6`, 10'a yuvarlanır; saklı xp geçerli kalır). 10–19: `80 · L^1.6 · (1 + 0.6 · (L − 9))`. 10→11 5 100, 12→13 16 480, 15→16 35 130, 19→20 62 260; 1→20 toplam 295 010 (1→10: 10 710) |
| Nitelik puanı | seviye başına 3 (değişmedi; seviye 20: 57). Göç 0005'in `3 · (level − 1)` CHECK'i geçerli |
| Yetenek puanı | seviye başına 1 (değişmedi; seviye 20: 19) |
| Can / mana / saldırı / savunma | sınıf tablolarının seviye başı artışları 20'ye kadar doğrusal (değişmedi) |
| Eski karakterler | seviye 10'dakiler xp 0 ile yeniden deneyim kazanıyor (`addExperience`), başka bir şey değişmiyor |
| Yetenek kademeleri | her yetenek 5 kademe; kademe 4 seviye 11, kademe 5 seviye 16 ister (`skills.toml [rules] rank_levels = [1, 1, 1, 11, 16]`, yeni sonuç `LevelTooLow = 6`). 4–5. kademe değerleri 2→3 adımının devamı; şifalar (Soluk Güneş, Halka Şifası, Yenileyen Işık, Dirençli Soluk) yarım adım. Göç **0008_skill_ranks_5.sql** (`character_skills.rank` 1..5; uygulanmadı: müdür uygular). `set-level` düşürürken kademe seviyesi de tutmazsa yetenekler sıfırlanır (`skillRanksFitLevel`) |

### 35.2 Avlaklar (slot)

Veri `gamedata/monsters.toml` `[[slot]]` (alanlar ve kurallar `PROTOCOL_LEVELS.md` §3). Bütün dünya avlaklardan oluşuyor:
eski dağınık `[[spawn]]` girdileri avlaklara çevrildi (biçim testler için duruyor). 46 avlak: 41 sıradan (6 üye, halka
6–7 m, gezinme 1 m → alan 14–16 m), 5 boss (tek). Kurallar ve sayılar:

| Kural | Değer |
|---|---|
| Üye sayısı / alan | 1..12 (sevk edilen: 6, 7'den itibaren biri merkezde); alan = en uzak yuva + gezinme ≤ 9 m; yuvalar ≥ 2,5 m arayla |
| İki avlak arası | merkezler ≥ alanA + alanB + büyük aggro + 4 m (komşu avlağın üyesi, çekilen avlağın kenarında duran oyuncuyu fark etmez) |
| Güvenli bölge | alan + aggro dışarıda |
| Geçit / işaretler (sunucu açılışı) | geçit yarıçapı + 12 m + alan + aggro; `spawn_` / `arrive_` 8 m (boss: tasma + 10 m) |
| Arazi (sunucu açılışı) | merkez, her yuva ve gezinme çemberinin 8 noktası yürünebilir; her yuvadan merkeze düz yürünebilir çizgi; her yuva bölgenin ilk `spawn_`/`arrive_` işaretinden yürüyerek (4-komşu taşma doldurma) erişilebilir |
| Sv. 1–2 avlakları | halka 7 m (alan 16 m), bu türlerin fark yarıçapı 6 → 4 m: dışarıdan yaklaşan oyuncu tek üye çeker (seviye 1 karakter iki üyeyle başa çıkamıyordu: `integration.stats` 5 kez öldü) |
| Tasma | sıradan avlaklarda 20 m (yuvadan), boss'larda şablonunki (22–24 m) |
| Yeniden doğma (kendi yuvasında) | Sv. 1–2: 12 s, 3–4: 18 s, 5–10: 20 s, 11–19: 25 s; boss'lar: Kor Bekçisi 10 dk (90 s'den), Anız Ağası / Körük Hanı 20 dk, Cüruf Anası 30 dk, Ocak Efendisi 45 dk |
| Zincirleme çekme | yok (M9'dan beri canavarlar yalnız kendi fark yarıçapıyla saldırıyor); aralık kuralı komşuyu bu yarıçapın dışında tutuyor (`SlotZoneTests`) |
| İstemci | oyuncu alan + 6 m içindeyken bölge adının altında (Güvenli Bölge satırının yerinde) amber bir satır: "◆ Kül Çukuru 2 · Sv. 1-2"; otomasyon durumları `slot`, `slot_<anahtar>` |

Bölge haritaları: Ortaocak ve Tanyeri'de yer yeterliydi (arazileri aynı; avlaklar boş yerlere kondu, dekor 35.3'teki "açık
zemin" kuralıyla yeniden dağıtıldı). Demirağız'ın vadisi dardı:
 `gen_zones.py` havzaları genişletti — batı (160/245,
r 46), doğu (352/200, r 46), güneybatı (176/148, r 38; eskisi 24) ve yeni güneydoğu havzası (335/132, r 30). `monster_circles`
artık avlak alanlarını da (alan + 2,5 m) dekor dışı tutuyor; bölge 2 ve 3 yeniden üretildi (Blender başsız, `--no-preview`;
yalnız yürünebilirlik önizlemeleri güncel). Tüccarlar, geçitler, `check_merchants`, `check_road_paths` değişmedi / geçti.
Mini harita yok (oyunda yok).

Avlaklar (46; alan = halka + gezinme; uzaklık bölgenin kasaba meydanından):

| Bölge | Avlak (anahtar) | Ad | Merkez | Canavar (sv.) | Üye | Halka / alan | Yeniden doğma | Tasma | Kasabaya uzaklık |
|---|---|---|---|---|---|---|---|---|---|
| 1 | esik_kul_1 | Kül Çukuru 1 | 213 / 298 | Kül Çömezi (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 60 m |
| 1 | esik_kul_2 | Kül Çukuru 2 | 198 / 240 | Kül Çömezi (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 60 m |
| 1 | esik_kul_3 | Kül Çukuru 3 | 237 / 187 | Kül Çömezi (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 72 m |
| 1 | esik_kul_4 | Kül Çukuru 4 | 322 / 236 | Kül Çömezi (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 69 m |
| 1 | esik_curuf_1 | Cüruf Yatağı 1 | 146 / 265 | Cüruf Yürüyen (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 110 m |
| 1 | esik_curuf_2 | Cüruf Yatağı 2 | 201 / 160 | Cüruf Yürüyen (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 111 m |
| 1 | esik_curuf_3 | Cüruf Yatağı 3 | 316 / 152 | Cüruf Yürüyen (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 120 m |
| 1 | esik_bekci | Bekçi Közlüğü | 210 / 338 | Kor Bekçisi (6-6) | 1 | 0 / 4 m | 600 s | 22 m | 94 m |
| 1 | esik_is_1 | İs Korusu 1 | 140 / 370 | İs Avcısı (11-13) | 6 | 7 / 8 m | 25 s | 20 m | 163 m |
| 1 | esik_is_2 | İs Korusu 2 | 106 / 323 | İs Avcısı (11-13) | 6 | 7 / 8 m | 25 s | 20 m | 164 m |
| 1 | esik_is_3 | İs Korusu 3 | 171 / 399 | İs Avcısı (11-13) | 6 | 7 / 8 m | 25 s | 20 m | 166 m |
| 1 | esik_dokum_1 | Döküm Taşlığı 1 | 108 / 113 | Cüruf Zırhlısı (14-16) | 6 | 6.5 / 7.5 m | 25 s | 20 m | 206 m |
| 1 | esik_dokum_2 | Döküm Taşlığı 2 | 150 / 95 | Cüruf Zırhlısı (14-16) | 6 | 6.5 / 7.5 m | 25 s | 20 m | 193 m |
| 1 | esik_dokum_3 | Döküm Taşlığı 3 | 90 / 160 | Cüruf Zırhlısı (14-16) | 6 | 6.5 / 7.5 m | 25 s | 20 m | 192 m |
| 1 | esik_kor_1 | Kor Halkası 1 | 400 / 100 | Kor Büyücüsü (17-19) | 6 | 7 / 8 m | 25 s | 20 m | 212 m |
| 1 | esik_kor_2 | Kor Halkası 2 | 430 / 135 | Kor Büyücüsü (17-19) | 6 | 7 / 8 m | 25 s | 20 m | 212 m |
| 1 | esik_kor_3 | Kor Halkası 3 | 366 / 89 | Kor Büyücüsü (17-19) | 6 | 7 / 8 m | 25 s | 20 m | 200 m |
| 1 | esik_cuana | Cüruf Gölü | 85 / 250 | Cüruf Anası (15-15) | 1 | 0 / 3 m | 1800 s | 22 m | 171 m |
| 1 | esik_ocak | Ocak Tahtı | 420 / 420 | Ocak Efendisi (20-20) | 1 | 0 / 3 m | 2700 s | 24 m | 232 m |
| 2 | tanyeri_aniz_1 | Anız Tarlası 1 | 180 / 368 | Anız Yanığı (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 100 m |
| 2 | tanyeri_aniz_2 | Anız Tarlası 2 | 175 / 229 | Anız Yanığı (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 101 m |
| 2 | tanyeri_aniz_3 | Anız Tarlası 3 | 330 / 222 | Anız Yanığı (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 109 m |
| 2 | tanyeri_aniz_4 | Anız Tarlası 4 | 338 / 362 | Anız Yanığı (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 110 m |
| 2 | tanyeri_yanik_1 | Yanık Karakol 1 | 136 / 212 | Yanık Nöbetçi (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 142 m |
| 2 | tanyeri_yanik_2 | Yanık Karakol 2 | 370 / 203 | Yanık Nöbetçi (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 152 m |
| 2 | tanyeri_yanik_3 | Yanık Karakol 3 | 254 / 131 | Yanık Nöbetçi (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 165 m |
| 2 | tanyeri_kavruk_1 | Kavruk Ekin 1 | 100 / 390 | Kavruk Tırpancı (5-7) | 6 | 6.5 / 7.5 m | 20 s | 20 m | 177 m |
| 2 | tanyeri_kavruk_2 | Kavruk Ekin 2 | 130 / 420 | Kavruk Tırpancı (5-7) | 6 | 6.5 / 7.5 m | 20 s | 20 m | 173 m |
| 2 | tanyeri_kavruk_3 | Kavruk Ekin 3 | 76 / 349 | Kavruk Tırpancı (5-7) | 6 | 6.5 / 7.5 m | 20 s | 20 m | 182 m |
| 2 | tanyeri_harman_1 | Harman Yeri 1 | 208 / 88 | Harman Devi (8-10) | 6 | 6 / 7 m | 20 s | 20 m | 212 m |
| 2 | tanyeri_harman_2 | Harman Yeri 2 | 301 / 94 | Harman Devi (8-10) | 6 | 6 / 7 m | 20 s | 20 m | 208 m |
| 2 | tanyeri_harman_3 | Harman Yeri 3 | 166 / 117 | Harman Devi (8-10) | 6 | 6 / 7 m | 20 s | 20 m | 198 m |
| 2 | tanyeri_aga | Ağa Harmanı | 415 / 255 | Anız Ağası (10-10) | 1 | 0 / 3 m | 1200 s | 22 m | 170 m |
| 3 | demiragiz_kiragi_1 | Kırağı Taşlığı 1 | 222 / 268 | Kırağı Kayası (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 83 m |
| 3 | demiragiz_kiragi_2 | Kırağı Taşlığı 2 | 292 / 268 | Kırağı Kayası (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 84 m |
| 3 | demiragiz_kiragi_3 | Kırağı Taşlığı 3 | 205 / 238 | Kırağı Kayası (1-2) | 6 | 7 / 8 m | 12 s | 20 m | 118 m |
| 3 | demiragiz_donuk_1 | Donuk Ocaklar 1 | 148 / 262 | Donuk Ocakçı (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 136 m |
| 3 | demiragiz_donuk_2 | Donuk Ocaklar 2 | 152 / 226 | Donuk Ocakçı (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 157 m |
| 3 | demiragiz_donuk_3 | Donuk Ocaklar 3 | 238 / 200 | Donuk Ocakçı (3-4) | 6 | 6.5 / 7.5 m | 18 s | 20 m | 145 m |
| 3 | demiragiz_koruk_1 | Körük Çukuru 1 | 340 / 222 | Buzul Körükçü (5-7) | 6 | 6.5 / 7.5 m | 20 s | 20 m | 148 m |
| 3 | demiragiz_koruk_2 | Körük Çukuru 2 | 372 / 212 | Buzul Körükçü (5-7) | 6 | 6.5 / 7.5 m | 20 s | 20 m | 176 m |
| 3 | demiragiz_koruk_3 | Körük Çukuru 3 | 350 / 180 | Buzul Körükçü (5-7) | 6 | 6.5 / 7.5 m | 20 s | 20 m | 189 m |
| 3 | demiragiz_ors_1 | Örs Meydanı 1 | 176 / 166 | Örs Kıran (8-10) | 6 | 6 / 7 m | 20 s | 20 m | 195 m |
| 3 | demiragiz_ors_2 | Örs Meydanı 2 | 164 / 132 | Örs Kıran (8-10) | 6 | 6 / 7 m | 20 s | 20 m | 231 m |
| 3 | demiragiz_ors_3 | Örs Meydanı 3 | 286 / 161 | Örs Kıran (8-10) | 6 | 6 / 7 m | 20 s | 20 m | 185 m |
| 3 | demiragiz_han | Han Ocağı | 335 / 132 | Körük Hanı (10-10) | 1 | 0 / 3 m | 1200 s | 22 m | 226 m |

### 35.3 Yeni canavarlar, bosslar, modeller

Seviye başına bir temel (can / saldırı / savunma / xp: Sv. 5 210/23/13/103 … Sv. 19 760/67/49/664; xp = 75 · (L/4)^1.4),
türe göre çarpanlar (ör. Harman Devi can ×1,3, saldırı ×1,15; Cüruf Zırhlısı savunma ×1,4; Kor Büyücüsü 7 m menzil) ve zırhı
karşılamak için saldırıya +2…+6. Hedef (`tune` hesabı, M9 kuralıyla aynı): seviyesindeki satılan donanımla bir savaşçı
seviyesindeki canavarı ~6 vuruşta öldürür, ~9 vuruş dayanır; büyücü / haydut / rahip ±%20. Boss'lar seviyelerinin avlak
canavarından ≥3× can, daha yüksek saldırı (test: `HuntingTests`). Hepsi `elite = true`: hedef çerçevesinde altın "Boss"
çipi, ad etiketinde "· Boss" (önce "Elit").

**Modeller**: main'deki sanat birleştirildikten sonra (c6d81ae) on bir yeni şablon kendi modelini (`model` = anahtar) ve
sanat ajanının oyunda ayarladığı ölçeği kullanıyor (35.11): Kavruk Tırpancı 1,05, Harman Devi 1,10, Buzul Körükçü 1,00, Örs
Kıran 1,20, İs Avcısı 1,15, Cüruf Zırhlısı 1,40, Kor Büyücüsü 1,35 (bir türün üç seviyesi aynı ölçekte), Anız Ağası 1,70,
Körük Hanı 1,70, Cüruf Anası 1,90, Ocak Efendisi 1,60. Halka / çarpışma yarıçapı verisi yok (istemci halkayı ölçekten
çiziyor). Birleştirmeden önce geçici olarak mevcut modeller ödünç alınmıştı (yalnız bu dalın geçmişinde).

**Açık zemin**: avlaklar ve bosslar ağaç gölgesinde kalmasın diye (sanat incelemesi: Anız Ağası gölgede okunmuyordu) üç
bölgenin üreticisi her avlağın alanı + 8 m içine ağaç ve kaya koymuyor (`gen_terrain.slot_areas`, `SLOT_MARGIN`; ev, çit, fener
gibi yerleşim dekoru etkilenmiyor). Üç bölge yeniden üretildi (başsız, `--no-preview`): bölge 1'de ağaçlar yer değiştirdi
(meşe 249 → 246, çam 180 → 181), bölge 2'de (meşe 139 → 145, çam 111 → 105), bölge 3'te bir yol mangalı Kırağı Taşlığı 3'e
yer açmak için düştü (12 → 11). Ortaocak'ın köy içi (evler, tezgâhlar, tüccarlar) değişmedi; köye yakın üç Kül Çukuru'nun
yanında köyün çit / fener dekoru kalıyor.


| Id | Ad | Sv. | Can | Saldırı | Savunma | Menzil | Aralık ms | Hız | Fark yarıçapı | XP | Yeniden doğma | Geçici model (ölçek) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 5 | Kor Bekçisi (boss) | 6 | 900 | 38 | 14 | 2.6 | 2000 | 4.0 | 10.0 | 300 | 600 s | ember_warden (1.25) |
| 14 | Kavruk Tırpancı | 5 | 190 | 25 | 10 | 2.4 | 1600 | 4.3 | 7.0 | 103 | 20 s | scorched_reaper (1.05) |
| 15 | Kavruk Tırpancı | 6 | 215 | 28 | 13 | 2.4 | 1600 | 4.3 | 7.0 | 132 | 20 s | scorched_reaper (1.05) |
| 16 | Kavruk Tırpancı | 7 | 245 | 31 | 14 | 2.4 | 1600 | 4.3 | 7.0 | 164 | 20 s | scorched_reaper (1.05) |
| 17 | Harman Devi | 8 | 390 | 40 | 23 | 2.8 | 2600 | 3.0 | 7.0 | 238 | 20 s | threshing_brute (1.1) |
| 18 | Harman Devi | 9 | 430 | 43 | 25 | 2.8 | 2600 | 3.0 | 7.0 | 280 | 20 s | threshing_brute (1.1) |
| 19 | Harman Devi | 10 | 470 | 47 | 29 | 2.8 | 2600 | 3.0 | 7.0 | 325 | 20 s | threshing_brute (1.1) |
| 20 | Buzul Körükçü | 5 | 210 | 24 | 14 | 2.4 | 2000 | 3.4 | 7.0 | 103 | 20 s | glacier_bellowsman (1.0) |
| 21 | Buzul Körükçü | 6 | 240 | 27 | 18 | 2.4 | 2000 | 3.4 | 7.0 | 132 | 20 s | glacier_bellowsman (1.0) |
| 22 | Buzul Körükçü | 7 | 270 | 30 | 20 | 2.4 | 2000 | 3.4 | 7.0 | 164 | 20 s | glacier_bellowsman (1.0) |
| 23 | Örs Kıran | 8 | 375 | 41 | 21 | 2.6 | 2400 | 3.2 | 7.0 | 238 | 20 s | anvil_breaker (1.2) |
| 24 | Örs Kıran | 9 | 410 | 45 | 23 | 2.6 | 2400 | 3.2 | 7.0 | 280 | 20 s | anvil_breaker (1.2) |
| 25 | Örs Kıran | 10 | 450 | 49 | 26 | 2.6 | 2400 | 3.2 | 7.0 | 325 | 20 s | anvil_breaker (1.2) |
| 26 | İs Avcısı | 11 | 370 | 45 | 26 | 2.2 | 1500 | 4.6 | 9.0 | 309 | 20 s | soot_stalker (1.15) |
| 27 | İs Avcısı | 12 | 395 | 48 | 28 | 2.2 | 1500 | 4.6 | 9.0 | 349 | 20 s | soot_stalker (1.15) |
| 28 | İs Avcısı | 13 | 425 | 51 | 31 | 2.2 | 1500 | 4.6 | 9.0 | 391 | 20 s | soot_stalker (1.15) |
| 29 | Cüruf Zırhlısı | 14 | 690 | 57 | 50 | 2.6 | 2400 | 2.8 | 7.0 | 520 | 20 s | slag_bulwark (1.4) |
| 30 | Cüruf Zırhlısı | 15 | 755 | 62 | 55 | 2.6 | 2400 | 2.8 | 7.0 | 572 | 20 s | slag_bulwark (1.4) |
| 31 | Cüruf Zırhlısı | 16 | 810 | 65 | 59 | 2.6 | 2400 | 2.8 | 7.0 | 626 | 20 s | slag_bulwark (1.4) |
| 32 | Kor Büyücüsü | 17 | 550 | 75 | 35 | 7.0 | 2200 | 3.6 | 10.0 | 626 | 20 s | ember_warlock (1.35) |
| 33 | Kor Büyücüsü | 18 | 595 | 78 | 38 | 7.0 | 2200 | 3.6 | 10.0 | 678 | 20 s | ember_warlock (1.35) |
| 34 | Kor Büyücüsü | 19 | 645 | 83 | 39 | 7.0 | 2200 | 3.6 | 10.0 | 730 | 20 s | ember_warlock (1.35) |
| 35 | Ocak Efendisi (boss) | 20 | 9000 | 105 | 62 | 3.0 | 2200 | 4.2 | 12.0 | 4000 | 2700 s | hearth_lord (1.6) |
| 36 | Anız Ağası (boss) | 10 | 2800 | 58 | 30 | 2.8 | 2000 | 4.0 | 10.0 | 1300 | 1200 s | stubble_lord (1.7) |
| 37 | Körük Hanı (boss) | 10 | 3000 | 56 | 33 | 2.8 | 2200 | 3.6 | 10.0 | 1300 | 1200 s | bellows_khan (1.7) |
| 38 | Cüruf Anası (boss) | 15 | 5200 | 82 | 52 | 3.0 | 2400 | 3.4 | 11.0 | 2400 | 1800 s | slag_matron (1.9) |

### 35.4 Eşyalar: kademeler, zırh, nadirlik, Eşsiz eşyalar

Kimlikler 20–165 (146 yeni şablon). Yeni öğeler (veri modeli, `PROTOCOL_LEVELS.md` §4):

* **Zırh** (`kind = "armor"`; baş, gövde, bacak, eller, ayak): Kuşanma kabının 2–6. yuvaları v2'den beri protokolde ve
  veritabanında vardı (göç gerekmedi); envanter penceresi yuvaları zaten çiziyordu. Zırh yalnız kendi yuvasına girer,
  savunma ve nitelikleri silah gibi sayılır, çift tıklama / sağ tık doğru yuvaya koyar. Karakterin üstünde çizilmiyor (model
  yok) ve başkalarına gösterilmiyor (`ENTITY_EQUIP` iki el).
* **Nadirlik**: Sıradan (açık krem), Seyrek (yeşil), Nadir (mavi), **Eşsiz** (`unique`, amber `#F0A23C`). Eşsizler
  satılamaz / satın alınamaz (`value = 0`, ipucunda "Satılamaz"); ganimet sahipliği, çanta, yığın kuralları değişmedi.
  Sunucu genel duyuru yapmıyor.
* **`tint = [r, g, b]`**: simge her yerde (çanta, kuşanılanlar, ganimet, dükkân, adet penceresi, çubuk, sürükleme, karakter
  listesi) ve elde silah örgüsü (sRGB → doğrusal) bu renkle çarpılıyor; aynı sanatı paylaşan eşyalar ayırt ediliyor.
* Aksesuar yuvaları (yüzük, küpe, kolye, kemer) **eklenmedi**: Kuşanma kabında yalnız bir boş yuva (7, "Takı") var; dört
  yeni yuva protokol (kap boyu 8), göç, pencere düzeni ve görsel gerektirir. Ayrı bir kilometre taşı olarak öneriliyor.

Kademeler (satılanlar sıradan; düşenler seyrek / nadir):

| Grup | Seviye | Nadirlik | Kaynak | Örnek değerler |
|---|---|---|---|---|
| Silah "Sınır" (8 tür) | 5 | Sıradan | Usta Berkay, Örsbaşı Gürşad; Sv. 5–7 avlaklar %0,3 | kılıç 11, balta 18, topuz 9, hançer 7, pala 8, asa 12, değnek 11, kalkan 6 sav. |
| Silah "Harman / Örs / Kırağı" | 8 | Seyrek (+2 ana nitelik; balta +3; topuz +1 sav.) | Sv. 8–10 avlaklar %0,4, Anız Ağası / Körük Hanı %8 | kılıç 13 … kalkan 8 |
| Silah "Eşik" | 11 | Sıradan | Demirci Ilgaz (Ortaocak) | kılıç 16, balta 26, asa 18, kalkan 10 |
| Silah "İs / Cüruf / Kurum" | 14 | Seyrek (+3; balta +4) | İs Avcısı (hafifler) / Cüruf Zırhlısı (ağırlar) %0,3–0,4, Cüruf Anası %8 | kılıç 19 … kalkan 12 |
| Silah "Kor" | 17 | Nadir (+5 ana, +2 ikinci) | Kor Büyücüsü %0,2, Ocak Efendisi %6 | kılıç 22, balta 35, asa 25, kalkan 14 |
| Zırh "Eski" (3 hat × 5 yuva) | 1 | Sıradan | Sv. 1–4 avlaklar %0,4–0,5 | takım savunması ağır 4 |
| Zırh "Sağlam" | 5 | Sıradan (+1 göğüste) | yurt demircileri (gövde, bacak), Sv. 5–7 avlaklar %0,4 (hepsi) | ağır takım 8 |
| Zırh "Ayazlı" | 9 | Seyrek (+1 her parça, +2 göğüs) | Sv. 8–10 avlaklar %0,3, yurt boss'ları %8 (göğüs) | ağır takım 12 |
| Zırh "Yeminli" | 13 | Sıradan (+2 göğüs, +1 bacak) | Demirci Ilgaz (gövde, bacak), İs Avcısı %0,3 (hepsi) | ağır takım 16 |
| Zırh "Korlu" | 17 | Nadir (+2…+4, göğüste ikinci nitelik +2) | Cüruf Zırhlısı %0,15 (baş/gövde/bacak), Kor Büyücüsü %0,15, Cüruf Anası %4 (bacak), Ocak Efendisi %4 (gövde/baş) | ağır takım 20 |

Zırh hatları: **Ağır** (savaşçı, rahip; savunma ×1, Dayanıklılık), **Deri** (haydut, savaşçı, büyücü; ×0,7, Çeviklik),
**Bez** (büyücü, rahip, haydut; ×0,45, Zekâ / İrade). Ağır takımın hedef savunması 4 / 8 / 12 / 16 / 20 (Sv. 1 / 5 / 9 / 13 / 17); gövde %35, bacak %25, baş %15, eller / ayak
%12,5, yuvarlanır, parça başına en az 1 (gerçek ağır takımlar 5 / 8 / 13 / 16 / 21).
Her sınıfın her zırh yuvasında her seviye bandında ≥2 seçenek; her sınıfın ≥2 silah türü (savaşçı kılıç / balta / topuz,
haydut hançer / pala, büyücü asa / değnek, rahip topuz / değnek). İksirler: Ocak Şerbeti (can 360, 55 Akçe), Buzul Çiyi
(mana 200, 55 Akçe) üç iksircide. Yeni malzemeler (her yeni türün): Kavruk Başak 6, Harman Taşı 9, Körük Derisi 6, Örs Kıymığı 9,
İs Tozu 12, Cüruf Levhası 16, Kor Mürekkebi 20, Ocak Yüreği 160 (değer; satış yarısı).

**Düşme ağırlıkları** (`loot.toml`; bir tabloda en çok 24 satır, bir zar en çok 8 eşya tutar): sıradan donanım parça başı
%0,4–0,5, seyrek %0,3–0,4, nadir %0,15–0,2; bosslar kendi kademelerini %4–8 ile, **Eşsizleri boss başına %4–6** ile
(Kor Bekçisi 4 × %5, Anız Ağası 4 × %6, Körük Hanı 4 × %6, Cüruf Anası 4 × %5, Ocak Efendisi 5 × %4; bir boss öldürmede en az
bir Eşsiz olasılığı ~%19–22).

**Eşsizler** (21; düzey = boss'un seviyesi; aynı yuva ve sınıf için o seviyeye kadarki her sıradan / seyrek / nadir eşyadan
güçlü — `HuntingTests`):

| Boss | Eşsizler |
|---|---|
| Kor Bekçisi (6) | Bekçinin Közlü Kılıcı (kılıç 12, Güç +3, Day +2), Kor Nöbet Asası (asa 14, Zekâ +4, İrade +2), Közden Örme Yelek (deri gövde 5, Çev +4), Bekçi Tolgası (ağır baş 4, Day +3, İrade +2) |
| Anız Ağası (10) | Ağanın Tırpan Palası (pala 12, Çev +5, Güç +2), Ağanın Harman Topuzu (topuz 13, sav 1, İrade +5, Day +2), Yanık Sancak Cübbesi (bez gövde 5, Zekâ +4, İrade +3), Ağa Çizmesi (ağır ayak 4, Day +3, Güç +2) |
| Körük Hanı (10) | Hanın Buz Baltası (balta 24, Güç +5, Day +2), Körük Nefesi Değneği (değnek 15, Zekâ +4, İrade +4), Ayaz Hançeri (hançer 10, Çev +5), Han Eldiveni (deri eller 3, Çev +3, Güç +2) |
| Cüruf Anası (15) | Ananın Döküm Kalkanı (kalkan 16, Day +6), Cüruf Gözü Asası (asa 25, Zekâ +7), Döküm Dizlik (ağır bacak 7, Day +4, Güç +3), Kurum Şalvarı (bez bacak 4, Zekâ +4, İrade +4) |
| Ocak Efendisi (20) | Ocak Yemini (kılıç 27, Güç +8, Day +4), Son Kıvılcım (hançer 18, Çev +8), Közbeyi Asası (asa 31, Zekâ +8, İrade +4), Kül Tacı (bez baş 5, Zekâ +4, İrade +6), Efendi Göğüs Zırhı (ağır gövde 12, Day +6, Güç +3) |

**Ortak sanat** (yeni sanat ısmarlanabilsin diye): yeni silahların hepsi mevcut 8 silah modelini ve 10 simgeyi renkle paylaşıyor
(kılıç / balta `sword_keep` / `axe_hearth`, topuz `mace_sun` + simge `mace_slag`, hançer `dagger_hunter` + `dagger_thrall`, pala
`sword_soldier` + `sword_charred`, asa `staff_ember`, değnek `staff_frost`, kalkan `shield_oak` + `shield_warden`); 75 zırh ve 9
Eşsiz zırh beş yeni yer tutucu simgeyi (`armor_head/chest/legs/hands/feet`, `tools/blender/gen_armor_icons.py`, başsız
Blender, açık çelik + kayış; hat ve kademe rengi `tint` ile) paylaşıyor; malzemeler `ash_dust` / `slag_shard` / `ember_core`,
iksirler `potion_hp_medium` / `potion_mp_medium` simgelerini renkle. Zırh karakterde görünmüyor. Tam liste 35.10'da.


### 35.5 Ekonomi (`vo_tests "[economy]"`, `VO_ECONOMY_TABLE=1`)

Model §32.6'daki gibi; M15 eklemeleri: satılan en iyi zırh parçaları (gövde, bacak) da alınıyor, av seçiminde
`xpForKill` düşüşü sayılıyor, seviye 20'de (xp yok) en çok Akçe getiren av seçiliyor. Yeni kontroller: 1–20 boyunca kazanılan
Akçe satılan bütün donanımın ≥3 katı; gelir seviyeyle artıyor; her seviyenin en çok bir seviye üstünde bir avı var.
Ölçülen (dinlenme modu, Akçe / saat; iksir modunda kazanılan / iksire giden):

| Sınıf | Sv. 12 (av) | Sv. 16 (av) | Sv. 20 (av) | 1–20 toplam | saf av süresi 1→20 | satın alınan donanım |
|---|---|---|---|---|---|---|
| Savaşçı | 5 059 (İs Avcısı 13) | 8 059 (Kor Büyücüsü 17) | 9 609 (Kor Büyücüsü 17) | 36 108 | 5,7 sa | 1 529 |
| Haydut | 5 554 (İs Avcısı 13) | 9 296 (Kor Büyücüsü 17) | 11 080 (Kor Büyücüsü 17) | 35 221 | 5,1 sa | 888 |
| Büyücü | 4 164 (Harman Devi 10) | 6 448 (Kor Büyücüsü 17) | 7 949 (Kor Büyücüsü 17) | 38 147 | 7,1 sa | 1 008 |
| Rahip | 4 299 (Harman Devi 10) | 6 952 (Kor Büyücüsü 17) | 8 496 (Kor Büyücüsü 17) | 37 312 | 6,7 sa | 1 029 |

İksir modunda iksire giden pay Sv. 12'de %23–34, Sv. 16–20'de %14–18. Sv. 9 geliri §32.6'daki 3 000'den 3 300–3 600'e
çıktı; Sv. 12'de ~4 200–5 600, Sv. 20'de ~8 000–11 000. İlk ölçümde Sv. 20 savaşçı saatte ~11 500 kazanıyordu; İs Avcısı / Cüruf Zırhlısı /
Kor Büyücüsü paraları ~%13 düşürüldü (34–60 → 30–52, 44–76 → 38–64, 56–96 → 46–78), şimdi ~9 600. Satılan
en pahalı eşya 485 Akçe (Eşik Baltası); birikim sürüyor: Akçe çıkışı (güçlendirme) bir sonraki kilometre taşı.

### 35.6 İstemci

| Konu | Ne |
|---|---|
| Seviye 11–20 | HUD, Künye, karakter listesi sabitten okuyor; deneyim ipucu Sv. 20'de "En yüksek seviyedesin." (ekran görüntüleri) |
| Avlak satırı | `WorldScene::currentSlot` (paylaşılan `MonsterData::slotAt`, alan + 6 m); `world.slotname` etiketi, Güvenli Bölge satırının yerinde, `TextRole::Small`, amber, sağa yaslı; ölçek dünya sahnesinin HUD ölçeğiyle; bölge bandı düşerken altına iner |
| Boss işareti | hedef çerçevesinde "Boss" çipi, etikette "· Boss", tür satırı "Boss canavar" |
| Eşsiz | ad rengi amber, ipucunda "Eşsiz" ve "Satılamaz" |
| Zırh | ipucunda "Zırh · Baş / Gövde / …"; çanta → yuva (çift tık, sağ tık, sürükle) paylaşılan kurallarla |
| Renk | `SceneResources::itemTint` her simge çiziminde; `AttachedMesh::tint` elde silahta |
| Yetenekler | kademe "x/5"; 4. ve 5. kademenin ipucu seviyesini söylüyor ("Sonraki kademe (11. seviye): …"), `LevelTooLow` metni "Kademe 4 için 11. seviyeye ulaşmalısın." |
| Otomasyon | durumlar `slot`, `slot_<anahtar>`; `scene movestack unique <yuva>` |

Protokol v9 (sürüm artışı + `docs/PROTOCOL_LEVELS.md`): yalnız `SKILL_*` kademe aralığı 0..5 ve `LevelTooLow = 6`.

### 35.7 Test kancaları (sunucu yapılandırması, varsayılan kapalı, günlüğe uyarı)

| Anahtar | Etki | Kullanan |
|---|---|---|
| `world.testing_boss_respawn_ms` | boss'lar bu sürede yeniden doğar | `integration.hunt`, `m15_unique` |
| `world.testing_force_unique_drop` | her boss ganimeti bir Eşsiz içerir (`game::forceUniqueDrop`) | `integration.hunt`, `m15_unique`, `SlotZoneTests` |
| `world.testing_xp_percent` | deneyim kazancı ölçeklenir | `integration.hunt` (100 000 %: bir Kül Çömezi = Sv. 12) |
| `world.testing_enter_level` | dünyaya giren karakter en az bu seviyede (xp 0, can / mana dolu, kaydedilir) | `m15_*` akışları (Sv. 20) |
| `world.testing_monster_attack_percent` | canavar saldırısı ölçeklenir | `m15_*` akışları (%40: başlangıç donanımıyla Sv. 14–19 avlakları) |


### 35.8 Testler, akışlar, ekran görüntüleri

Yeni birim testleri: `tests/game/HuntingTests.cpp` (avlak ayrıştırma ve kuralları, arazi kuralları — yürünebilirlik, düz
çizgi, erişilebilirlik, geçit / işaret; sevk edilen dünya: her canavar bir avlakta, 6–8 üye, türlerin ≥3 avlağı, yeni türlerin
ad / seviye / bölgeleri, seviyeler kasabadan uzaklaştıkça artıyor; zırh kuralları; 146 eşyanın her biri bir başlangıç
takımından, dükkândan ya da ganimet tablosundan erişilebilir, her sınıf / zırh yuvası / seviye bandında ≥2 seçenek; her
boss'un ≥3 Eşsizi, satılamaz, aynı yuvadaki her eşyadan güçlü; zorlanan Eşsiz düşüşü), `tests/server/world/SlotZoneTests.cpp`
(üyeler yuvalarında, avlağın tasması / yeniden doğması, öldürülen üye kendi yuvasında geri gelir, komşu avlak çekilmez ve
tasma onları eve döndürür, açılış kuralları, boss kancaları), `CombatTests` (eğri 10–19, toplam 295 010), `SkillsTests`
(kademe 4–5, `LevelTooLow`, `skillRanksFitLevel`), `EconomyTests` (M15 modeli). Değişen beklentiler: kademe 3 → 5, protokol
8 → 9, `LootTests` / `ShopTests` / `MonsterSpawnTests` / `WeaponTests` M15 verisine.

Uçtan uca: `integration.hunt` (yeni, ~36 s: seviye 12'ye, üye 12 s içinde kendi yuvasında, Ağır Darbe kademe 4 Sv. 12'de,
kademe 5 `LevelTooLow`), `integration.db.hunt` (test veritabanı yok: atlandı). Yük botları (`integration.load`) avlakları
seviyelerine göre avlıyor (geçit kaçınması değişmedi).

UI akışları (veritabanısız, kancalarla): `m15_homelands` (2 koşu), `m15_esik` (1), `m15_unique` (2: 1280x720, 1920x1080).
Ekran görüntüleri `docs/screenshots/m15/`: `tanyeri_*` (Kavruk Ekin / Harman Yeri yaklaşma, dövüş, öldürme), `demiragiz_*`
(Körük Çukuru / Örs Meydanı), `esik_*` (İs Korusu / Döküm Taşlığı / Kor Halkası), `unique_1280_*` / `unique_1920_*` (Sv. 20
HUD, avlak satırı, boss dövüşü, Eşsiz düşüşü — amber ad —, Eşsiz ipucu).

Geliştirmede düşen ve düzeltilen: avlak yuvası duvara denk gelen iki Demirağız avlağı (yer değiştirdi, bölge 3 yeniden
üretildi), Kor Bekçisi'nin 4 m gezinmesi (sınır 3 → 5 m), seviye 20'de ekonomi modelinin xp'siz av seçimi, `hunt` senaryosunda
eski ölüm kaydı yüzünden yeniden doğuşun görülmemesi, kancayla Sv. 20'ye çıkan karakterin hesap hizmetinde puan harcayamaması
(akışlar harcamıyor), 6 m halkalı avlaklarda yaklaşma noktasının alanın 13 m dışında kalması, bir yaklaşma görüntüsünde
kameranın kayalığın içinde kalması; ilk tam koşuda `integration.stats` ve `integration.skills` (seviye 1 savaşçı altılı Kül
Çukuru'nda 4–5 kez öldü: iki üye birden çekiliyordu ve 12 s'lik yeniden doğuşla dinlenmeden dövüşe giriyordu) — Sv. 1–2
avlakları 7 m halka + 4 m fark yarıçapı (35.2), avcı botlar (`fightThrall`) %70 canın altında %90'a kadar oturuyor;
`fix1_monsters` akışının Yanık Nöbetçi yürüyüşü yeni avlağa (Yanık Karakol 1) yönlendirildi.


**Son tam koşular**: Debug `.\build.ps1 -Test` 673/673 geçti (16 `integration.db.*` testi atlandı: çalışma kopyasında test veritabanı ayarı yok), ~31 dk. Tam `run-ui-flows.ps1`: 106 koşunun 95'i ilk tam koşuda geçti; düşen 11 koşu (eski akışların M15 verisine takılan beklentileri: kademe metni x/3 → x/5, avlağa taşınan Yanık Nöbetçi yürüyüşü, geçit ipucu için varış yayılması, uzayan dükkân stokları — sınırda kaydırma, Demirağız'da genişleyen vadinin yol mangalına çarpan rotası, Kor Bekçisi'nin 10 m fark yarıçapına giren haydut) düzeltildikten sonra yeniden koşturuldu ve hepsi geçti (`-Only fix1_monsters,zones,skills_bar,skills_warrior,skills_rogue,shop,fix1_shop`, ardından `zones,shop`); düzeltmelerden sonra tek parça bir tam koşu yapılmadı. Release `.\build.ps1 -Config Release -Test`: derleme temiz, 673 testin hepsi geçti (`integration.db.*` yine atlandı), ~32 dk.

### 35.9 Bilinen eksikler / zayıf noktalar / öneriler

- **Zırh simgeleri** yer tutucu (basit başsız Blender modelleri; gövde küçük boyda bir kese gibi okunuyor); zırh karakterde
  çizilmiyor ve başkalarına görünmüyor. 84 zırh ve Eşsiz zırh beş simgeyi, 40 silah ve 12 Eşsiz silah sekiz modeli renkle
  paylaşıyor; iki büyük iksir orta iksirlerin simgesini (renkli) paylaşıyor.
- **Aksesuar yuvaları** eklenmedi (35.4): öneri ayrı kilometre taşı (kap boyu, göç, pencere, görsel).
- **Denge** yalnız hesapla (`tune`, `[economy]`, `HuntingTests`, `SlotZoneTests`) ve kancalı akışlarla denendi; gerçek bir
  1→20 oyunu oynanmadı. Ekonomi modeli yetenekleri ve düşen donanımı saymıyor (alt sınır); Sv. 12'de büyücü / rahip hâlâ
  Harman Devi'yi (Sv. 10) seçiyor (İs Avcısı'nın 9 m fark yarıçapı ve hızı onlara ağır geliyor).
- **Akçe birikimi**: Sv. 16–20'de saatte 6 500–11 000; satılan en pahalı eşya 485. Güçlendirme (sonraki kilometre taşı) şart.
- **Önizlemeler**: üç bölgenin üstten / eğik önizlemeleri ve yükleme ekranı çizimleri eski dekorun (yalnız yürünebilirlik
  önizlemeleri yeni); Demirağız'ın yeni havzaları yükleme ekranında görünmüyor (`gen_loading_art.py` elle, ~4 dk).
- **Sanat ölçekleri** bir türün üç seviyesinde aynı; seviye farkı yalnız ad etiketinde.

- `testing_enter_level` hesabı (hesap hizmeti) hemen güncellemiyor: kancayla giren karakter bir sonraki kayda kadar nitelik /
  yetenek puanı harcayamıyor (akışlar bu yüzden puanları harcamıyor; saldırı kancası %25).
- Avlak satırı yalnız konumla (alan + 6 m) gösteriliyor; savaşta kaçarken avlaktan çıkınca satır kayboluyor (bilerek).
- Boss'ların tasması 22–24 m: bir grup boss'u yuvasından en çok bu kadar uzaklaştırabilir; boss'ların özel yeteneği yok
  (yalnız can / saldırı).
- `integration.db.hunt` ve bütün `integration.db.*` bu worktree'de koşulmadı: worktree'de `config/server.local.toml` /
  test veritabanı ayarı yok (kimlik bilgisi aranmadı). Göç 0008 yalnız birim testlerinde (`PostgresSkillsTests`
  beklentileri güncellendi) — müdürün test veritabanında koşması gerekiyor.

### 35.10 Yeni eşyaların tam listesi (kimlik 20–165)

Sınıflar: Sv savaşçı, Hy haydut, Bü büyücü, Şi şifacı. Renk `tint` (yoksa boş). Kaynak: düşme şansı (canavar türü) ya da satan tüccar.

| Id | Ad | Tür / yuva | Sv. | Nadirlik | Saldırı | Savunma | Nitelik / etki | Değer | Simge / model (renk) | Kaynak |
|---|---|---|---|---|---|---|---|---|---|---|
| 20 | Kavruk Başak | material |  | Sıradan |  |  |  | 6 | ash_dust (200,160,90) | Kavruk Tırpancı 45 % |
| 21 | Harman Taşı | material |  | Sıradan |  |  |  | 9 | slag_shard (210,190,150) | Harman Devi 45 %; Anız Ağası 100 % |
| 22 | Körük Derisi | material |  | Sıradan |  |  |  | 6 | ash_dust (170,205,235) | Buzul Körükçü 45 % |
| 23 | Örs Kıymığı | material |  | Sıradan |  |  |  | 9 | slag_shard (175,190,215) | Örs Kıran 45 %; Körük Hanı 100 % |
| 24 | İs Tozu | material |  | Sıradan |  |  |  | 12 | ash_dust (95,95,105) | İs Avcısı 45 % |
| 25 | Cüruf Levhası | material |  | Sıradan |  |  |  | 16 | slag_shard (150,120,110) | Cüruf Zırhlısı 45 %; Cüruf Anası 100 % |
| 26 | Kor Mürekkebi | material |  | Seyrek |  |  |  | 20 | ember_core (230,120,90) | Kor Büyücüsü 45 % |
| 27 | Ocak Yüreği | material |  | Nadir |  |  |  | 160 | ember_core (255,200,120) | Ocak Efendisi 100 % |
| 28 | Ocak Şerbeti | consumable |  | Sıradan |  |  | can +360 | 55 | potion_hp_medium (185,120,120) | İs Avcısı 4 %; Cüruf Zırhlısı 4 %; Cüruf Anası 100 %; Ocak Efendisi 100 %; Sürmeli Bacı (satar); Rahibe Aksun (satar); Kül Anası Tamar (satar) |
| 29 | Buzul Çiyi | consumable |  | Sıradan |  |  | mana +200 | 55 | potion_mp_medium (160,230,255) | İs Avcısı 3 %; Cüruf Zırhlısı 3 %; Ocak Efendisi 100 %; Sürmeli Bacı (satar); Rahibe Aksun (satar); Kül Anası Tamar (satar) |
| 30 | Sınır Kılıcı | weapon / main_hand (Sv) | 5 | Sıradan | 11 |  |  | 140 | sword_keep / sword_keep (228,228,236) | Kavruk Tırpancı 0.3 %; Buzul Körükçü 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 31 | Sınır Baltası | weapon / two_hand (Sv) | 5 | Sıradan | 18 |  |  | 180 | axe_hearth / axe_hearth (228,228,236) | Kavruk Tırpancı 0.3 %; Buzul Körükçü 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 32 | Sınır Topuzu | weapon / main_hand (Sv,Şi) | 5 | Sıradan | 9 |  |  | 130 | mace_slag / mace_sun (228,228,236) | Buzul Körükçü 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 33 | Sınır Hançeri | weapon / either_hand (Hy) | 5 | Sıradan | 7 |  |  | 110 | dagger_thrall / dagger_hunter (228,228,236) | Kavruk Tırpancı 0.3 %; Buzul Körükçü 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 34 | Sınır Palası | weapon / either_hand (Hy) | 5 | Sıradan | 8 |  |  | 120 | sword_charred / sword_soldier (228,228,236) | Kavruk Tırpancı 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 35 | Kıvılcım Asası | weapon / two_hand (Bü) | 5 | Sıradan | 12 |  |  | 140 | staff_ember / staff_ember (228,228,236) | Kavruk Tırpancı 0.3 %; Buzul Körükçü 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 36 | Çiy Değneği | weapon / two_hand (Şi,Bü) | 5 | Sıradan | 11 |  |  | 130 | staff_frost / staff_frost (228,228,236) | Kavruk Tırpancı 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 37 | Sınır Kalkanı | shield / off_hand (Sv,Şi) | 5 | Sıradan |  | 6 |  | 120 | shield_warden / shield_oak (228,228,236) | Buzul Körükçü 0.3 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 38 | Harman Kılıcı | weapon / main_hand (Sv) | 8 | Seyrek | 13 |  | Güç +2 | 225 | sword_keep / sword_keep (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 39 | Harman Baltası | weapon / two_hand (Sv) | 8 | Seyrek | 21 |  | Güç +3 | 290 | axe_hearth / axe_hearth (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 40 | Örs Topuzu | weapon / main_hand (Sv,Şi) | 8 | Seyrek | 11 | 1 | İra +2 | 210 | mace_slag / mace_sun (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 41 | Kırağı Hançeri | weapon / either_hand (Hy) | 8 | Seyrek | 8 |  | Çev +2 | 175 | dagger_thrall / dagger_hunter (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 42 | Tırpan Palası | weapon / either_hand (Hy) | 8 | Seyrek | 9 |  | Çev +2 | 190 | sword_charred / sword_soldier (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 43 | Buzul Asası | weapon / two_hand (Bü) | 8 | Seyrek | 15 |  | Zek +2 | 225 | staff_ember / staff_ember (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 44 | Anız Değneği | weapon / two_hand (Şi,Bü) | 8 | Seyrek | 13 |  | İra +2 | 210 | staff_frost / staff_frost (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 45 | Örs Kalkanı | shield / off_hand (Sv,Şi) | 8 | Seyrek |  | 8 | Day +2 | 190 | shield_warden / shield_oak (190,214,255) | Harman Devi 0.4 %; Örs Kıran 0.4 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 46 | Eşik Kılıcı | weapon / main_hand (Sv) | 11 | Sıradan | 16 |  |  | 380 | sword_keep / sword_keep (255,236,190) | Demirci Ilgaz (satar) |
| 47 | Eşik Baltası | weapon / two_hand (Sv) | 11 | Sıradan | 26 |  |  | 485 | axe_hearth / axe_hearth (255,236,190) | Demirci Ilgaz (satar) |
| 48 | Eşik Topuzu | weapon / main_hand (Sv,Şi) | 11 | Sıradan | 14 |  |  | 350 | mace_slag / mace_sun (255,236,190) | Demirci Ilgaz (satar) |
| 49 | Eşik Hançeri | weapon / either_hand (Hy) | 11 | Sıradan | 10 |  |  | 295 | dagger_thrall / dagger_hunter (255,236,190) | Demirci Ilgaz (satar) |
| 50 | Eşik Palası | weapon / either_hand (Hy) | 11 | Sıradan | 12 |  |  | 325 | sword_charred / sword_soldier (255,236,190) | Demirci Ilgaz (satar) |
| 51 | Eşik Asası | weapon / two_hand (Bü) | 11 | Sıradan | 18 |  |  | 380 | staff_ember / staff_ember (255,236,190) | Demirci Ilgaz (satar) |
| 52 | Eşik Değneği | weapon / two_hand (Şi,Bü) | 11 | Sıradan | 16 |  |  | 350 | staff_frost / staff_frost (255,236,190) | Demirci Ilgaz (satar) |
| 53 | Eşik Kalkanı | shield / off_hand (Sv,Şi) | 11 | Sıradan |  | 10 |  | 325 | shield_warden / shield_oak (255,236,190) | Demirci Ilgaz (satar) |
| 54 | İs Kılıcı | weapon / main_hand (Sv) | 14 | Seyrek | 19 |  | Güç +3 | 530 | sword_keep / sword_keep (150,150,162) | Cüruf Zırhlısı 0.4 %; Cüruf Anası 8 % |
| 55 | Cüruf Baltası | weapon / two_hand (Sv) | 14 | Seyrek | 30 |  | Güç +4 | 685 | axe_hearth / axe_hearth (150,150,162) | Cüruf Zırhlısı 0.4 %; Cüruf Anası 8 % |
| 56 | Cüruf Gürzü | weapon / main_hand (Sv,Şi) | 14 | Seyrek | 16 | 1 | İra +3 | 495 | mace_slag / mace_sun (150,150,162) | Cüruf Zırhlısı 0.4 %; Cüruf Anası 8 % |
| 57 | İs Hançeri | weapon / either_hand (Hy) | 14 | Seyrek | 12 |  | Çev +3 | 420 | dagger_thrall / dagger_hunter (150,150,162) | İs Avcısı 0.3 %; Cüruf Anası 8 % |
| 58 | İs Palası | weapon / either_hand (Hy) | 14 | Seyrek | 14 |  | Çev +3 | 455 | sword_charred / sword_soldier (150,150,162) | İs Avcısı 0.3 %; Cüruf Anası 8 % |
| 59 | Kurum Asası | weapon / two_hand (Bü) | 14 | Seyrek | 21 |  | Zek +3 | 530 | staff_ember / staff_ember (150,150,162) | İs Avcısı 0.3 %; Cüruf Anası 8 % |
| 60 | Kurum Değneği | weapon / two_hand (Şi,Bü) | 14 | Seyrek | 19 |  | İra +3 | 495 | staff_frost / staff_frost (150,150,162) | İs Avcısı 0.3 %; Cüruf Anası 8 % |
| 61 | Cüruf Siperi | shield / off_hand (Sv,Şi) | 14 | Seyrek |  | 12 | Day +3 | 455 | shield_warden / shield_oak (150,150,162) | Cüruf Zırhlısı 0.4 %; Cüruf Anası 8 % |
| 62 | Kor Kılıcı | weapon / main_hand (Sv) | 17 | Nadir | 22 |  | Güç +5, Day +2 | 895 | sword_keep / sword_keep (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 63 | Kor Baltası | weapon / two_hand (Sv) | 17 | Nadir | 35 |  | Güç +5, Day +2 | 1150 | axe_hearth / axe_hearth (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 64 | Kor Gürzü | weapon / main_hand (Sv,Şi) | 17 | Nadir | 19 |  | Zek +2, İra +5 | 830 | mace_slag / mace_sun (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 65 | Kor Hançeri | weapon / either_hand (Hy) | 17 | Nadir | 14 |  | Güç +2, Çev +5 | 705 | dagger_thrall / dagger_hunter (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 66 | Kor Palası | weapon / either_hand (Hy) | 17 | Nadir | 16 |  | Güç +2, Çev +5 | 770 | sword_charred / sword_soldier (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 67 | Kor Asası | weapon / two_hand (Bü) | 17 | Nadir | 25 |  | Zek +5, İra +2 | 895 | staff_ember / staff_ember (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 68 | Kor Değneği | weapon / two_hand (Şi,Bü) | 17 | Nadir | 22 |  | Zek +2, İra +5 | 830 | staff_frost / staff_frost (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 69 | Kor Siperi | shield / off_hand (Sv,Şi) | 17 | Nadir |  | 14 | Güç +2, Day +5 | 770 | shield_warden / shield_oak (255,172,122) | Kor Büyücüsü 0.2 %; Ocak Efendisi 6 % |
| 70 | Eski Tolga | armor / head (Sv,Şi) | 1 | Sıradan |  | 1 |  | 8 | armor_head (150,150,152) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 71 | Eski Göğüs Zırhı | armor / chest (Sv,Şi) | 1 | Sıradan |  | 1 |  | 14 | armor_chest (150,150,152) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 72 | Eski Dizlik | armor / legs (Sv,Şi) | 1 | Sıradan |  | 1 |  | 10 | armor_legs (150,150,152) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 73 | Eski Demir Eldiven | armor / hands (Sv,Şi) | 1 | Sıradan |  | 1 |  | 6 | armor_hands (150,150,152) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 74 | Eski Demir Çizme | armor / feet (Sv,Şi) | 1 | Sıradan |  | 1 |  | 6 | armor_feet (150,150,152) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 75 | Eski Deri Başlık | armor / head (Hy,Sv,Bü) | 1 | Sıradan |  | 1 |  | 7 | armor_head (135,100,70) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 76 | Eski Deri Yelek | armor / chest (Hy,Sv,Bü) | 1 | Sıradan |  | 1 |  | 12 | armor_chest (135,100,70) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 77 | Eski Tozluk | armor / legs (Hy,Sv,Bü) | 1 | Sıradan |  | 1 |  | 8 | armor_legs (135,100,70) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 78 | Eski Deri Eldiven | armor / hands (Hy,Sv,Bü) | 1 | Sıradan |  | 1 |  | 5 | armor_hands (135,100,70) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 79 | Eski Çarık | armor / feet (Hy,Sv,Bü) | 1 | Sıradan |  | 1 |  | 5 | armor_feet (135,100,70) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 80 | Eski Sarık | armor / head (Bü,Şi,Hy) | 1 | Sıradan |  | 1 |  | 6 | armor_head (175,165,145) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 81 | Eski Cübbe | armor / chest (Bü,Şi,Hy) | 1 | Sıradan |  | 1 |  | 10 | armor_chest (175,165,145) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 82 | Eski Şalvar | armor / legs (Bü,Şi,Hy) | 1 | Sıradan |  | 1 |  | 8 | armor_legs (175,165,145) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 83 | Eski Bez Eldiven | armor / hands (Bü,Şi,Hy) | 1 | Sıradan |  | 1 |  | 4 | armor_hands (175,165,145) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 84 | Eski Pabuç | armor / feet (Bü,Şi,Hy) | 1 | Sıradan |  | 1 |  | 4 | armor_feet (175,165,145) | Kül Çömezi 0.5 %; Cüruf Yürüyen 0.4 %; Anız Yanığı 0.5 %; Yanık Nöbetçi 0.4 %; Kırağı Kayası 0.5 %; Donuk Ocakçı 0.4 % |
| 85 | Sağlam Tolga | armor / head (Sv,Şi) | 5 | Sıradan |  | 1 |  | 40 | armor_head (205,208,214) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 86 | Sağlam Göğüs Zırhı | armor / chest (Sv,Şi) | 5 | Sıradan |  | 3 | Day +1 | 70 | armor_chest (205,208,214) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 87 | Sağlam Dizlik | armor / legs (Sv,Şi) | 5 | Sıradan |  | 2 |  | 50 | armor_legs (205,208,214) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 88 | Sağlam Demir Eldiven | armor / hands (Sv,Şi) | 5 | Sıradan |  | 1 |  | 30 | armor_hands (205,208,214) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 89 | Sağlam Demir Çizme | armor / feet (Sv,Şi) | 5 | Sıradan |  | 1 |  | 30 | armor_feet (205,208,214) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 90 | Sağlam Deri Başlık | armor / head (Hy,Sv,Bü) | 5 | Sıradan |  | 1 |  | 34 | armor_head (170,124,82) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 91 | Sağlam Deri Yelek | armor / chest (Hy,Sv,Bü) | 5 | Sıradan |  | 2 | Çev +1 | 60 | armor_chest (170,124,82) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 92 | Sağlam Tozluk | armor / legs (Hy,Sv,Bü) | 5 | Sıradan |  | 1 |  | 42 | armor_legs (170,124,82) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 93 | Sağlam Deri Eldiven | armor / hands (Hy,Sv,Bü) | 5 | Sıradan |  | 1 |  | 26 | armor_hands (170,124,82) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 94 | Sağlam Çarık | armor / feet (Hy,Sv,Bü) | 5 | Sıradan |  | 1 |  | 26 | armor_feet (170,124,82) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 95 | Sağlam Sarık | armor / head (Bü,Şi,Hy) | 5 | Sıradan |  | 1 |  | 30 | armor_head (125,145,175) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 96 | Sağlam Cübbe | armor / chest (Bü,Şi,Hy) | 5 | Sıradan |  | 1 | Zek +1 | 52 | armor_chest (125,145,175) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 97 | Sağlam Şalvar | armor / legs (Bü,Şi,Hy) | 5 | Sıradan |  | 1 |  | 38 | armor_legs (125,145,175) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 %; Usta Berkay (satar); Örsbaşı Gürşad (satar) |
| 98 | Sağlam Bez Eldiven | armor / hands (Bü,Şi,Hy) | 5 | Sıradan |  | 1 |  | 22 | armor_hands (125,145,175) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 99 | Sağlam Pabuç | armor / feet (Bü,Şi,Hy) | 5 | Sıradan |  | 1 |  | 22 | armor_feet (125,145,175) | Kavruk Tırpancı 0.4 %; Buzul Körükçü 0.4 % |
| 100 | Ayazlı Tolga | armor / head (Sv,Şi) | 9 | Seyrek |  | 2 | Day +1 | 72 | armor_head (172,202,236) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 101 | Ayazlı Göğüs Zırhı | armor / chest (Sv,Şi) | 9 | Seyrek |  | 4 | Day +2 | 126 | armor_chest (172,202,236) | Harman Devi 0.3 %; Örs Kıran 0.3 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 102 | Ayazlı Dizlik | armor / legs (Sv,Şi) | 9 | Seyrek |  | 3 | Day +1 | 90 | armor_legs (172,202,236) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 103 | Ayazlı Demir Eldiven | armor / hands (Sv,Şi) | 9 | Seyrek |  | 2 | Day +1 | 54 | armor_hands (172,202,236) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 104 | Ayazlı Demir Çizme | armor / feet (Sv,Şi) | 9 | Seyrek |  | 2 | Day +1 | 54 | armor_feet (172,202,236) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 105 | Ayazlı Deri Başlık | armor / head (Hy,Sv,Bü) | 9 | Seyrek |  | 1 | Çev +1 | 61 | armor_head (145,140,160) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 106 | Ayazlı Deri Yelek | armor / chest (Hy,Sv,Bü) | 9 | Seyrek |  | 3 | Çev +2 | 107 | armor_chest (145,140,160) | Harman Devi 0.3 %; Örs Kıran 0.3 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 107 | Ayazlı Tozluk | armor / legs (Hy,Sv,Bü) | 9 | Seyrek |  | 2 | Çev +1 | 76 | armor_legs (145,140,160) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 108 | Ayazlı Deri Eldiven | armor / hands (Hy,Sv,Bü) | 9 | Seyrek |  | 1 | Çev +1 | 46 | armor_hands (145,140,160) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 109 | Ayazlı Çarık | armor / feet (Hy,Sv,Bü) | 9 | Seyrek |  | 1 | Çev +1 | 46 | armor_feet (145,140,160) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 110 | Ayazlı Sarık | armor / head (Bü,Şi,Hy) | 9 | Seyrek |  | 1 | Zek +1 | 54 | armor_head (155,180,215) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 111 | Ayazlı Cübbe | armor / chest (Bü,Şi,Hy) | 9 | Seyrek |  | 2 | Zek +2 | 94 | armor_chest (155,180,215) | Harman Devi 0.3 %; Örs Kıran 0.3 %; Anız Ağası 8 %; Körük Hanı 8 % |
| 112 | Ayazlı Şalvar | armor / legs (Bü,Şi,Hy) | 9 | Seyrek |  | 1 | İra +1 | 68 | armor_legs (155,180,215) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 113 | Ayazlı Bez Eldiven | armor / hands (Bü,Şi,Hy) | 9 | Seyrek |  | 1 | Zek +1 | 40 | armor_hands (155,180,215) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 114 | Ayazlı Pabuç | armor / feet (Bü,Şi,Hy) | 9 | Seyrek |  | 1 | İra +1 | 40 | armor_feet (155,180,215) | Harman Devi 0.3 %; Örs Kıran 0.3 % |
| 115 | Yeminli Tolga | armor / head (Sv,Şi) | 13 | Sıradan |  | 2 |  | 128 | armor_head (236,214,158) | İs Avcısı 0.3 % |
| 116 | Yeminli Göğüs Zırhı | armor / chest (Sv,Şi) | 13 | Sıradan |  | 6 | Day +2 | 224 | armor_chest (236,214,158) | İs Avcısı 0.3 %; Demirci Ilgaz (satar) |
| 117 | Yeminli Dizlik | armor / legs (Sv,Şi) | 13 | Sıradan |  | 4 | Day +1 | 160 | armor_legs (236,214,158) | İs Avcısı 0.3 %; Demirci Ilgaz (satar) |
| 118 | Yeminli Demir Eldiven | armor / hands (Sv,Şi) | 13 | Sıradan |  | 2 |  | 96 | armor_hands (236,214,158) | İs Avcısı 0.3 % |
| 119 | Yeminli Demir Çizme | armor / feet (Sv,Şi) | 13 | Sıradan |  | 2 |  | 96 | armor_feet (236,214,158) | İs Avcısı 0.3 % |
| 120 | Yeminli Deri Başlık | armor / head (Hy,Sv,Bü) | 13 | Sıradan |  | 2 |  | 109 | armor_head (180,138,86) | İs Avcısı 0.3 % |
| 121 | Yeminli Deri Yelek | armor / chest (Hy,Sv,Bü) | 13 | Sıradan |  | 4 | Çev +2 | 190 | armor_chest (180,138,86) | İs Avcısı 0.3 %; Demirci Ilgaz (satar) |
| 122 | Yeminli Tozluk | armor / legs (Hy,Sv,Bü) | 13 | Sıradan |  | 3 | Çev +1 | 136 | armor_legs (180,138,86) | İs Avcısı 0.3 %; Demirci Ilgaz (satar) |
| 123 | Yeminli Deri Eldiven | armor / hands (Hy,Sv,Bü) | 13 | Sıradan |  | 1 |  | 82 | armor_hands (180,138,86) | İs Avcısı 0.3 % |
| 124 | Yeminli Çarık | armor / feet (Hy,Sv,Bü) | 13 | Sıradan |  | 1 |  | 82 | armor_feet (180,138,86) | İs Avcısı 0.3 % |
| 125 | Yeminli Sarık | armor / head (Bü,Şi,Hy) | 13 | Sıradan |  | 1 |  | 96 | armor_head (165,112,175) | İs Avcısı 0.3 % |
| 126 | Yeminli Cübbe | armor / chest (Bü,Şi,Hy) | 13 | Sıradan |  | 3 | Zek +2 | 168 | armor_chest (165,112,175) | İs Avcısı 0.3 %; Demirci Ilgaz (satar) |
| 127 | Yeminli Şalvar | armor / legs (Bü,Şi,Hy) | 13 | Sıradan |  | 2 | İra +1 | 120 | armor_legs (165,112,175) | İs Avcısı 0.3 %; Demirci Ilgaz (satar) |
| 128 | Yeminli Bez Eldiven | armor / hands (Bü,Şi,Hy) | 13 | Sıradan |  | 1 |  | 72 | armor_hands (165,112,175) | İs Avcısı 0.3 % |
| 129 | Yeminli Pabuç | armor / feet (Bü,Şi,Hy) | 13 | Sıradan |  | 1 |  | 72 | armor_feet (165,112,175) | İs Avcısı 0.3 % |
| 130 | Korlu Tolga | armor / head (Sv,Şi) | 17 | Nadir |  | 3 | Day +2 | 224 | armor_head (242,146,96) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Ocak Efendisi 4 % |
| 131 | Korlu Göğüs Zırhı | armor / chest (Sv,Şi) | 17 | Nadir |  | 7 | Güç +2, Day +4 | 392 | armor_chest (242,146,96) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Ocak Efendisi 4 % |
| 132 | Korlu Dizlik | armor / legs (Sv,Şi) | 17 | Nadir |  | 5 | Day +3 | 280 | armor_legs (242,146,96) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Cüruf Anası 4 % |
| 133 | Korlu Demir Eldiven | armor / hands (Sv,Şi) | 17 | Nadir |  | 3 | Day +2 | 168 | armor_hands (242,146,96) | Kor Büyücüsü 0.15 % |
| 134 | Korlu Demir Çizme | armor / feet (Sv,Şi) | 17 | Nadir |  | 3 | Day +2 | 168 | armor_feet (242,146,96) | Kor Büyücüsü 0.15 % |
| 135 | Korlu Deri Başlık | armor / head (Hy,Sv,Bü) | 17 | Nadir |  | 2 | Çev +2 | 190 | armor_head (186,96,60) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Ocak Efendisi 4 % |
| 136 | Korlu Deri Yelek | armor / chest (Hy,Sv,Bü) | 17 | Nadir |  | 5 | Çev +4, Day +2 | 333 | armor_chest (186,96,60) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Ocak Efendisi 4 % |
| 137 | Korlu Tozluk | armor / legs (Hy,Sv,Bü) | 17 | Nadir |  | 4 | Çev +3 | 238 | armor_legs (186,96,60) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Cüruf Anası 4 % |
| 138 | Korlu Deri Eldiven | armor / hands (Hy,Sv,Bü) | 17 | Nadir |  | 2 | Çev +2 | 143 | armor_hands (186,96,60) | Kor Büyücüsü 0.15 % |
| 139 | Korlu Çarık | armor / feet (Hy,Sv,Bü) | 17 | Nadir |  | 2 | Çev +2 | 143 | armor_feet (186,96,60) | Kor Büyücüsü 0.15 % |
| 140 | Korlu Sarık | armor / head (Bü,Şi,Hy) | 17 | Nadir |  | 1 | Zek +2 | 168 | armor_head (205,76,66) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Ocak Efendisi 4 % |
| 141 | Korlu Cübbe | armor / chest (Bü,Şi,Hy) | 17 | Nadir |  | 3 | Zek +4, İra +2 | 294 | armor_chest (205,76,66) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Ocak Efendisi 4 % |
| 142 | Korlu Şalvar | armor / legs (Bü,Şi,Hy) | 17 | Nadir |  | 2 | İra +3 | 210 | armor_legs (205,76,66) | Cüruf Zırhlısı 0.15 %; Kor Büyücüsü 0.15 %; Cüruf Anası 4 % |
| 143 | Korlu Bez Eldiven | armor / hands (Bü,Şi,Hy) | 17 | Nadir |  | 1 | Zek +2 | 126 | armor_hands (205,76,66) | Kor Büyücüsü 0.15 % |
| 144 | Korlu Pabuç | armor / feet (Bü,Şi,Hy) | 17 | Nadir |  | 1 | İra +2 | 126 | armor_feet (205,76,66) | Kor Büyücüsü 0.15 % |
| 145 | Bekçinin Közlü Kılıcı | weapon / main_hand (Sv) | 6 | Eşsiz | 12 |  | Güç +3, Day +2 | satılamaz | sword_keep / sword_keep (255,214,140) | Kor Bekçisi 5 % |
| 146 | Kor Nöbet Asası | weapon / two_hand (Bü) | 6 | Eşsiz | 14 |  | Zek +4, İra +2 | satılamaz | staff_ember / staff_ember (255,214,140) | Kor Bekçisi 5 % |
| 147 | Közden Örme Yelek | armor / chest (Hy,Sv,Bü) | 6 | Eşsiz |  | 5 | Çev +4 | satılamaz | armor_chest (255,214,140) | Kor Bekçisi 5 % |
| 148 | Bekçi Tolgası | armor / head (Sv,Şi) | 6 | Eşsiz |  | 4 | Day +3, İra +2 | satılamaz | armor_head (255,214,140) | Kor Bekçisi 5 % |
| 149 | Ağanın Tırpan Palası | weapon / either_hand (Hy) | 10 | Eşsiz | 12 |  | Güç +2, Çev +5 | satılamaz | sword_charred / sword_soldier (255,214,140) | Anız Ağası 6 % |
| 150 | Ağanın Harman Topuzu | weapon / main_hand (Sv,Şi) | 10 | Eşsiz | 13 | 1 | Day +2, İra +5 | satılamaz | mace_slag / mace_sun (255,214,140) | Anız Ağası 6 % |
| 151 | Yanık Sancak Cübbesi | armor / chest (Bü,Şi,Hy) | 10 | Eşsiz |  | 5 | Zek +4, İra +3 | satılamaz | armor_chest (255,214,140) | Anız Ağası 6 % |
| 152 | Ağa Çizmesi | armor / feet (Sv,Şi) | 10 | Eşsiz |  | 4 | Güç +2, Day +3 | satılamaz | armor_feet (255,214,140) | Anız Ağası 6 % |
| 153 | Hanın Buz Baltası | weapon / two_hand (Sv) | 10 | Eşsiz | 24 |  | Güç +5, Day +2 | satılamaz | axe_hearth / axe_hearth (255,214,140) | Körük Hanı 6 % |
| 154 | Körük Nefesi Değneği | weapon / two_hand (Şi,Bü) | 10 | Eşsiz | 15 |  | Zek +4, İra +4 | satılamaz | staff_frost / staff_frost (255,214,140) | Körük Hanı 6 % |
| 155 | Ayaz Hançeri | weapon / either_hand (Hy) | 10 | Eşsiz | 10 |  | Çev +5 | satılamaz | dagger_thrall / dagger_hunter (255,214,140) | Körük Hanı 6 % |
| 156 | Han Eldiveni | armor / hands (Hy,Sv,Bü) | 10 | Eşsiz |  | 3 | Güç +2, Çev +3 | satılamaz | armor_hands (255,214,140) | Körük Hanı 6 % |
| 157 | Ananın Döküm Kalkanı | shield / off_hand (Sv,Şi) | 15 | Eşsiz |  | 16 | Day +6 | satılamaz | shield_warden / shield_oak (255,214,140) | Cüruf Anası 5 % |
| 158 | Cüruf Gözü Asası | weapon / two_hand (Bü) | 15 | Eşsiz | 25 |  | Zek +7 | satılamaz | staff_ember / staff_ember (255,214,140) | Cüruf Anası 5 % |
| 159 | Döküm Dizlik | armor / legs (Sv,Şi) | 15 | Eşsiz |  | 7 | Güç +3, Day +4 | satılamaz | armor_legs (255,214,140) | Cüruf Anası 5 % |
| 160 | Kurum Şalvarı | armor / legs (Bü,Şi,Hy) | 15 | Eşsiz |  | 4 | Zek +4, İra +4 | satılamaz | armor_legs (255,214,140) | Cüruf Anası 5 % |
| 161 | Ocak Yemini | weapon / main_hand (Sv) | 20 | Eşsiz | 27 |  | Güç +8, Day +4 | satılamaz | sword_keep / sword_keep (255,214,140) | Ocak Efendisi 4 % |
| 162 | Son Kıvılcım | weapon / either_hand (Hy) | 20 | Eşsiz | 18 |  | Çev +8 | satılamaz | dagger_hunter / dagger_hunter (255,214,140) | Ocak Efendisi 4 % |
| 163 | Közbeyi Asası | weapon / two_hand (Bü) | 20 | Eşsiz | 31 |  | Zek +8, İra +4 | satılamaz | staff_ember / staff_ember (255,214,140) | Ocak Efendisi 4 % |
| 164 | Kül Tacı | armor / head (Bü,Şi,Hy) | 20 | Eşsiz |  | 5 | Zek +4, İra +6 | satılamaz | armor_head (255,214,140) | Ocak Efendisi 4 % |
| 165 | Efendi Göğüs Zırhı | armor / chest (Sv,Şi) | 20 | Eşsiz |  | 12 | Güç +3, Day +6 | satılamaz | armor_chest (255,214,140) | Ocak Efendisi 4 % |

### 35.11 Canavar sanatı (sanat ajanı, 2026-11-18)

On bir yeni canavar modeli (sekiz av yuvası canavarı + üç bölge patronu), M13/fix1 yurt canavarlarıyla aynı yoldan:
hazır donatılı bir kaynak gövde, yeniden boyama (`gen_monsters.RECOLORS`), dinlenme pozunda oran değişikliği ve kemiklere
deri bağlı ek parçalar (`tools/blender/monster_shapes_m15.py`), oyuncuların 59 kemikli iskeleti, 11 klip (idle, walk, run,
punch_jab, punch_cross, spell_*, hit_chest, death). Hepsi başsız Blender'da betikle; indirme, dış model yok. Her parça
opak (yarı saydam parça yok, alfa gerekmiyor). Ayrıntılı tablo: `docs/ASSET_PIPELINE.md` §4c "M15".
`gamedata/monsters.toml`, sunucu ve bölge üretimi değiştirilmedi (sistem ajanının alanı).

| Anahtar | Ad | Bölge | Kaynak | Gövde / tepe (m) | Önerilen `scale` | Çizilen boy (m) | Halka/çarpışma yarıçapı önerisi (m, çizilmiş) |
|---|---|---|---|---|---|---|---|
| `scorched_reaper` | Kavruk Tırpancı | Tanyeri 5–7 | aydor_1 | 1,71 / 1,81 | 1,05 | 1,9 | 0,5 |
| `threshing_brute` | Harman Devi | Tanyeri 8–10 | slag_walker | 2,00 / 2,45 | 1,10 | 2,7 | 0,8 |
| `stubble_lord` | Anız Ağası (patron 10) | Tanyeri | ember_warden | 2,06 / 2,37 | 1,70 | 4,0 | 1,2 |
| `glacier_bellowsman` | Buzul Körükçü | Demirağız 5–7 | kargan_2 | 1,94 / 2,13 | 1,00 | 2,1 | 0,6 |
| `anvil_breaker` | Örs Kıran | Demirağız 8–10 | ember_warden | 2,04 | 1,20 | 2,45 | 0,8 |
| `bellows_khan` | Körük Hanı (patron 10) | Demirağız | kargan_2 | 2,01 / 2,41 | 1,70 | 4,1 | 1,2 |
| `soot_stalker` | İs Avcısı | Ortaocak 11–13 | kargan_1 | 1,80 / 1,83 | 1,15 | 2,1 | 0,7 |
| `slag_bulwark` | Cüruf Zırhlısı | Ortaocak 14–16 | aydor_2 | 1,80 / 1,90 | 1,40 | 2,65 | 0,9 |
| `ember_warlock` | Kor Büyücüsü | Ortaocak 17–19 | aydor_0 | 1,81 / 2,03 | 1,35 | 2,75 | 0,8 |
| `hearth_lord` | Ocak Efendisi (patron 20) | Ortaocak | kargan_0 | 2,01 / 2,66 | 1,60 | 4,25 | 1,3 |
| `slag_matron` | Cüruf Anası (patron 15) | Ortaocak | aydor_1 | 1,74 / 2,06 | 1,90 | 3,9 | 1,3 |

İsim etiketi ve seçim halkası istemcide kendiliğinden ölçekleniyor (etiket = mesh tepesi × `scale`, halka = 0,7 × `scale`);
ek veri gerekmiyor. Patronların ölçekle büyüyen halkası geniş eteğe / omuz parçalarına yeterli. Tepe = şapka / taç dahil.

**Doğrulama.** İçerik testleri (`MonsterTests`: on bir model ölçülen boylarıyla, ölüm klibinde zemin altı payı, baş /
leğen oranı taç ve kamburluk için düşürüldü; `ContentTests` dejenere üçgen) geçiyor. Blender'da her klip için uzanma
(`--reach`: her köşe her 3. karede, sınır 0,85 × tepe) ölçülüp tırpan, buz sırtı ve gövde ölçeği buna göre ayarlandı.
Oyun içi: geçici, yerel bir bağlantıyla (mevcut şablonların `model` / ad / ölçek / can değerleri ve `camera at:` en yakın
canavarı seçecek şekilde; hepsi commit'ten önce geri alındı) üç bölgede seviye 1 karakter her canavara yürüyüp vurdu ve
öldürdü. Ekran görüntüleri `docs/screenshots/m15/art_<anahtar>_{1_idle,1b_idle_side,2_attack,2b_attack,3_death}.png`
(1280×720, normal kamera uzaklığı, oyuncu yanında; İs Avcısı'nın yan duruşu kamerayı eve soktuğu için yok).

**Düzeltilenler (oyun içi / önizleme incelemesinden):** saman şapka kamburlukla öne düşüp çuval gibi görünüyordu (geri
eğik, açık hasır); Harman Devi'nin göğüs dikişi büyük bir ağız gibi parlıyordu (yalnız en parlak çekirdek yanıyor);
demet ve taçlardaki parlak bantlar mum gibi okunuyordu; buz ailesinin donu kamuflaj lekesi gibiydi (ince taneli, kabarık
yerlerde); Buzul Körükçü oyuncu Kargan'a benziyordu (buz mavisi ten, büyük körük); cüruf dikişleri kalın turuncu
çerçeveydi (ince, kabukla kesik); Kor Büyücüsü'nün kor çekirdeği ve kıvılcımları uzaktan görünmüyordu (büyütüldü).

**Hâlâ zayıf**: Buzul Körükçü Demirağız'ın loş ışığında hâlâ koyu; körük önden zor seçiliyor (arkadan ve yandan belli).
Kavruk Tırpancı'nın tırpanı saldırı anında gövdenin arkasında kalabiliyor. Kor Büyücüsü uzaktan koyu bir cüppe; parlak
olan yalnız etek ucu ve göğüs. Saldırı kareleri vuruş anına denk getirildi ama her karede yumruk tam açık değil (aynı
punch klipleri; canavara özgü saldırı klibi yok). Ölü yatarken sırttaki parçalar (körük, taç, demetler) zemine 0,3–0,8 m
giriyor (test payı her model için yazıldı; arazi gizliyor). Kaynak gövdeler oyuncu / eski canavar gövdeleri: İs Avcısı ile
Kor Büyücüsü aynı bölgede değil ama Körük Hanı ve Buzul Körükçü aynı gövde (aile bilerek: biri dik, taçlı, 1,7×).

**Benzersiz eşya önerisi (yalnız öneri, veri değişmedi):** mevcut silah modelleri yeni dokuyla:
Anız Ağası → `axe_hearth` kömürleşmiş sap + altın kor çatlakları ("Anız Orağı" benzeri), `shield_oak` saman örgülü kenar;
Körük Hanı → `mace_sun` buzlu demir baş + soğuk mavi ışıma, `staff_frost` beyaz kırağı; Cüruf Anası → `sword_keep` cüruf
kını, turuncu dikiş ışıması, `dagger_hunter` obsidyen ağız; Ocak Efendisi → `staff_ember` ızgara başlı, kor çekirdekli,
`sword_soldier` kızgın demir ağız. M10'daki gibi (`gen_loot.make_variant`) önce simge, sonra gerekirse dünya mesh'i.

## 36. M16 "Tav" — eşya yükseltme (2026-11-19)

Sözleşme: `docs/PROTOCOL_UPGRADE.md` (protokol **v10**; istemci sunumu §7, bütün Türkçe metinler orada). Mekanik
klasik MMORPG yükseltmesi gibi (taş + ücret, şans, +6'dan itibaren yok olma); adlar ("Tav", Ham / Saf / Usta Tav Taşı),
metinler, simgeler ve sayılar özgün. Oyunun ilk gerçek Akçe çıkışı. Ekran görüntüleri: `docs/screenshots/m16/`.

### 36.1 Kararlar

| Konu | Karar |
|---|---|
| Ne yükselir | Her silah, kalkan ve zırh parçası (Eşsizler dahil), +0…+10, **eşya örneği başına** (`character_items.upgrade`, göç 0009). Yığılanlar / malzemeler / iksirler yükselmez (`NotUpgradable`) |
| Nerede | Üç demirci (`npcs.toml` `upgrades = true`: Demirci Ilgaz, Usta Berkay, Örsbaşı Gürşad); dükkânı açık olmalı (talk menzili 4 m + 0,5) |
| Çanta mı, kuşanılan mı | **Yalnız çanta** (istek bir çanta yuvası adlandırır): görünen donanım ve statlar dövüş ortasında bir denemeyle değişmez, yok olan eşya bir kuşanma yuvasından gitmez (ENTITY_EQUIP yok). İstemci: "Önce çıkarmalısın: yalnız çantadaki eşya yükseltilir." |
| Taş | Kademe 1–2 (gereken seviye 1–8) **Ham Tav Taşı**; 3–4 (9–16) **Saf Tav Taşı**; 5 (17–20) ve **her Eşsiz** **Usta Tav Taşı**. Kademe = `min(5, (seviye − 1) / 4 + 1)` |
| Şans | +1…+3 %100, +4 %80, +5 %60, +6 %45, +7 %30, +8 %15, +9 %8, +10 %3 (görevdeki tablo, değiştirilmedi) |
| Başarısızlık | Taş ve ücret her denemede gider. +4 / +5: eşya olduğu gibi kalır. +6 ve üstü: eşya **yok olur**; istemci önce sorar (Yükselt / Vazgeç) |
| Stat | Yalnız saldırı / savunma; aşağıdaki tablo (36.2) |
| Satış | Tüccar yükseltilmiş eşyaya düz fiyatı (`value / 2`) öder: yükseltmeden Akçe çıkmaz; Eşsizler satılamaz kalır; istemci yükseltilmiş eşyayı satmadan önce sorar |
| Çift tıklama / bayat istek | İstek `item_id` ve `from_level` taşır: başka bir eşya (`NoItem`) ya da bilinen seviyesinden farklı bir eşya (`Changed`) için hiçbir şey denenmez — yanlışlıkla iki tık bir eşyayı +5'ten +7 denemesine götüremez |
| Zar | Sunucunun (oturum başına `std::mt19937`, sistemin rastgele aygıtından tohumlu); 0..999, şans permil altındaysa başarı |
| Sel sınırı | UPGRADE_REQ saniyede 5 (fazlası `ERROR(Flood)`); istemci en çok 4 gönderir ("Çok hızlı: biraz bekle") |
| Günlük | Her deneme için bir INFO satırı: karakter, eşya, `+a -> +b`, sonuç, şans, zar, ücret, taş, kalan bakiye |
| Test kancaları | `world.testing_upgrade_outcome` ("success" / "fail" = +4'ten itibaren / "fail_from_<4..10>"), `world.testing_enter_coins` (dünyaya giren karakter en az bu kadar Akçe ile, kaydedilir); ikisi de varsayılan kapalı, açıkken günlüğe uyarı |

### 36.2 Sayılar

**Taşlar** (`items.toml` 166–168, `upgrade_stone = 1|2|3`; yığın 50; simgeler `tools/blender/gen_upgrade_icons.py`,
başsız Blender): Ham Tav Taşı 25 Akçe (kaba, gri-kahve bileği taşı), Saf Tav Taşı 110 (soluk, altıgen, mavi damarlı),
Usta Tav Taşı 320 (koyu, kor çatlaklı). Üç demircide satılıyor (stok sonu; stok sınırı 24 → 32). Düşmeler (`loot.toml`
her tablonun sonu; tablo sınırı 24 → 32 satır): Ham %1–3 seviye 1–10, Kor Bekçisi %30 ×1–2, Anız Ağası / Körük Hanı %40
×1–3; Saf %0,8–2,5 seviye 8–19, seviye 10 bossları %15, Cüruf Anası %40 ×1–2; Usta Cüruf Zırhlısı %0,3, Kor Büyücüsü %1,
Cüruf Anası %10, Ocak Efendisi %50 ×1–2.

**Ücret** = kademe tabanı × hedef çarpanı (aşağı yuvarlanır). Taban: kademe 1: 6, 2: 20, 3: 55, 4: 110, 5: 190
(Eşsizler seviyelerinin kademesini kullanır).

| Hedef | +1 | +2 | +3 | +4 | +5 | +6 | +7 | +8 | +9 | +10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Çarpan | ×1 | ×1,5 | ×2 | ×3 | ×4 | ×6 | ×8 | ×11 | ×15 | ×20 |
| Kademe 1 | 6 | 9 | 12 | 18 | 24 | 36 | 48 | 66 | 90 | 120 |
| Kademe 2 | 20 | 30 | 40 | 60 | 80 | 120 | 160 | 220 | 300 | 400 |
| Kademe 3 | 55 | 82 | 110 | 165 | 220 | 330 | 440 | 605 | 825 | 1 100 |
| Kademe 4 | 110 | 165 | 220 | 330 | 440 | 660 | 880 | 1 210 | 1 650 | 2 200 |
| Kademe 5 | 190 | 285 | 380 | 570 | 760 | 1 140 | 1 520 | 2 090 | 2 850 | 3 800 |

**Stat** — seviye L'nin eklediği = `yuvarla(taban × p(L) / 100)` (yarımlar yukarı); taban 8 ve üstü (silahlar,
kalkanlar) için +6'dan itibaren her seviye bir öncekinden en az 1 fazla ekler (riskli bir deneme sayıyı hiç değiştirmeden
geçmesin). Zırh parçaları (1–7) düz yüzdeyi kullanır.

| Seviye | +1 | +2 | +3 | +4 | +5 | +6 | +7 | +8 | +9 | +10 |
|---|---|---|---|---|---|---|---|---|---|---|
| p(L), tabanın %'si | 5 | 10 | 15 | 20 | 25 | 31 | 38 | 46 | 55 | 65 |
| Eşik Kılıcı (16) | 17 | 18 | 18 | 19 | 20 | 21 | 22 | 23 | 25 | 26 |
| Kor Kılıcı (22) | 23 | 24 | 25 | 26 | 28 | 29 | 30 | 32 | 34 | 36 |
| Ocak Baltası (10) | 11 | 11 | 12 | 12 | 13 | 14 | 15 | 16 | 17 | 18 |
| Korlu Göğüs Zırhı (7) | 7 | 8 | 8 | 8 | 9 | 9 | 10 | 10 | 11 | 12 |

Ayar hedefi "+7'deki kademe-N eşya ≈ +0'daki kademe-N+1 eşya" (`UpgradeTests`, `VO_ECONOMY_TABLE=1` tabloyu basar;
her silah tutuşu / kalkan / gövde zırhı hattı için kademenin en güçlü sıradan / seyrek / nadir eşyası, taban < 6 olan
küçük zırh parçaları hariç):

| Hat | Kademe N (+0 → +7) | Kademe N+1 (+0) | +7 / sonraki |
|---|---|---|---|
| Kılıç | Burç Kılıcı 9 → 13 / Harman 13 → 18 / Eşik 16 → 22 / İs 19 → 26 | Harman 13 / Eşik 16 / İs 19 / Kor 22 | 1,00 / 1,12 / 1,16 / 1,18 |
| Topuz | Cüruf Topuzu 7 → 10 / Örs 11 → 16 / Eşik 14 → 20 / Cüruf Gürzü 16 → 22 | Örs 11 / Eşik 14 / Cüruf Gürzü 16 / Kor 19 | 0,91 / 1,14 / 1,25 / 1,16 |
| Pala | Tırpan Palası 9 → 13 / Eşik 12 → 17 / İs 14 → 20 | Eşik 12 / İs 14 / Kor 16 | 1,08 / 1,21 / 1,25 |
| Hançer | Çömez Bıçağı 5 → 7 / Kırağı 8 → 12 / Eşik 10 → 15 / İs 12 → 17 | Kırağı 8 / Eşik 10 / İs 12 / Kor 14 | 0,88 / 1,20 / 1,25 / 1,21 |
| Asa | Kırağı 10 → 15 / Buzul 15 → 21 / Eşik 18 → 25 / Kurum 21 → 29 | Buzul 15 / Eşik 18 / Kurum 21 / Kor 25 | 1,00 / 1,17 / 1,19 / 1,16 |
| İki el balta | Ocak Baltası 10 → 15 / Harman 21 → 29 / Eşik 26 → 36 / Cüruf 30 → 41 | Harman 21 / Eşik 26 / Cüruf 30 / Kor 35 | 0,71 / 1,12 / 1,20 / 1,17 |
| Kalkan | Meşe 4 → 6 / Örs 8 → 12 / Eşik 10 → 15 / Cüruf Siperi 12 → 17 | Örs 8 / Eşik 10 / Cüruf Siperi 12 / Kor 14 | 0,75 / 1,20 / 1,25 / 1,21 |
| Gövde zırhı | Ayazlı 4 → 6 / Yeminli 6 → 8 | Yeminli 6 / Korlu 7 | 1,00 / 1,14 |

Kademe 2–5: 1,00–1,25 (test sınırı 0,85–1,25); kademe 1 (başlangıç takımı düzeyi; bir sonraki kademe çok daha güçlü):
0,71–1,00 (sınır 0,60). Yani +7 bir kademe atlatır, biraz da öne geçirir; +10 (%65) bir buçuk kademe kadar.

### 36.3 Ekonomi

**Beklenen maliyet** (`UpgradeTests` "[economy]"; ücret + taş satın alma fiyatıyla; +6'dan itibaren her yok oluş yeni bir
+(k−1) eşya demek: `V_k = (V_(k−1) + c_k) / p_k`, +5'e kadar `V_k = V_(k−1) + c_k / p_k`; eşyanın kendisi kademenin
ortanca değeriyle):

| Kademe | Taş | Eşya değeri | +5'e | +7'ye | +10'a |
|---|---|---|---|---|---|
| 1 | Ham | 10 | 237 | 2 518 | ~7,3 milyon |
| 2 | Ham | 110 | 446 | 5 701 | ~17 milyon |
| 3 | Saf | 90 | 1 471 | 16 564 | ~49 milyon |
| 4 | Saf | 160 | 2 292 | 27 004 | ~80 milyon |
| 5 | Usta | 280 | 4 728 | 53 761 | ~158 milyon |

(+10 tablodaki %3 şansla pratikte erişilmez: +5'ten +10'a yok olmadan çıkma olasılığı ~1/20 000. Görevin şans tablosu
değiştirilmedi; +8–+10 "şanslıların ve zenginlerin" seviyesi.)

**Gelire karşı** (§35.5: Sv. 12 ~4 200–5 600, Sv. 16 ~6 400–9 300, Sv. 20 ~8 000–11 000 Akçe / saat; 1→20 toplam 35–38 bin):
- Sv. 1–4 (~440 Akçe toplam): bir silahı +3'e çıkarmak ~100 Akçe (3 Ham + 27 ücret), +5 ~240 — erken gelirin yarısı.
- Sv. 5–8 (kademe 2, gelir ~2–3 bin): silah + gövde +5 ~900 Akçe (%30–45).
- Sv. 9–16 (kademe 3–4, gelir ~10–15 bin): silah ve gövde +5 (~3–4,6 bin) ve geri kalan beş parça +3 (~3–4 bin) gelirin
  %50–70'i.
- Sv. 20 (kademe 5, Eşsizler Usta taşıyla): yedi parçalık takımı +5'e çıkarmak ~33 bin (3–4 saatlik gelir); bir silahı +7'ye
  çıkarmak ortalama ~54 bin (5–7 saat). **Düzenli yükselten bir oyuncu Sv. 20'de saatlik gelirinin tamamını harcar**; 1→20
  boyunca kazandığının ~%50–70'ini.
- Saatte harcanan (Sv. 20, savaşçı, ~9 600 / saat gelir, ~150 öldürme / saat): kademe 5 takımı +5'e götüren oyuncu saatte
  ~2 parça +5 (~9 500) — gelirin ~%100'ü. Taş düşmeleri (Usta %1 × ~150 = ~1,5 / saat ≈ 480 Akçe değeri, Saf ~165) harcamanın
  ~%7'sini karşılıyor.

Satış koruması: yükseltilmiş eşya düz fiyatla satılır (`UpgradeTests`: her yükseltilebilir eşya için satış ≤ değer / 2,
Eşsizler 0). Taşlar yarı fiyata geri satılır (alınandan pahalıya satılamaz).

### 36.4 Protokol, sunucu, veritabanı

| Konu | Ne |
|---|---|
| Protokol v10 | `UPGRADE_REQ` 0x0A00 (seq, npc, bag_slot, item_id, from_level, stone_slot; 19 bayt), `UPGRADE_RES` 0x0A01 (seq, result, bag_slot, template, level, stone, fee; 15 bayt), 15 sonuç kodu; `ItemEntry` + `u8 upgrade` (15 bayt); `SHOP_OPEN_RES` 32 kalem |
| Paylaşılan kod | `shared/game/Upgrade.h`: tablolar, kademe, taş, `planUpgrade` (kontrol sırası §2.2), `upgradeOutcome`, sonrası; `Items.h`: `ItemStack/StoredItem::upgrade`, `upgrade_stone` anahtarı; `Npcs.h`: `upgrades`; `Combat.cpp` `equipmentBonus` yükseltilmiş değerleri sayıyor |
| Dünya | `ShopCommand::Upgrade` (`ZoneShop.cpp`): NotOpen → NotSmith → YouAreDead → OutOfRange, kabul → `ShopGrant::Upgrade` (sayaç `upgradeGrants`, kapanış satırında "upgrade attempts") |
| Oturum | `handleUpgrade` (sel 5/s), eşya kuyruğunda `startUpgrade`: planla, zar (ya da kanca), tek `applyItemWrite` (taş miktarı, `coinsRemoved`, `ItemWrite::upgrades` — seviye ya da silme), commit'ten sonra ITEM_UPDATE + COINS + UPGRADE_RES, günlük satırı; `testing_fail_first_item_write` yükseltmeye de uygulanıyor (bit 8) |
| Veritabanı | `0009_item_upgrades.sql`: `upgrade smallint NOT NULL DEFAULT 0`, CHECK 0..10 ve "yükseltilmiş satır tek birim"; Postgres yazımı `WHERE upgrade = before` (araya giren `set-upgrade` / `set-coins` yazımı geri çevirir). Gerçek `vedatonline` veritabanına **uygulanmadı** (müdürün işi) |
| vo_admin | `list-items` "upg" sütunu ve "Ad (+N)", `show-character` "gear" bölümü (yer, ad, +N/10, saldırı / savunma), `set-upgrade <karakter> <eşya id | bag/N | equip/...> <0..10>` (test) |

### 36.5 İstemci

| Konu | Ne |
|---|---|
| Sekmeler | Demircinin penceresinde selamlamanın altında "Satın al" / "Yükselt" (öteki satıcılarda yok); "Yükselt" stok ve satış alanının yerine bölümü açar, pencere boyu ona göre |
| Bölüm (`UpgradePanel`) | eşya kuyusu (çantada sağ tık / pencereye sürükleme koyar, kuyuya sağ tık boşaltır), ad (renkli) + "+4 → +5" + "Saldırı 18 → 19", "Taş:" (simge, ad, "Elinde: n" — 0 kırmızı), "Ücret:" (Akçe sikkesi, yetmezse kırmızı), "Başarı şansı: %45" (yeşil / altın / kırmızı), başarısızlık satırı (yok olacaksa kırmızı, kalın, kırmızı çerçeve), "Yükselt" düğmesi (yerel bir kontrol tutmazsa kapalı, nedeni kırmızı), sonuç satırı |
| Soru | +6 ve üstü: "Dikkat: <Ad (+N)>" — şans, taş, ücret ve "YOK OLUR"; Yükselt / Vazgeç (Esc = Vazgeç) |
| Efekt | başarı: büyüyen altın halka + ışınlar + parlayan kuyu; başarısızlık: kuyu sarsılır, grileşir; yok oluş: kırmızı parlama, simge iki parçaya ayrılıp düşer, çatlaklar (~1,8 s); sonuç satırı 8 s |
| "(+N)" her yerde | ipucu başlığı "Ad (+N)", "Saldırı +20 (+4 tav)", "Yükseltme: +N / +10"; çanta / kuşanma simgelerinde sol üstte "+N" rozeti; satış sorusu ve satış satırı; +7'den itibaren ad **leylak `#C98BFF`** (Eşsizler amber kalır) |
| Otomasyon | `shoptab`, `upgradepick`, `upgradeclear`, `upgradestart`, `upgradeforce`, `expectupgrade`, `expectupgradepanel`, `upgradelog`; durumlar `shoptabs`, `upgradetab`, `upgradeitem`, `upgradeready`, `upgradeconfirm`, `upgradebusy/idle`, sayaçlar `upgradeokN` … |

### 36.6 Botlar ve uçtan uca

botclient `upgrade` senaryosu (`RunScenario.ps1 -Scenario upgrade`; `integration.upgrade` ve `integration.db.upgrade`):
üç sunucu koşusu, her biri ayrı karakterle (`testing_enter_coins = 5000`):
- **"success"**: NotOpen (dükkân yok), NotSmith (şerbetçi), NoStone, WrongStone (kademe 1 baltaya Saf), NotUpgradable (taşın
  kendisi), NoItem ×2, Changed; Ham taşları alınıp balta +0 → +10 (her seviyede ITEM_UPDATE / COINS / UPGRADE_RES sırası,
  ücret, taş sayısı), MaxLevel; kılıç çıkarılıp +5; kalan Akçe taşa harcanıp +6 için NotEnoughCoins; +10 balta kuşanılıp
  yeniden giriş (seviyeler duruyor, ENTER_WORLD_RES saldırısı `deriveStats(equipmentBonus)` ile aynı); 6 UPGRADE_REQ'de
  ERROR(Flood).
- **"fail"**: +1…+3, sonra iki kez +4 başarısız — balta +3 kalır, taşlar ve ücretler gider, yeniden girişte +3.
- **"fail_from_6"**: +1…+5, +6 balta yok olur (yuva boşalır), yeniden girişte yok.
- `-Database` ile ayrıca `vo_admin show-character` (gear "(+10)", "+5/10"), `list-items` ("+10" sütunu), `set-upgrade <ad>
  bag/<yuva> 7` ve bir sonraki girişte kılıç +7.

### 36.7 Testler, akışlar, ekran görüntüleri

Yeni birim testleri: `tests/game/UpgradeTests.cpp` (tablolar, adım kuralı, taşlar ve kademeler, +7 ≈ sonraki kademe,
`planUpgrade`'in bütün retleri ve sırası, zar sonuçları, envanterde seviye, ekipman bonusu ve satış fiyatı, `upgrade_stone` /
`upgrades` doğrulaması, taş düşmeleri, v10 mesajları, ekonomi tablosu), `tests/server/UpgradeServerTests.cpp` (süreç içi
sunucu: retler, başarı yazımı cevaptan önce, başarısız yazım hiçbir şeyi değiştirmez, kancayla Failed / Destroyed, +7 kılıcın
saldırıya etkisi, MaxLevel, sel), `tests/db/PostgresUpgradeTests.cpp` (0009 bir M15 veritabanına, CHECK'ler, atomik deneme
yazımı — yanlış seviye / yetmeyen Akçe hiçbir şey değiştirmez —, yok etme, `setItemUpgrade`, gerçek sunucuyla saklama ve
yeniden giriş), `tests/client/UpgradeViewTests.cpp` (adlar / renkler / ipucu, bölüm görünümü, metinler, oturum, ayrıştırıcı),
`ShopZoneTests` (dünyanın dört kontrolü ve grant), fuzz'da v10 istekleri.

Değişen eski testler: `MessagesTests` (sürüm 10, ItemEntry 15 bayt), `ShopTests` (stok sınırı 32), `LootTests` (168 şablon,
Kül Çömezi 21 satır, tablo sınırı 32), `HuntingTests` (168 şablon), `PostgresShopTests` / `PostgresSkillsTests` /
`PostgresStatsTests` (uygulanan göçlere 9 eklendi).

UI akışları (`run-ui-flows.ps1`, veritabanısız, kancalar `EnterCoins` / `UpgradeOutcome`): `m16_upgrade` (1280x720,
1024x640 @150 %: sekmeler, taş alma, boş bölüm, bölümdeki balta, başarı efekti, +5 uyarısı ve ipucu, +6 sorusu, +7 leylak ad,
+10, taş "yükseltilemez", Saf taşsız Eşik Kılıcı, şerbetçide NotSmith, uzaklaşınca kapanma), `m16_keep` (1280x720,
1920x1080: +5 iki kez başarısız, eşya +4 kalır), `m16_destroy` (1280x720, 1920x1080, 1024x640 @150 %: +5 uyarısı, soru,
Vazgeç, yok oluş efekti ve sonrası). Ekran görüntüleri `docs/screenshots/m16/` (`<akış>_<çözünürlük>_<ad>.png`).

**Son tam koşular** (dalın son kodu, 04282bc; ondan sonra yalnız belgeler): Debug `.\build.ps1 -Test` **697/697** ctest
(16 `integration.db.*` atlandı: worktree'de `config/server.local.toml` yok; ~32 dk). Tam `run-ui-flows.ps1`: **112/113**
koşu; düşen `loot/1280x720` (seviye 1 savaşçı ilk Kül Çömezi'ne öldü — §35.8'de bilinen seviye 1 dövüş oynaklığı, M16'yla
ilgisiz) ayrıca yeniden koşturuldu: 2/2. Bir önceki tam koşuda (aynı kod, yalnız bir yorum farkı) 111/113, düşen
`two_clients_shop` (içme hareketi zamanlaması) yeniden 2/2. Release `.\build.ps1 -Config Release -Test`: derleme temiz,
**696/697** — düşen `integration.skills` (seviye 1 bot 4 ölümde seviye atlayamadı; aynı oynaklık) Release ikilileriyle yeniden
koşturulup geçti. Elle, ana kopyanın test bağlantısıyla (`vedatonline_test`, kendi `vo_it_*` şeması):
`RunScenario.ps1 -Scenario upgrade -Database` geçti (show-character / list-items / set-upgrade dahil). `Postgres*` birim
testleri (`PostgresUpgradeTests` dahil) `vedatonline_test`'e karşı koştu. Geliştirmede düşen ve düzeltilenler: sel sınırına
takılan hızlı tıklama (istemci bütçesi), ilk tablo (%3 adım) ile +1'in hiç artırmaması (%5 adım + +6'dan itibaren +1 tabanı),
botun yeniden girişte kancanın doldurduğu bakiyeyi beklememesi, sunucu günlüğü tamponlu olduğu için satır sayan kontroller,
eski testlerin M16 verisine (şablon sayısı, ganimet satırları, stok sınırı, uygulanan göçler, protokol sürümü).

### 36.8 Bilinen eksikler / zayıf noktalar

- **Küçük zırh parçaları** (savunma 1–3) düz yüzdeyle bazı seviyelerde hiç artmıyor (eldiven 1 → +9'a kadar 1); bölüm
  "Savunma 2 → 2" diye dürüstçe gösteriyor. Silah / kalkanlarda +1…+5 arasında da tek tük duraklama olabilir (taban 10: +1 ve
  +2 ikisi 11); +6'dan itibaren (taban 8+) her seviye artıyor.
- **+8…+10** görev tablosuyla (%15 / %8 / %3 ve yok olma) pratikte erişilmez (beklenen maliyet milyonlar); bilinçli bırakıldı.
- Kuşanılmış eşya yükseltilemez (tasarım kararı): oyuncu önce çıkarmalı (istemci söylüyor).
- Başkaları yükseltme seviyesini görmez (`ENTITY_SPAWN` / `ENTITY_EQUIP` yalnız şablon taşır); silah modelinde parıltı
  yok.
- Efektler 2B (pencere içinde); demircide çekiç / kıvılcım animasyonu yok (yalnız mevcut `interact` klibi).
- Taş simgeleri basit prosedürel modeller; Ham taşı biraz "çakıl" gibi okunuyor.
- Yükseltme sonucu yalnız kendi oyuncuya; sunucu genel duyuru yapmıyor (+10 gibi nadir anlar için öneri).
- Sunucunun deneme günlüğü INFO düzeyinde ve stdout tamponlu: süreç zorla öldürülürse son satırlar kaybolabilir
  (uçtan uca test bu yüzden yalnız ilk satırı arıyor).
- `world.testing_enter_coins` her girişte bakiyeyi tekrar doldurur (yalnız test).
- Gerçek bir 1→20 oyunuyla denenmedi; ekonomi hesabı §35.5'in modeline ve beklenen değere dayanıyor.

## 37. M17 "Görevler" — görevler ve ortak köy Eşik (2026-11-19)

Sözleşme: `docs/PROTOCOL_QUESTS.md` (protokol **v11**; istemci sunumu §7, bütün Türkçe metinler orada). Mekanik
klasik iki halklı MMORPG gibi (iki ırkın ortak kasabasında görev verenler, öldür / topla / konuş / boss hedefleri,
zincirler, bekleme süreli tekrarlanabilir görevler, yükseltmenin tek merkezi); bütün adlar, NPC'ler, hikâyeler ve
metinler özgün (`docs/LORE.md`: Ant Taşı, Közaranlar, Külden Doğanlar). Ekran görüntüleri: `docs/screenshots/m17/`.

### 37.1 Kararlar

| Konu | Karar |
|---|---|
| Veri | `gamedata/quests.toml` (sıkı doğrulama: bilinmeyen anahtar, kimlik 1..999, metin boyları, `after` dosyada önce gelen bir görev, tekrarlanabilir = bekleme 60 s..7 gün, 1-3 hedef, sayılar 1..50, konuşma hedefi = teslim NPC'si, toplama = yığılan eşya, boss = elit şablon, ödül sınırları, sınıf eşyası o sınıfa uygun, seçme 2-4 eşya; NPC / canavar / eşya kataloglarıyla çapraz); sunucu geçersiz dosyayla açılmaz (çıkış 6) |
| Hedef türleri | `kill` (şablon listesi), `boss` (elit), `collect` (çantada sayılır, teslimde alınır), `talk` (görev o NPC'ye teslim edilir) |
| Önkoşul | seviye, ırk (isteğe bağlı), önceki görev (`after`) |
| Ödül | XP, Akçe, herkese eşya, **sınıfa göre eşya** (`class_items`: "sınıfıma uyan parça"), **seçmeli eşya** (`choice`), yükseltme taşları |
| Durumlar | alınabilir / etkin (sayaçlar) / teslim edilebilir / bitti — saklanan yalnız etkin ve bitti (`character_quests`); "alınabilir" ve "hazır" veriden, satırdan ve çantadan çıkar |
| En çok etkin görev | **10** (`[rules] max_active`; test kancası `world.testing_quest_max_active`) |
| Öldürme payı | ganimetin sahibi (deneyimi kazanan, yoksa son vuruş): `Zone::killMonster` -> `WorldBatch::kills` -> oturum |
| Toplama eşyaları | **normal ganimetten** (Kül Tozu, Cüruf Parçası, Kavruk Başak, Körük Derisi); göreve özel düşme yok (yeni şablon gerekmedi) |
| Kayıt | kabul / bırakma / teslim hemen ve cevaptan önce; öldürme sayaçları dünyanın kayıt aralığıyla (60 s) ve dünyadan çıkışta (çıkış, karakter seçimi, kopma, sunucu kapanışı) |
| Teslim | **tek işlem**: görev satırı, alınan birimler, ödül eşyaları, Akçe ve XP (saklanan seviye / xp `addExperience` ile); sonra dünya aynı XP'yi canlı karaktere ekler (XP_GAIN) ve hemen kaydeder |
| Doğrulama | NPC görüşte, oyuncu canlı, konuşma menzili 4 m + 0,5 (dünya); veren / teslim alan NPC, önkoşullar, çanta yeri (dolu çanta: "Çantan dolu: ödül sığmıyor"), kese sınırı (oturum, işlenmiş duruma karşı); sel sınırı saniyede 5 (istemci en çok 4, bir seferde bir istek) |
| Yükseltme merkezi | yalnız **Demirci Ilgaz** (Eşik) yükseltir; yurt demircileri taş satmaya devam eder, pencerelerinde "Yükselt" sekmesi yok, yerine "Tav işi Eşik'te yapılır: Demirci Ilgaz'a git." (`NpcDef::sendsToSmith`); sunucu `NotSmith` |
| NPC'ler | `kind = "quest"` (stoksuz); mevcut NPC modelleri `tint` / `scale` ile ödünç (iki NPC aynı modeli ancak farklı renk / boyla paylaşır) |

### 37.2 Yeni NPC'ler (`npcs.toml` 10-14)

| Id | Ad | Unvan | Yer | Model (renk, ölçek) |
|---|---|---|---|---|
| 10 | Bilge Oruç | Ant Taşı Bekçisi | Ortaocak (252, 274), Ant Taşı'nın kuzeyi | npc_smith_aydor (kül grisi, 0,96) |
| 11 | Avcı Selvi | Közaran Gözcüsü | Ortaocak (273, 264), meydanın doğusu | npc_potion_aydor (yosun yeşili, 1,02) |
| 12 | Karakol Başı Toygar | Ortaocak Muhafızı | Ortaocak (260, 232), meydanın güneyi | npc_smith_kargan (pas kırmızısı, 1,05) |
| 13 | Sancaktar Ece | Burç Sancaktarı | Tanyeri (251,5, 276,5) | npc_potion_aydor (saman altını, 1,0) |
| 14 | Közbaşı Barsan | Demirağız Ocakbaşı | Demirağız (267, 327) | npc_potion_kargan (soğuk demir mavisi, 1,08) |

Yerler sunucunun açılış kurallarını (müşteri yeri, yol katmanı, işaretlerden uzaklık) geçen açık zemin noktaları;
tüccarlardan Demirci Ilgaz (görev 4'ü alır, 5'i verir) ve Sürmeli Bacı (görev 7'yi alır) da göreve katılıyor.

### 37.3 Görev listesi

| Id | Ad | Veren → teslim | Sv. | Önce | Hedef | XP | Akçe | Eşya |
|---|---|---|---|---|---|---|---|---|
| 1 | Çatlak Taşın Konuğu | Oruç → Selvi | 1 | – | Selvi ile konuş | 30 | 10 | – |
| 2 | Külden Kalkanlar | Selvi | 1 | 1 | 6 Kül Çömezi | 70 | 20 | sınıfa göre Eski Dizlik / Tozluk / Şalvar |
| 3 | Kül Tozu | Selvi | 1 | 1 | 5 Kül Tozu topla | 60 | 15 | 3 Kızılca Şerbet |
| 4 | Ham Taşın Sırrı | Oruç → Ilgaz | 2 | 2 | Ilgaz ile konuş | 50 | 15 | **2 Ham Tav Taşı** (ilk yükseltme; +1/+2 başarısız olmaz) |
| 5 | Örse Cüruf | Ilgaz | 3 | 4 | 4 Cüruf Parçası topla | 110 | 25 | 2 Ham Tav Taşı |
| 6 | Cüruf Yatağı | Toygar | 3 | 2 | 8 Cüruf Yürüyen | 160 | 35 | sınıfa göre Burç Kılıcı / Çömez Bıçağı / Kırağı Asası / Cüruf Topuzu |
| 7 | Şerbetçiye Haber | Toygar → Sürmeli Bacı | 4 | 6 | Bacı ile konuş | 90 | – | 3 Közlü Şerbet, 2 Ayaz Özü |
| 8 | Bekçinin Yolu | Toygar | 4 | 6 | 12 Cüruf Yürüyen | 220 | 45 | seçmeli: Eski Göğüs Zırhı / Eski Deri Yelek / Eski Cübbe |
| 9 | Kor Bekçisi | Oruç | 5 | 8 | boss Kor Bekçisi | 350 | 100 | 2 Ham Tav Taşı + sınıfa göre Sağlam Göğüs / Deri Yelek / Cübbe |
| 10 | Burca Dönüş (Aydor) | Oruç → Ece | 6 | 9 | Ece ile konuş (Tanyeri) | 120 | 30 | – |
| 11 | Ocağa Dönüş (Kargan) | Oruç → Barsan | 6 | 9 | Barsan ile konuş (Demirağız) | 120 | 30 | – |
| 21 | Kavruk Ekin | Ece | 6 | 10 | 10 Kavruk Tırpancı | 380 | 70 | – |
| 22 | Başak Demeti | Ece | 6 | 21 | 8 Kavruk Başak topla | 300 | 50 | sınıfa göre Sınır Kılıcı / Hançeri / Kıvılcım Asası / Sınır Topuzu |
| 23 | Harman Devleri | Ece | 8 | 22 | 8 Harman Devi | 600 | 120 | – |
| 24 | Ağanın Tırpanı | Ece | 9 | 23 | boss Anız Ağası | 700 | 200 | 2 Saf Tav Taşı |
| 31 | Körük Çukuru | Barsan | 6 | 11 | 10 Buzul Körükçü | 380 | 70 | – |
| 32 | Körük Derisi | Barsan | 6 | 31 | 8 Körük Derisi topla | 300 | 50 | sınıfa göre (22 gibi) |
| 33 | Örs Kıranlar | Barsan | 8 | 32 | 8 Örs Kıran | 600 | 120 | – |
| 34 | Hanın Ocağı | Barsan | 9 | 33 | boss Körük Hanı | 700 | 200 | 2 Saf Tav Taşı |
| 41 | Kül Nöbeti (tekrar, 30 dk) | Toygar | 2 | 2 | 10 Kül Çömezi | 50 | 20 | – |
| 42 | Burç Erzakı (tekrar, 60 dk, Aydor) | Ece | 7 | 22 | 6 Kavruk Başak topla | 200 | 45 | – |
| 43 | Ocak Körüğü (tekrar, 60 dk, Kargan) | Barsan | 7 | 32 | 6 Körük Derisi topla | 200 | 45 | – |

Bir karakterin yolu: Eşik'te 10 görev (1-9 ve ırkının 10 / 11'i), yurdunda 4 görev = **14 tek seferlik görev** + 2
tekrarlanabilir (41 ve ırkının 42 / 43'ü).

### 37.4 Ekonomi (`QuestTests` "[economy]", `VO_ECONOMY_TABLE=1`)

| | XP | Akçe |
|---|---|---|
| Eşik (1-9 + 10 / 11) | 1 260 | 295 |
| Yurt (21-24 / 31-34) | 1 980 | 440 |
| **Toplam (tek seferlik, her ırk)** | **3 240** | **735** |
| Seviye 1→10 için gereken XP | 10 700 | |
| Görevlerin payı | **%30,3** (test sınırı %25-35) | Sv. 10'a kadar av gelirinin (~6-8 bin) ~%10'u (test: < 1 500) |

Eşik'in görevleri ilk seviyelerde (1→6: 2 570 XP) daha ağır basar (~%49): yeni oyuncuyu avlaklara, demirciye ve
yurduna götüren rehber; 6→10'da (8 130 XP) görevler ~%24, av asıl kaynak. Eşyalar ekonomiyi bozmuyor: sınıf eşyaları
seviyelerinin sıradan / satılan parçaları (Eski 1. sv., Burç / Kırağı 3. sv., Sağlam 5. sv., Sınır 5. sv.), satış
fiyatı değerin yarısı. Taşlar: Eşik yolu 6 Ham Tav Taşı (~150 Akçe değer, ilk +1…+3 denemeleri), yurt boss'u 2 Saf
Tav Taşı. Tekrarlanabilirler saatte en çok bir / iki kez (50-200 XP, 20-45 Akçe): avın yanında küçük bir ek.

### 37.5 Protokol, sunucu, veritabanı

| Konu | Ne |
|---|---|
| Protokol v11 | `QUEST_LIST` 0x0B00 (girişte SKILL_BAR'dan sonra, bölge değişiminde de), `QUEST_ACCEPT_REQ` 0x0B01, `QUEST_ABANDON_REQ` 0x0B02, `QUEST_HANDIN_REQ` 0x0B03 (seçim indeksi), `QUEST_RES` 0x0B04 (18 sonuç kodu, teslimde xp / Akçe), `QUEST_UPDATE` 0x0B05; `QuestEntry` 13 bayt |
| Paylaşılan kod | `shared/game/Quests.h`: katalog + doğrulama, `QuestLog`, `checkQuestAccept`, `noteQuestKill`, `objectiveProgress` / `questReady`, `planQuestHandIn` (alınan birimler, ödül yerleşimi, BagFull / PurseFull / BadChoice), `questStatus` / `questMarkerFor`; `Npcs.h`: `NpcKind::Quest`, `tint`, `scale`, `sendsToSmith` |
| Dünya | `QuestCommand` (Check: NPC görüşte / canlı / menzil -> `QuestGrant` ya da QUEST_RES; Reward: XP + hemen kayıt; bölgeler arasında varışa eklenir), `WorldBatch::kills` (ganimet sahibi), sayaç `questGrants` |
| Oturum | `handleQuest` (sel 5/s), eşya kuyruğunda `startQuestGrant` / `startQuestAbandon`, `noteKills`, `saveQuestProgress` (oturum tikinde kayıt aralığıyla ve `leaveWorld`'de), görevler girişte eşyalardan sonra yüklenir |
| Veritabanı | `0010_quests.sql` (`character_quests`), `IAccountService::loadQuests / applyQuestWrite` (bellek ve PostgreSQL aynı). Gerçek `vedatonline` veritabanına **uygulanmadı** (müdürün işi) |
| vo_admin | `show-character`: görevler bölümü (durum, sayaçlar, bekleme); `set-quest <karakter> <görev> none|active|done [p1 p2 p3 | <saniye önce>]` (test) |
| Test kancaları | `world.testing_quest_max_active`, `world.testing_quest_kill_credit` (ikisi de varsayılan kapalı, açıkken günlüğe uyarı) |

### 37.6 İstemci

| Konu | Ne |
|---|---|
| İşaret | baş üstünde özgün, prosedürel bir **köz taşı** (kısa közlü sapın üstünde yontulmuş elmas): **amber dolu** = görev alınabilir, **içi boş kül grisi** = devam ediyor, **yeşil-altın, tikli, nabız gibi atan halka** = teslim edilebilir; 60 m'ye kadar en az 26 piksel; tüccarda dükkân işaretinin üstünde |
| Diyalog | sol tarafta (pencere düzenleyicisi), NPC adı / unvanı / selamı, görev listesi ya da görev: NPC'nin sözü, Hedefler (ilerlemeyle), Ödüller (simge + ipucu, "sınıfına göre", tıklanarak seçilen seçmeli ödül), Kabul et / Teslim et / Vazgeç / Kapat / Geri; küçük ekranda gövde kayar; uzaklaşınca (9,5 m), ölünce, bölge değişince kapanır |
| Tüccar + görev | alınabilir ya da teslim edilebilir görevi olan tüccar önce diyaloğu açar ("Alışveriş" düğmesiyle dükkâna); yoksa dükkân açılır ve altta "Görevler" düğmesi görünür |
| Görevler penceresi | **L** ya da HUD'da Yetenekler ile Tuşlar arasındaki "Görevler (L)" düğmesi (prosedürel parşömen simgesi; teslim edilebilir görev varken parlar); etkin görevler "3 / 10", seçilenin hedefleri, sözü, "Teslim: <NPC> (<bölge>)", "Bırak" (soru; varsayılan Vazgeç) |
| İzleyici | HUD'da bölge adının altında en çok 5 görev ve hedefleri ("Kül Çömezi 3/6"; biten yeşil), hazır görev "<NPC> ile konuş"; bir pencere üstüne gelince gizlenir |
| Bildirim | ekranın üst dörtte birinde 4 s: "Yeni görev: …", "Tamam: …", "Görev tamamlandı: … — <NPC> ile konuş", "Görev teslim edildi: … (+70 deneyim, +20 Akçe)" |
| Kod | yeni dosyalar: `client/world/QuestView.*` (saf, testli), `client/scenes/QuestWindows.*`, `client/scenes/WorldQuests.cpp`; mevcutlarda küçük kancalar (WorldScene, WorldShop, WorldWindows, ShopWindow, HudView, HudBar) |

### 37.7 Testler, akışlar, ekran görüntüleri

Yeni birim testleri: `tests/game/QuestTests.cpp` (yükleyicinin bütün retleri, sevk edilen görev hattı ve ekonomi
payı, kabul kontrolleri ve sırası, bekleme süresi, öldürme sayımı, hazır olma / çantada sayım, teslim planı — alınan
birimler, ödüller, BagFull, PurseFull, BadChoice —, durum ve işaretler, v11 mesajları, NPC türleri / renk / ölçek),
`tests/client/QuestViewTests.cpp` (görev defteri, bildirimler, istek temposu, diyalog girdileri, metinler,
ayrıştırıcı), `tests/server/QuestServerTests.cpp` (süreç içi sunucu: retler, konuşma / öldürme / toplama
görevleri, cevaptan önce yazım, sınır, bekleme, yeniden giriş, başarısız yazım, sel), `tests/db/PostgresQuestTests.cpp`
(0010 bir M16 veritabanına, CHECK'ler, tek işlemli yazım, ilerleme kaydı, set-quest, gerçek sunucuyla yeniden giriş ve
sunucu yeniden başlatma). Uçtan uca: botclient `quests` senaryosu (`integration.quests`, `integration.db.quests`;
`basics` / `check` / `line` aşamaları: her hedef türü, bütün retler, bırakma, yeniden giriş, sel; DB'de
show-character / set-quest ve sunucu yeniden başlatma).

Değişen eski testler: `MessagesTests` (sürüm 11), `ShopTests` (11 NPC), `UpgradeTests` (yalnız Ilgaz yükseltir),
`HudViewTests` (altı menü düğmesi: dar çubukta k 0,747), `ServerTests` / `ItemServerTests` / `ZoneServerTests` /
`TestServer.h` (girişte QUEST_LIST), `Postgres*Tests` (uygulanan göçlere 10 eklendi), botclient `merchantsOf`
(görev NPC'lerini atlıyor) ve `stats` (girişte SKILL_BAR ile CHAR_STATS arasında QUEST_LIST), RunScenario `shop`
(11 NPC).

UI akışları: `m17_quests` (1280x720, 1024x640 @150 %), `m17_line` (1920x1080, 1280x720), `m17_home` (1280x720,
1920x1080). Ekran görüntüleri `docs/screenshots/m17/`.

Son koşular (2026-11-19/20, son değişiklikten sonra): `.\build.ps1 -Test` (Debug) **726 / 726** geçti — çalışma
ağacında `config/server.local.toml` olmadığından 17 `integration.db.*` testi kendini atladı (çıkış 77); aynı
yapıyla `VEDAT_TEST_DB_URL` (yalnız `vedatonline_test`) verilerek DB testleri ayrıca: Debug **52 / 52**. Tam
`tools\dev\run-ui-flows.ps1`: 67 akış, 119 koşu, **117 geçti**; düşen tek çift eski M14 `two_clients_shop` (B'nin
yürüyüşü A'nın 8 m yakınına varmadan bitti; tek başına yeniden koşunca geçti — ara sıra). `.\build.ps1 -Config
Release -Test` **726 / 726** (aynı 17 atlama; DB testleri ayrıca Release **52 / 52**).
## 38. M17 "Kuşam" — giyilen zırh ve yeni silahlar (sanat / çizim, 2026-11-19)

Ayrıntı: `docs/CLIENT_RENDERING.md` §18, `docs/ASSET_PIPELINE.md` §4h, `docs/PROTOCOL_ITEMS.md` §2.1.

### 38.1 Ne yapıldı

* **Zırh karakterin üstünde çiziliyor** (yerel oyuncu, diğer oyuncular, karakter seçim önizlemesi). Her bakış
  (`aydor_0..2`, `kargan_0..2`) × hat (ağır / deri / bez) × biçim (`a` = Eski / Sağlam / Ayazlı, `b` = Yeminli / Korlu
  ve Eşsizler) × yuva (baş, gövde, bacak, eller, ayak) için ayrı, deri iskeletine bağlı bir parça: 180 dosya
  (`assets_src/armor/<bakış>/<hat>_<biçim>_<yuva>.glb`, `tools/blender/gen_armor.py`, tamamen prosedürel). Parçalar
  bakışın iskeletini birebir paylaşıyor (eklemler ve ters bağlama matrisleri karakter glb'sinden kopya), istemci onları
  gövdenin kemik paletiyle, gövdeden hemen sonra, her geçişte (ana + iki gölge katmanı) çiziyor
  (`render::CharacterDraw::worn`). Gövde, kollar ve ayaklar derinin üstünde aynı ağırlıklarla kayan bir kabuk; bacaklar
  her bacağın çevresine oturtulmuş tüpler + kalça eteği; başlıklar kafaya sabit. Gövdeden bir şey gizlenmiyor; pelerin,
  kürk, örgü, sakal, önlük üstte kalıyor (bilerek).
* **Hatlar ve kademeler**: ağır = parlak çelik levhalar (üst üste binen lamlar, apolet, dizlik, burun koruyuculu tolga /
  b'de sorguçlu sivri tolga, yanak siperi, gorget); deri = dikiş ve kayışlı yelek, kemer, tozluk, kulaklı başlık / b'de
  kukuleta, tek apolet, çapraz kayış; bez = cübbe + etek (b'de diz altı, çan yenler), şalvar, sarık / b'de mücevher ve
  tüylü uzun sarık, kıvrık burunlu pabuç. Kademe rengi eşyanın `tint`'i (tüm parçayı çarpıyor); b biçimi farklı siluet
  ve altın kenar.
* **15 zırh simgesi** (`armor_<hat>_<yuva>`), beş yer tutucunun yerine; kademe rengi yine `tint`. `gen_armor_icons.py`
  ve eski beş simge silindi.
* **14 yeni silah modeli** (`tools/blender/gen_forged_weapons.py`, prosedürel): sırtlı kılıç, pala, iki elli geniş kılıç,
  dalgalı hançer, kavisli hançer, flanşlı topuz, dikenli topuz, savaş çekici, sakallı balta, teber, ayin asası (ışıyan
  küre), kristal asa, kule kalkanı, kabzalı küçük kalkan; simgeleri ve önizlemeleriyle. Mevcut tutuş satırları
  (`attachments.toml`) altı bakışta da yeterli (yeni satır yok). **Kor kademesi** (62–69) sekiz yeni modele bağlandı;
  diğer altı modelin şablonu yok (sonraki eşya genişlemesine).
* **items.toml**: 84 zırh şablonunun `icon`'u hat + yuvaya, yeni `model` anahtarı giyilen parçaya bağlandı; zırhta
  `model` artık izinli, `grip` yasak.

### 38.2 Ağ (protokol sürümü artırılmadı)

`CharSummary`, `ENTITY_SPAWN` (state'ten sonra) ve `ENTITY_EQUIP` sonuna 5 × u16 zırh şablonu eklendi (10 bayt);
`ENTITY_EQUIP` artık zırh değişince de gidiyor. Sunucu: `VisibleEquipment::armor` → `EquipCommand::armor` →
`Zone::setEquipment` → `CharacterInfo::armorTemplates`; karakter listesi / yükleme (`MemoryAccountService`,
`PostgresAccountService`: yuva 2..6 için beş alt sorgu, `kCharacterColumnCount` 25 → 30).

### 38.3 Testler, akışlar, ekran görüntüleri

Yeni: `tests/assets/content/ArmorTests.cpp` (180 parçanın varlığı, iskeletin gövdeyle aynı olması, üçgen bütçesi,
yuvanın gövdedeki yeri, 15 simge, her zırh şablonunun modeli / simgesi, 14 silahın tutuş çerçevesi / bütçesi /
simgesi, her silah şablonunun modeli), `MessagesTests` (zırh alanlarının gidiş-dönüşü, boyutlar), `HuntingTests`
(zırhta `model` izinli). UI akışları: `m17_dress` (Aydor, Kargan: üç hat ön / yan / arka, b biçimleri, ağır hattın beş
kademesi, Eşsiz takım, karışık takım, koşu / saldırı / oturma / ölüm / içme / büyü), `m17_weapons` (14 model iki ırkta
elde), `m17_two_clients` (gerçek satın alma + kuşanma, öteki istemci zırhı görüyor, seçim ekranı önizlemesi), `m17_perf`
(20 hayalet). Ekran görüntüleri `docs/screenshots/m17_art/`.

### 38.4 İkinci geçiş: zırhın görünüşü (2026-11-20)

Müdürün değerlendirmesi: sistem çalışıyor ama görünüş kaba (Kargan'da topak topak ve şişkin kabuklar, krom gibi
ağır zırh, iç içe geçmeler, envanterde düz siluet). Yapılanlar:

* **Kabuk yerine vekil hacimler** (`gen_armor.py`): gövde, kollar, kalça, bacaklar, ayaklar ve çizme konçları artık
  bir eksen etrafındaki halkalardan oluşan "loft"lar. Her halkanın yarıçapı bölge derisinin açısal dilimlerdeki
  yüzdelik değerinden (kürk tutamı, toka, kese sayılmıyor), düşük dereceli Fourier uydurması (pürüzsüz, kasıtlı kesit),
  eksen boyunca yumuşatma ve derinin %95-97'si içeride kalana kadar büyütme ile bulunuyor; payı ince (ağır 1,6 cm, deri
  0,9, bez 1,0). Kürk yakanın, pelerinin, önlüğün altına sokuluyor; göğse sarkan örgü ve sakal ise zırhın içinde
  kalıyor. Gövdenin omuz üstü daralıyor (omuz uçları apoletin). Kargan'ın pul eteği kalça zırhının altında, eteğin
  ucu otomatik bulunuyor; uyluk tüpleri eteği hesaba katmıyor (balon bacak bitti). Apolet, dirsek, dizlik ve başlıklar
  yumuşatılmış kubbeler; eller önce yumuşatılan deri kabukları. Ağırlıklar en yakın derinin ağırlıkları, loft ızgarası
  üzerinde yumuşatılmış: koşu, saldırı, oturma, ölümde parçalar deriyi izliyor.
* **Malzemeler**: üç hat için küçük, tekrarlanan detay dokuları (`tools/blender/armor_textures.py`, prosedürel, özgün):
  fırçalanmış çelik, perçinli kıvrık kenar şeridi; damarlı ve dikişli panel deri, dikişli kayış; bez dokuma ve altın /
  fildişi işlemeli şerit (basamaklı baklava ve kancalar). UV'ler metre cinsinden; istemci giyilen parçaları wrap
  örnekleyiciyle çiziyor (gövde atlası clamp kalıyor). Geometri detayı: keskin basamaklı lamlar, b'de oluklar ve orta
  sırt, perçinler, tokalar, bağcıklar, çapraz kayış, deri etek parçaları, çan yen, işlemeli stol. Kademe rengi yine
  eşyanın `tint`'i; biçim b daha büyük apolet + dirseklik + gorget + yüzgeç sorguçlu tolga / uzun cübbe + stol.
* **Envanter figürü**: ekipman alanında kendi karakterin, giydiği zırh ve silahla canlı 3D
  (`client/scenes/InventoryFigure.h`, seçim ekranının önizlemesi). `scene expectinvfigure <n>`, akış `m17_invfigure`
  (1280x720, 1920x1080, 1024x640 @1,5).
* **Sıkıştırma**: glb'de renk u16, ağırlık u8, indis u16 (köşe başına 60 → 48 bayt, UV eklenmesine rağmen); parça
  başına köşe sayısı arttığı için `assets_src/armor` yine ~29 MB.
* **Performans** (`m17_perf`, Debug, 1920x1080, 20 hayalet + kendi karakter). Önce: zırhsız 71,8 fps / 163 çizim;
  zırhlı (90 parça) 68,7 fps, 252 çizim, 1,57 M üçgen, gölge 326 çizim. Sonra: zırhsız 74,4 fps / 160; zırhlı
  (85 parça) 64,0 fps, 367 çizim, 1,63 M üçgen, gölge 326 çizim (gölge geçişinde bir parçanın malzemeleri tek çizim).
  Parça başına 2-4 malzeme olduğu için ana geçişte çizim sayısı arttı; parça başına üçgen 800-5 700 (eskiden
  300-4 800), tam takım ≈ 14-17 bin.
* **Hâlâ zayıf**: Kargan gövdesi ve bez cübbe hacimli (vücudun kendisi geniş; cübbe eteği çan gibi), Kargan'ın
  kürk yakası apoletlerle iç içe; kemer keseleri ve önlük kenarları yer yer görünüyor; ağır bacak tüpleri kalın;
  oturuşta uzun cübbe ve şalvar dizde katlanıyor; perçin ve dikiş gibi ince detaylar oyun kamerasından zor seçiliyor.

### 38.5 Testler ve akışlar (ikinci geçiş)

`ArmorTests`: her parça hattının döşenen dokularını (taban rengi, normal, ORM, `armor/textures/`) ve teğetleri
taşıyor, en çok 5 malzeme. Yeni akış `m17_invfigure` (üç boyut). Son koşular: `tools\build-assets.ps1 -Config Debug`
temiz, Debug `.\build.ps1 -Test` 732/732 (17 DB testi atlandı), `tools\dev\run-ui-flows.ps1` 129/129. Ekran
görüntüleri yenilendi (`docs/screenshots/m17_art/`, aynı adlar) + `inv_figure_<boyut>.png`.

## 39. M18 "Kuşanma" — silah özellikleri, bonus satırları, takılar, eşya çeşitliliği (2026-11-20)

Sözleşme: `docs/PROTOCOL_EQUIPMENT.md` (protokol **v12**; istemci sunumu §6, Türkçe metinler orada), veritabanı
`docs/DATABASE.md` §3.10 (göç **0011_accessory_slots**). Mekanik klasik MMORPG donanımı gibi (silah gecikmesi, erim,
hasar aralığı, nitelik / can / mana / kritik / kaçınma satırları, yüzük-küpe-kolye-kemer); bütün adlar, metinler,
simgeler ve sayılar özgün (KO'dan ya da başka bir oyundan ad yok). Ekran görüntüleri: `docs/screenshots/m18/`.
İçerik bir kerelik bir betikle üretildi (`tools/content/m18_items.py`; kimlikler sabit, dosyalar elle düzenlenebilir).

### 39.1 Kararlar

| Konu | Karar |
|---|---|
| Saldırı aralığı | Silah şablonunda `speed_ms`; **sağ eldeki** silahın aralığı karakterin temel saldırı aralığı olur (sunucu `trySwing` bu aralığı bekler; hız büyüleri kısaltmaya devam eder). Silahsız: sınıfın aralığı |
| Yan yana silahlar (sidegrade) | Temel saldırı vuruşu **`saldırı × (silah aralığı / sınıf aralığı)`** — yavaş silah tek vuruşta daha çok, hızlı silah daha az vurur; savunmadan önce saniyedeki hasar silah hızından bağımsız (`saldırı / sınıf aralığı`). Fark hissi: yavaş silah savunmaya (vuruş başına çıkarılır) daha az kaybeder, hızlı silah daha sık vurur. Yetenekler kendi çarpan / bekleme süreleriyle, bu çarpanı kullanmaz. Başlangıç silahları sınıf aralığında (çarpan 1): M9'dan beri ayarlanan seviye 1 sayıları değişmedi |
| Erim | Silahta `reach` (m); yakın dövüş sınıflarında sağ eldeki silahın erimi saldırı menzili olur, sunucu +0,5 m toleransla doğrular. Büyücünün temel saldırısı 14 m'lik büyü olarak kalır (asa erimi yok) |
| Hasar aralığı | **Yapıldı**: `damage = [alt, üst]`, `alt + üst = 2 × attack` (ortalama `attack` kalır, eski sayılar anlamını korur). Mevcut formüle temiz oturdu: zar `[min − %10 orta, maks + %10 orta]` aralığında tekdüze; `min = maks` iken tam eski `attack × (0,9..1,1)`. Yetenekler de aralığı kullanır; süreli hasar ve kalkan ortalamayı |
| İki el | M8'den beri sol eli bloke ediyor (değişmedi; test edildi) |
| Bonus satırları | Her kuşanılabilir eşyada `max_hp`, `max_mp`, `crit`, `dodge` (binde) ve M11 nitelikleri. Sunucu toplar (`deriveStats`), CHAR_STATS bildirir. **İsabet** satırı yok: oyuncu saldırıları her zaman isabet ediyor, canavarlar kaçınamıyor (isabet / kaçınma yalnız canavar → oyuncu yönünde var) |
| Gereksinim | M11'in nitelik gereksinimleri; yeni üst kademelerde (Korlu 17, Ant 19) kullanılıyor, temel değerle (sınıf başlangıcı + harcanan puan). Eski şablonlara eklenmedi (eski akışlar puan harcamadan Kor eşyası kuşanıyor) |
| Yükseltme | +1…+10 saldırı / savunmayı ölçeklemeye devam eder; hasar aralığı yükseltilmiş saldırıyla kayar (genişliği aynı). **Bonus satırları ve nitelikler ölçeklenmez** (yükseltme yalnız "güç sayısı"; satırlar eşyanın kimliği). **Takılar yükseltilmez** (`NotUpgradable`): ölçeklenecek tek sayıları 0–5 savunma, +7 takı neredeyse hiçbir şey değiştirmeden en pahalı taşları yerdi |
| Takılar | `kind = "accessory"`, yuvalar `ring` / `earring` / `necklace` / `belt`; Kuşanma kabı 8 → **13 yuva** (7 Kolye — eskiden "ayrılmış" —, 8 Kemer, 9–10 Yüzük, 11–12 Küpe); karakterde çizilmez, başkalarına gönderilmez |
| Protokol | **v12**: CHAR_STATS'a 16 bayt (saldırı min / maks, erim cm, vuruş çarpanı binde, eşyalardan can / mana / kritik / kaçınma), INVENTORY en çok 41 kalem, ITEM_MOVE yuva 0..12 |
| Veritabanı | `0011_accessory_slots.sql`: `character_items_slot` CHECK'i Kuşanma 0..12. Gerçek `vedatonline` veritabanına **uygulanmadı** (müdürün işi) |
| Test kancası | `world.testing_enter_items = "171, 205, ..."` (varsayılan kapalı, açıkken günlüğe uyarı): giren karakter bu şablonlardan elinde olmayanları birer adet çantaya alır, girişten önce kaydedilir |

### 39.2 Silah aileleri

Saldırı = M15 merdiveni (`kılıç(L) ≈ 6 + 0,95·L`: Sv. 5 → 11, 17 → 22, 19 → 24) × aile çarpanı; değer de aynı merdivenle.

| Aile | Sınıf | Tutuş / yuva | Aralık | Erim | Hasar aralığı | Saldırı çarpanı | Model |
|---|---|---|---|---|---|---|---|
| Hançer | Haydut | dagger / tek el (iki elde) | 1,2 sn | 2,2 m | ±%10 | 0,64 | dagger_hunter, dagger_fang |
| **Dalgalı Hançer** (yeni) | Haydut | dagger / tek el | 1,2 sn | 2,2 m | ±%10 | 0,64 + kritik satırı | dagger_twin |
| Pala | Haydut | sword / tek el | 1,45 sn | 2,5 m | ±%18 | 0,73 | sword_soldier, sword_curved |
| Kılıç | Savaşçı | sword / sağ el | 1,6 sn | 2,5 m | ±%15 | 1,0 | sword_soldier, sword_keep, sword_ridge |
| Topuz / Gürz | Savaşçı, Şifacı | mace / sağ el | 1,7 sn | 2,4 m | ±%25 | 0,85 | mace_sun, mace_flanged |
| **Çivili Topuz** (yeni) | Savaşçı, Şifacı | mace / sağ el | 1,7 sn | 2,4 m | ±%28 | 0,85 | mace_spiked |
| **Büyük Kılıç** (yeni) | Savaşçı | axe2h / iki el | 2,0 sn | 2,8 m | ±%15 | 1,6 | sword_broad |
| Balta | Savaşçı | axe2h / iki el | 2,2 sn | 2,7 m | ±%20 | 1,6 | axe_hearth, axe_bearded |
| **Teber** (yeni) | Savaşçı | axe2h / iki el | 2,3 sn | **3,3 m** | ±%20 | 1,48 (erim karşılığı) | halberd |
| **Savaş Çekici** (yeni) | Savaşçı | axe2h / iki el | 2,4 sn | 2,6 m | ±%30 | 1,6 | hammer_war |
| Asa | Büyücü | staff / iki el | 1,8 sn | (14 m büyü) | ±%12 | 1,12 | staff_ember, staff_ritual |
| **Billur Asa** (yeni) | Büyücü | staff / iki el | 1,5 sn | (14 m büyü) | ±%12 | 1,12 | staff_crystal |
| Değnek | Şifacı, Büyücü | staff / iki el | 1,9 sn | 2,9 m | ±%15 | 1,0 | staff_frost |
| Kalkan | Savaşçı, Şifacı | sol el | — | — | — | savunma ≈ 3,4 + 0,62·L | shield_oak, shield_tower |
| **Kalkancık** (yeni) | Savaşçı, Şifacı | sol el | — | — | — | savunma ×0,6 + kaçınma (6 + L/2 binde) | shield_buckler |

Bütün 14 dövme silah modeli (M17'de altısı boştaydı) ve eski 8 model en az bir şablonda kullanılıyor (test).
Animasyon temposu: oyuncunun temel saldırı klibi aralığın %42'si kadar sürer (0,45–1,05 sn): hançer ~0,5, kılıç ~0,67,
balta ~0,92, teber ~0,97 sn (`swingSeconds`; yerel oyuncu sunucunun aralığıyla — hız büyüsü dahil —, diğer oyuncular
sağ ellerindeki silahla).

### 39.3 Yeni şablonlar (kimlik 169–303; toplam **303**)

| Kimlikler | Ne | Seviye / nadirlik | Kaynak |
|---|---|---|---|
| 169–175 | Sağlam Büyük Kılıç, Savaş Çekici, Teber, Çivili Topuz, Dalgalı Hançer, Billur Asa, Kalkancık | 5, Sıradan | Usta Berkay, Örsbaşı Gürşad; Kavruk Tırpancı / Buzul Körükçü %0,3; görev 21 / 31 (sınıfa göre) |
| 176–182 | Ayazlı … (+2 ana nitelik; Dalgalı'da kritik) | 9, Seyrek | Harman Devi / Örs Kıran %0,4; Anız Ağası / Körük Hanı %8 |
| 183–189 | Yeminli … | 13, Sıradan | Demirci Ilgaz; İs Avcısı %0,3; Cüruf Anası %6 |
| 190–196 | Korlu … (+5 / +2, **gereksinim**: Güç 22 / Dayanıklılık 16 / Çeviklik 24 / Zekâ 24) | 17, Nadir | Kor Büyücüsü %0,2; Ocak Efendisi %5 |
| 197–203 | Ant … (+6 / +3, can / mana / kritik satırı, gereksinim Güç 25 / Day. 18 / Çev. 27 / Zekâ 27) | 19, Nadir | Kor Büyücüsü %0,08; Ocak Efendisi %3 |
| 204–211 | Ant Kılıcı, Baltası, Gürzü, Hançeri, Palası, Asası, Değneği, Siperi (eski 8 ailenin Sv. 19 kademesi) | 19, Nadir | aynı |
| 212–226 | Ant zırh takımı: ağır (Dayanıklılık 20), deri (Çeviklik 20), bez (İrade 18) × 5 yuva; can / kaçınma+kritik / mana satırları | 19, Nadir | Kor Büyücüsü %0,08; Ocak Efendisi %2,5 |
| 227–291 | **Takılar**: 5 kademe (Eski 3, Sağlam 7, Ayazlı 11, Yeminli 15, Korlu 19) × 13 (Kaya / Rüzgâr / Köz / Şafak / Meşe Yüzüğü; Kıvılcım / Çiy / Yankı Küpesi; Kurt Dişi / Ay Taşı Kolye, Köz Muskası; Örme Kemer, Deri Kuşak) | Eski / Sağlam Sıradan, Ayazlı / Yeminli Seyrek, Korlu Nadir | Eski: Sürmeli Bacı + Sv. 1–4 avlaklar %0,2–0,3; Sağlam: Rahibe Aksun, Kül Anası Tamar + Sv. 5–7 %0,3, görev 23 / 33 seçmeli; Ayazlı: İs Avcısı %0,3, Sv. 10 bossları %5; Yeminli: Cüruf Zırhlısı %0,3, Cüruf Anası %5; Korlu: Kor Büyücüsü %0,15, Ocak Efendisi %4 |
| 292–293 | Kor Şerbeti (can 600), Kırağı Damlası (mana 340) | — | üç şifacı; Cüruf Zırhlısı / Kor Büyücüsü / Cüruf Anası |
| 294–303 | **10 Eşsiz**: Bekçinin Kor Halkası, Bekçi Kalkancığı (Kor Bekçisi); Ağanın Başak Kuşağı, Ağanın Dalgalı Hançeri (Anız Ağası); Hanın Buz Küpesi, Han Billuru (Körük Hanı); Ananın Cüruf Gerdanlığı, Döküm Çekici (Cüruf Anası); Ocak Mührü, Efendinin Teberi (Ocak Efendisi) | boss seviyesi | bosslar %3–4 (bir boss 6–8 Eşsiz; öldürme başına en az biri ~%20–25) |

Takı satırları kademe t (1..5) ile: yüzük ana nitelik +t (Meşe: Dayanıklılık +t, can +5t); Kıvılcım küpe kritik 3t binde
+ Çeviklik; Çiy küpe mana 10t + İrade; Yankı küpe kaçınma 3t + Dayanıklılık; Kurt Dişi kolye can 15t; Ay Taşı kolye mana
15t; Köz Muskası kritik 2t + Güç; Örme Kemer savunma t + can 5t; Deri Kuşak kaçınma 3t + savunma. Değer: Eski 34–44,
Sağlam 95–125, Ayazlı 205–265, Yeminli 340–440, Korlu 545–705 Akçe. Simgeler 8 özgün prosedürel model
(`tools/blender/gen_accessory_icons.py`, başsız Blender; `acc_ring_band/signet`, `acc_earring_hoop/drop`,
`acc_necklace_chain/torc`, `acc_belt_leather/plated`), kademe + satır rengiyle `tint`.

Eski eşyalara eklenenler: bütün silahlara aile hızı / erimi / hasar aralığı (ortalama saldırı değişmedi); Ayazlı / Yeminli /
Korlu zırhlarına satırlar (ağır: can 10–40; deri: kaçınma 3–10, eldivende kritik; bez: mana 10–40); Eşsiz dengesi için
Son Kıvılcım kritik %1, Bekçi Kalkancığı savunma 7, Ant Palası Güç +2 / Çeviklik +5. Ganimet tablosu sınırı 32 → 80 satır
(Kor Büyücüsü 78, Ocak Efendisi 75 satır; bir zar yine en çok 8 eşya tutar). Dükkânlar: şifacılar 6 → 21 kalem,
Berkay 24 → 31, Gürşad / Ilgaz 25 → 32 (sınır 32).

### 39.4 Denge

Hesap (`EquipmentTests` "[economy]", `VO_ECONOMY_TABLE=1` tabloyu basar): seviyesinde 2/3 ana nitelik + 1/3 Dayanıklılık
harcamış karakter, o seviyenin silahı (tek el + sınıfın en iyi kalkanı, haydut iki elde aynı silah, iki el yalnız) ve
kademesinin zırh takımı (savaşçı / şifacı ağır, haydut deri, büyücü bez); **temel saldırının** saniyedeki hasarı (kritik
dahil, yetenek yok), o seviyenin ±1'indeki en sert sıradan canavarın savunmasına karşı.

| Sınıf | Sv. 1 | Sv. 5 | Sv. 9 | Sv. 13 | Sv. 17 | Sv. 19 |
|---|---|---|---|---|---|---|
| Canavar savunması | 6 | 18 | 29 | 50 | 59 | 39 |
| Savaşçı, tek el + kalkan (hasar/sn · savunma · can) | 9,6 · 12 · 90 | 18,3–20,5 · 24 · 210 | 30,5 · 32 · 355 | 37,5 · 41 · 465 | 57,2–66,9 · 61 · 680–705 | 72,2–86,2 · 69 · 840 |
| Savaşçı, iki el | 14,0 · 8 · 90 | 28,7–31,4 · 16 · 195 | 48,0–51,5 · 27 · 345 | 63,2–66,8 · 34 · 465 | 96,2–100,8 · 46 · 665 | 117,1–121,7 · 52 · 785 |
| Haydut, iki elde | 13,4 · 7 · 98 | 32,9–38,0 · 12 · 190 | 70,1 · 19 · 282 | 80,5 · 27 · 374 | 151,0–173,4 · 36 · 476 | 191,9–218,7 · 41 · 522 |
| Büyücü (asa / değnek / billur) | 8,5 · 7 · 90 | 17,9–19,4 · 12 · 166 | 33,4 · 18 · 242 | 38,7 · 25 · 318 | 61,0–69,2 · 29 · 394 | 82,3–92,0 · 35 · 432 |
| Şifacı, topuz + kalkan / değnek | 7,8 · 8 · 95 | 15,7–18,1 · 16–23 · 188–203 | 26,4 · 32 · 336 | 32,0 · 41 · 434 | 49,7–55,2 · 46–62 · 612–662 | 63,6–70,3 · 51–70 · 701–806 |

(Sv. 9 ve 13'te yalnız yeni ailelerin o seviyedeki silahı var; eski kademeler 8 / 11 / 14'te.) Testle korunan bantlar:
aynı seviye ve aynı el düzenindeki aileler birbirinin **%15'i** içinde (savaşçı / şifacının tek el + kalkan grubunda **%20**:
topuzlar Dayanıklılık / İrade satırı için hasardan ödün veriyor — Kor Gürzü Kor Kılıcı'nın %85'i, Ant Gürzü Ant Kılıcı'nın
%84'ü); savaşçının iki eli tek el + kalkandan **%10–60** fazla (ölçülen 1,45–1,53: M15'te 1,44; yavaş vuruş savunmaya daha
az kaybettiği için %5–8 arttı). Vuruş hissi: Korlu Savaş Çekici tek vuruşta Kor Kılıcı'nın ~2 katı (test: ≥1,5×). Hız
sırası testte: hançer < pala < kılıç < topuz < büyük kılıç < balta < teber < çekiç; erim: teber ve değnek > kılıç > hançer.
Başlangıç silahları sınıf aralığında olduğundan M9 / M11 ayar testleri (seviye 1 sayıları, Kor Bekçisi dövüşü) değişmeden
geçti. Diğer korunanlar: her sınıfın her 4 seviyelik bantta bir silahı, beş zırh yuvası ve dört takı türü (Sv. 1–4'te
Eski takılar Sv. 3); gereksinimler o seviyenin puanlarının yarısıyla karşılanabilir ve hiçbiri başlangıç değeriyle karşılanmaz;
her şablonun bir kaynağı var (başlangıç takımı, dükkân, ganimet, görev), M18 eşyalarının hepsi ganimetten ya da dükkândan;
dükkânlar yalnız Sıradan M18 eşyası satar; Eşsizler seviyelerine kadar aynı yuvadaki her eşyadan güçlü (güç ölçüsüne
bonus satırları eklendi: 10 can / mana ya da 5 binde kritik / kaçınma = yarım nitelik puanı).

### 39.5 Protokol, sunucu, veritabanı, araçlar

| Konu | Ne |
|---|---|
| Paylaşılan kod | `Items.h/.cpp`: yeni anahtarlar, `ItemKind::Accessory`, `SlotRule::Ring/Earring/Necklace/Belt`, `EquipSlot` 13 yuva, `checkEquip` / `quickMoveTarget` takı kuralları; `Combat.h/.cpp`: `EquipBonus` (aralık, hız, erim, satırlar), `Stats::attackMin/Max`, `swingFactor`, `bonus*`, `computeDamage` aralıklı, `basicDamagePerSecond`; `Loot.h` 80 satır |
| Sunucu | `trySwing` aralık + `swingFactor` ile, yetenekler aralıkla zar atar; CHAR_STATS v12 alanları; ekipmanla en çok can değişince gözlemcilere ENTITY_STATS; `world.testing_enter_items` (Session, girişten önce tek `applyItemWrite`) |
| Protokol v12 | CHAR_STATS 48 → 64 bayt; `kItemEquipmentSlots` 13; `kMaxInventoryEntries` 41 |
| Veritabanı | `0011_accessory_slots.sql` (CHECK yenilendi); bellek ve PostgreSQL hizmetinde kod değişmedi (yuva aralığı paylaşılan `isValidPlace`'ten) |
| vo_admin | `show-character`: 13 yuvanın hepsi (`equip ring1 …`, boşsa `-`) ve `totals:` satırı (can / mana eşya payıyla, saldırı aralığıyla, savunma, aralık, vuruş çarpanı, erim, kritik / kaçınma eşya payıyla) |
| Botclient | `equipment` senaryosu (`integration.equipment`, `integration.db.equipment`) — 39.7 |

### 39.6 İstemci

| Konu | Ne |
|---|---|
| Envanter | sağ sütunda Kemer (eski soluk "Takı" yuvası), ellerin altında 40'lık beş yuva: Kolye, Yüzük, Yüzük, Küpe, Küpe; çantanın altında altın toplam satırı "Saldırı 75 (70–81) · Savunma 28 · Hız 2,3 sn"; çift / sağ tık yüzüğü / küpeyi ilk boş yuvasına koyar |
| İpucu | "Saldırı 13–19", "Hız: 2,3 sn (yavaş)", "Erim: 3,3 m"; bonus satırları kendi renginde (açık mavi `#8CC4F2`): nitelikler, "Can +75", "Mana +30", "Kritik Vuruş +%1,5", "Kaçınma +%0,9"; karşılanmayan gereksinim / sınıf / seviye kırmızı; takılarda "Takılar yükseltilemez." |
| Künye | yeni satırlar **Temel Vuruş** (aralık × vuruş çarpanı; büyücüde Temel Büyü) ve **Erim**; eşya payı değerin solunda bonus renginde "Can (+145) 665", "Kritik Vuruş (+%1,5) %8,0"; satır yüksekliği 23 → 22, pencere 388 → 412 |
| Vuruş temposu | `Animator::playAttack(clip, maxSeconds, minSeconds)`: yavaş silah klibi yavaşlatılır (en çok 0,6×), hızlı silah hızlandırılır; durumlar `swingsN`, `swingfast` / `swingmedium` / `swingslow`; `scene combatlog` tempoyu ve iki vuruş arasındaki süreyi yazar |
| Otomatik saldırı | kovalamada sunucunun erimi (CHAR_STATS) kullanılır |
| Otomasyon | yerler `necklace belt ring1 ring2 earring1 earring2`, `scene expectequipslot`, `expectderived hit|reach`, `inv.summary` |

### 39.7 Testler, akışlar, ekran görüntüleri

Yeni birim testleri `tests/game/EquipmentTests.cpp` (yeni anahtarların ayrıştırılması ve bütün retleri, takı yuvaları ve hızlı
taşıma, türetilen değerler — aralık, çarpan, erim, satırlar, sınırlar, büyücü, hız büyüsü, sol el —, aralıklı hasar formülü,
v12 CHAR_STATS, içerik: her silahın hızı / erimi, aile temposu, 22 modelin hepsi, iki el kuralı, **sidegrade bantları**,
her sınıf / bantta silah + tam zırh + takılar, gereksinimlerin karşılanabilirliği, her şablonun kaynağı, yükseltmede satırların
sabit kalması, denge tablosu); sunucu `tests/server/EquipmentServerTests.cpp` (CHAR_STATS ve sunucunun vuruş zamanları
kılıçta 1600, baltada 2200 ms; teber 3,5 m'den vurur, kılıç `OutOfRange`; takılar yuvalarında, satırları CHAR_STATS'ta, yeniden
girişte aynı; `AttributeTooLow`); fuzz: ITEM_MOVE_REQ Kuşanma yuvaları 0..15. Değişen eski testler: `MessagesTests` (v12,
64 bayt, 41 kalem), `StatsViewTests` (satırlar, bonus rengi), `ItemsTests` (13 yuva, gereksinimler yalnız üst kademelerde),
`LootTests` / `HuntingTests` (303 şablon, satır sınırı 80, Kor Bekçisi 12 satır, Eşsiz güç ölçüsü), `ShopTests` (şifacılar takı
satar), `UpgradeViewTests` (aralıklı ipucu), `CombatTests` (`EquipBonus` alanları), `QuestTests` (sürüm), `Postgres*Tests`
(uygulanan göçlere 11), `WeaponTests` (takıların simgesi var, modeli yok), `items.voscript` ("Saldırı 8–12").

Botclient `equipment` (`RunScenario.ps1 -Scenario equipment`): **gear** (sunucu `testing_enter_level = 19`,
`testing_enter_items`): v12 CHAR_STATS paylaşılan formüllerle aynı; Sağlam Teber 2300 ms / 3,3 m / ×1,438, kalkan çantaya;
Ant Baltası `AttributeTooLow`, 13 Güç puanından sonra Ok; yüzük küpe yuvasına `WrongSlot`; iki yüzük + küpe + kolye + kemer
kuşanılır, satırlar CHAR_STATS'ta (can +145, kritik +15 binde), yeniden girişte aynı yuvalar / toplamlar. **check** (yalnız
`-Database`, `vo_admin show-character` — 13 yuva, `totals:` — ve sunucu yeniden başlatıldıktan sonra): aynı donanım.
**swing** (kancasız): seviye 1 savaşçı Kül Çömezi'ne karşı kılıçla 1600 ms, baltayla 2200 ms aralıkla vurur (sunucu zamanı),
balta vuruşu ortalamada kılıcınkinin ≥1,3 katı.

UI akışları: `m18_gear` (1280x720, 1920x1080, 1024x640 @150 %; `EnterLevel = 19`, `EnterItems`): boş ve dolu takı yuvaları,
`WrongSlot` iletisi, sağ tıkla ikinci yüzük / küpe, toplam satırı, teber / kolye / küpe / Korlu çekiç (kırmızı gereksinim) /
Eşsiz hançer (kırmızı sınıf) ipuçları, Ant Baltası reddi, Künye (Temel Vuruş, Erim, "(+125)"), 13 Güç sonrası Ant Baltası ve
Künye; `m18_swing` (warrior: kılıç ve balta vuruş dizileri, `swingmedium` → `swingslow`; rogue: hançerler `swingfast`).
Ekran görüntüleri `docs/screenshots/m18/` (`<akış>_<koşu>_<ad>.png`).

### 39.8 Son koşular

Bütün koşular `VEDAT_TEST_DB_URL=skip` ile (veritabanı bağlantısı yok). Son kod değişikliğinden (e3d1b29, botclient
`stats` senaryosunun hasar aralığı kontrolü) sonra: Debug `.\build.ps1 -Test` **750 / 750** geçti (63 test atlandı:
45 `Postgres*` / `World + PostgreSQL` birim testi ve 18 `integration.db.*`; ~38 dk). Release `.\build.ps1 -Config Release
-Test` **750 / 750** (aynı 63 atlama; derleme uyarısız; ~36 dk). Tam `tools\dev\run-ui-flows.ps1` (son ürün kodu değişikliğinden
sonra, botclient düzeltmesinden önce): **116 / 117** koşu; düşen `fix1_windows` (envanter 46 px uzadığı için betik pencereyi
eski başlık yüksekliğinden sürüklüyordu) betik düzeltilip tek başına yeniden koşturuldu: 1 / 1. Bu düzeltmeden sonra tek
parça bir tam akış koşusu yapılmadı. Ara koşularda yakalananlar: ilk Release koşusunda `integration.stats` (botclient hâlâ
tek sayılı hasar aralığı bekliyordu — rastlantıyla Debug'da geçmişti; düzeltildi) ve `integration.equipment` (sunucunun
tamponlu günlüğünde kaybolabilen INFO satırına bakıyordu; kontrol bota bırakıldı).

### 39.9 Bilinen eksikler / zayıf noktalar

- **Veritabanı testleri bu çalışma ağacında koşulmadı**: koşularda `VEDAT_TEST_DB_URL=skip` (bağlantı dizesi verilmedi).
  Dikkat: worktree ana kopyanın içinde olduğundan `tests/db/TestDb.cpp` `config/server.local.toml`'u yukarı doğru arayıp ana
  kopyanınkini bulabiliyor (ilk deneme koşusunda `Postgres*` birim testleri oradaki test bağlantısıyla koştu — kendi
  `vo_test_*` şemalarında; bağlantı dizesine bakılmadı); sonraki bütün koşular `skip` ile. `integration.db.equipment`,
  `PostgresItemTests` (yuva 7 = Kolye) ve göç 0011 **müdürün `vedatonline_test` koşusunu bekliyor**.
- Haydudun temel saldırı hasarı savaşçınınkinin ~1,6–1,8 katı (iki elde iki silahın saldırısı toplanıyor, M15'ten beri; hançer
  hızı sınıf aralığında olduğu için M18 değiştirmedi). Büyücünün temel saldırısı zayıf (yetenekleri asıl hasar). Sınıflar arası
  denge bu kilometre taşının işi değildi; dokunulmadı.
- Topuzlar savaşçı için kılıçtan %15–17 düşük hasar veriyor (satırları Şifacıya göre); bant bu yüzden %20.
- İki el / tek el oranı 1,53'e çıktı (yavaş vuruş savunmaya daha az kaybediyor): iki el kullanan savaşçı ~%5–8 güçlendi.
- Gereksinimler yalnız yeni Korlu / Ant kademelerinde; eski Kor (17) silahları ve Eşsizler gereksinimsiz (eski akışlar puan
  harcamadan kuşanıyor). Tutarsız görünebilir.
- 65 takı 8 simgeyi renkle paylaşıyor; yeni silah aileleri dövme modelleri renkle paylaşıyor; aynı model farklı kademede yalnız
  renk farkıyla.
- Vuruş hissi yalnız klibin oynatma hızı: her aileye özgü saldırı animasyonu yok (teber de kılıç klibini oynatıyor, yalnız
  yavaş). Diğer oyuncuların temposu silah şablonundan (hız büyüsünü görmez).
- `world.testing_enter_level` ile yükseltilen karakterin oturumu seviyeyi ancak yeniden girişte biliyor (M15'ten bilinen):
  botclient ve `m18_gear` bir kez çıkıp giriyor.
- Erimin sunucu doğrulaması süreç içi sunucu testinde sabit bir kuklayla sınanıyor; botclient yalnız CHAR_STATS'taki erimi ve
  vuruş aralığını gerçek dünyada sınıyor (hareket eden canavarla santimetre ölçümü güvenilir değil).
- Denge yalnız hesapla ve kancalı akışlarla denendi; gerçek bir 1→20 oyunu oynanmadı. Ekonomi testi geçti ama yeni dükkân
  kalemleri (takılar, Yeminli aileler) satın alınabilir donanım toplamını büyüttü.
- Ganimet tabloları uzadı (78 satıra kadar): bir zar yine en çok 8 eşya; düşük şanslı satırlar sona eklendiği için eski
  satırların zar sırası değişmedi.
