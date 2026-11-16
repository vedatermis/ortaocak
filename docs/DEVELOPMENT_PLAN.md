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
