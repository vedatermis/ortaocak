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